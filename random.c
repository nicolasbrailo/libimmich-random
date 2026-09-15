// For getrandom
#define _GNU_SOURCE

#include "immich.h"
#include "client.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

// Synchronisation: `lock` serialises immich_get_random_album_picture(), which
// is the only function that touches the fields below it (and the network).
// immich_random_album_picture_refresh() only sets `stale`, an atomic, so it
// never waits for a get() that is blocked on the network.
struct immich_random_album_picture {
  struct immich_client *client; // Not owned
  size_t max_pictures;          // 0: no limit
  unsigned percent;             // 1-100

  atomic_bool stale; // The album list needs to be (re-)fetched
  pthread_mutex_t lock;

  // Protected by lock
  uint64_t rng;
  bool have_albums;
  struct immich_album_list albums;
  // Indices into albums.items (only albums with assets), in the random order
  // they are visited this round. order_pos is the next one to visit.
  size_t *order;
  size_t order_len;
  size_t order_pos;
  char last_album_id[IMMICH_ID_SIZE];
  // Picture IDs sampled from the current album. sample_pos is the next one to
  // return.
  char (*sample)[IMMICH_ID_SIZE];
  size_t sample_len;
  size_t sample_pos;
};

// splitmix64: small, fast and good enough to sample pictures and shuffle albums
static uint64_t next_random(struct immich_random_album_picture *r) {
  uint64_t z = (r->rng += 0x9e3779b97f4a7c15);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

// Returns a uniformly distributed number in [0, n). n must be > 0.
static size_t random_below(struct immich_random_album_picture *r, size_t n) {
  // Reject the top values that would make a plain modulo biased
  uint64_t limit = UINT64_MAX - UINT64_MAX % n;
  uint64_t x;
  do {
    x = next_random(r);
  } while (x >= limit);
  return (size_t)(x % n);
}

static void swap_size(size_t *a, size_t *b) {
  size_t tmp = *a;
  *a = *b;
  *b = tmp;
}

struct immich_random_album_picture *
immich_random_album_picture_new(struct immich_client *c, size_t max_pictures,
                                unsigned percent) {
  if (percent > 100) {
    fprintf(stderr, "Invalid percentage: %u\n", percent);
    return NULL;
  }

  struct immich_random_album_picture *r = calloc(1, sizeof(*r));
  if (!r) {
    return NULL;
  }
  if (pthread_mutex_init(&r->lock, NULL) != 0) {
    free(r);
    return NULL;
  }

  r->client = c;
  r->max_pictures = max_pictures;
  r->percent = percent ? percent : 100;
  atomic_init(&r->stale, true);
  if (getrandom(&r->rng, sizeof(r->rng), 0) != (ssize_t)sizeof(r->rng)) {
    r->rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)r;
  }
  return r;
}

void immich_random_album_picture_free(struct immich_random_album_picture *r) {
  if (!r) {
    return;
  }
  pthread_mutex_destroy(&r->lock);
  immich_album_list_free(&r->albums);
  free(r->order);
  free(r->sample);
  free(r);
}

void immich_random_album_picture_refresh(struct immich_random_album_picture *r) {
  atomic_store(&r->stale, true);
}

// Fetches the album list if it's stale (always the case on first use). If a
// re-fetch fails, keeps the old list and leaves it stale so the fetch is
// retried next time. Returns -1 only if there is no list at all.
static int update_albums(struct immich_random_album_picture *r) {
  if (!atomic_exchange(&r->stale, false)) {
    return 0;
  }

  struct immich_album_list albums;
  size_t *order = NULL;
  if (immich_list_albums(r->client, &albums) != 0) {
    goto fail;
  }
  if (albums.count > 0) {
    order = malloc(albums.count * sizeof(*order));
    if (!order) {
      immich_album_list_free(&albums);
      goto fail;
    }
  }

  immich_album_list_free(&r->albums);
  free(r->order);
  r->albums = albums;
  r->order = order;
  r->order_len = 0;
  for (size_t i = 0; i < albums.count; i++) {
    if (albums.items[i].asset_count > 0) {
      r->order[r->order_len++] = i;
    }
  }
  r->order_pos = r->order_len; // Makes the next pick start a new round
  r->have_albums = true;
  return 0;

fail:
  atomic_store(&r->stale, true);
  if (r->have_albums) {
    fprintf(stderr, "Using the previous album list\n");
    return 0;
  }
  return -1;
}

// Starts a new round over all albums, in a random order that doesn't begin
// with the album that was just shown
static void shuffle_albums(struct immich_random_album_picture *r) {
  for (size_t i = r->order_len; i > 1; i--) {
    swap_size(&r->order[i - 1], &r->order[random_below(r, i)]);
  }
  if (r->order_len > 1 &&
      strcmp(r->albums.items[r->order[0]].id, r->last_album_id) == 0) {
    swap_size(&r->order[0], &r->order[1]);
  }
  r->order_pos = 0;
}

// How many of an album's n pictures to show. The percentage is applied first
// (rounded, but at least 1 so small albums aren't skipped), then the cap.
// Sampling k pictures out of a random sample of the album is the same as
// sampling k out of the whole album, so one sampling pass is enough.
static size_t sample_size(const struct immich_random_album_picture *r,
                          size_t n) {
  size_t k = n;
  if (r->percent < 100) {
    k = (n * r->percent + 50) / 100;
    if (k == 0 && n > 0) {
      k = 1;
    }
  }
  if (r->max_pictures > 0 && k > r->max_pictures) {
    k = r->max_pictures;
  }
  return k;
}

// Replaces the current sample with a new one from album_id. The sample may be
// empty if the album has no images. Returns -1 on error.
static int sample_album(struct immich_random_album_picture *r,
                        const char *album_id) {
  struct immich_picture_list pics;
  if (immich_list_album_pictures(r->client, album_id, &pics) != 0) {
    return -1;
  }

  size_t k = sample_size(r, pics.count);
  char (*sample)[IMMICH_ID_SIZE] = NULL;
  if (k > 0) {
    sample = malloc(k * sizeof(*sample));
    if (!sample) {
      immich_picture_list_free(&pics);
      return -1;
    }
  }

  // Selection sampling (Knuth's Algorithm S): walk the album once and keep
  // each picture with probability (still needed) / (still left). Every subset
  // of k pictures is equally likely, and the sample keeps the album's order.
  // Once still needed == still left the probability is 1, so it always ends
  // with exactly k pictures.
  size_t selected = 0;
  for (size_t i = 0; i < pics.count && selected < k; i++) {
    if (random_below(r, pics.count - i) >= k - selected) {
      continue;
    }

    // Copying an ID of the wrong length would truncate it into a different
    // ID. That can only happen if Immich changes its ID format, so say so.
    const char *id = pics.items[i].id;
    if (!immich_is_valid_id(id)) {
      fprintf(stderr,
              "Album %s: picture has an unexpected ID \"%s\" (expected a "
              "UUID). Has the Immich ID format changed?\n",
              album_id, id);
      free(sample);
      immich_picture_list_free(&pics);
      return -1;
    }
    memcpy(sample[selected++], id, IMMICH_ID_SIZE);
  }
  immich_picture_list_free(&pics);

  free(r->sample);
  r->sample = sample;
  r->sample_len = k;
  r->sample_pos = 0;
  return 0;
}

// Moves on to the next album that has images and samples it
static int next_album(struct immich_random_album_picture *r) {
  if (update_albums(r) != 0) {
    return -1;
  }

  // Albums with only videos yield no images. Try each album at most once, so
  // this gives up instead of looping forever if none has any.
  for (size_t tries = 0; tries < r->order_len; tries++) {
    if (r->order_pos == r->order_len) {
      shuffle_albums(r);
    }
    const struct immich_album *a = &r->albums.items[r->order[r->order_pos++]];
    if (!immich_is_valid_id(a->id)) {
      fprintf(stderr,
              "Album \"%s\" has an unexpected ID \"%s\" (expected a UUID). "
              "Has the Immich ID format changed?\n",
              a->name, a->id);
      return -1;
    }
    memcpy(r->last_album_id, a->id, IMMICH_ID_SIZE);
    if (sample_album(r, a->id) != 0) {
      return -1;
    }
    if (r->sample_len > 0) {
      return 0;
    }
  }

  fprintf(stderr, "No album has any pictures\n");
  return -1;
}

int immich_get_random_album_picture(struct immich_random_album_picture *r,
                                    char id[IMMICH_ID_SIZE]) {
  pthread_mutex_lock(&r->lock);
  int ret = 0;
  if (r->sample_pos == r->sample_len) {
    ret = next_album(r);
  }
  if (ret == 0) {
    memcpy(id, r->sample[r->sample_pos++], IMMICH_ID_SIZE);
  }
  pthread_mutex_unlock(&r->lock);
  return ret;
}
