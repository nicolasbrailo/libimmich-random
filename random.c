// For getrandom
#define _GNU_SOURCE

#include "client.h"
#include "immich.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

// An immich_album_filter that owns its strings. A NULL string is "no
// constraint", which is also what an empty one is turned into, so that
// "no patterns" has a single representation.
struct filter_strings {
  char *name;
  char *exclude;
  unsigned from_year;
  unsigned to_year;
};

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

  // A filter set from another thread, waiting to be picked up. `lock` can't
  // guard it: it's held for the whole of a fetch, and a setter must not wait
  // on the network. filter_lock is only ever held long enough to copy a
  // couple of strings.
  atomic_bool filter_changed;
  pthread_mutex_t filter_lock;
  struct filter_strings pending_filter; // Protected by filter_lock

  // The rotation as last built, for immich_random_album_picture_status().
  // Written under `lock`, read from any thread, so atomic.
  //
  // The verdict is derived where it is written, not where it is read, and
  // kept in ONE atomic. Deriving it from several would let a reader that
  // interleaves with a rebuild combine two rotations -- pairing "the server
  // has albums with pictures" from the old one with "none of them is in the
  // rotation" from the new one reads as "your filter matches nothing", which
  // would blame a filter for an album list that just came back empty.
  atomic_int selection;   // An enum immich_album_selection
  atomic_size_t n_albums; // Albums the server returned
  atomic_size_t n_kept;   // ...of those, how many are in the rotation

  // Protected by lock
  uint64_t rng;
  struct filter_strings filter; // The filter currently in force
  bool warned_empty;            // Already said why nothing can be picked
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

static void filter_strings_free(struct filter_strings *f) {
  free(f->name);
  free(f->exclude);
  f->name = NULL;
  f->exclude = NULL;
  f->from_year = 0;
  f->to_year = 0;
}

// Replaces what dst holds with a copy of src (a NULL src clears it). Returns
// -1, leaving dst untouched, if out of memory.
static int filter_strings_set(struct filter_strings *dst,
                              const struct immich_album_filter *src) {
  char *name = NULL;
  char *exclude = NULL;
  if (src && src->name && src->name[0]) {
    name = strdup(src->name);
    if (!name) {
      return -1;
    }
  }
  if (src && src->exclude && src->exclude[0]) {
    exclude = strdup(src->exclude);
    if (!exclude) {
      free(name);
      return -1;
    }
  }

  filter_strings_free(dst);
  dst->name = name;
  dst->exclude = exclude;
  dst->from_year = src ? src->from_year : 0;
  dst->to_year = src ? src->to_year : 0;
  return 0;
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
  if (pthread_mutex_init(&r->filter_lock, NULL) != 0) {
    pthread_mutex_destroy(&r->lock);
    free(r);
    return NULL;
  }

  r->client = c;
  r->max_pictures = max_pictures;
  r->percent = percent ? percent : 100;
  atomic_init(&r->stale, true);
  atomic_init(&r->filter_changed, false);
  atomic_init(&r->selection, IMMICH_ALBUMS_NONE_KNOWN);
  atomic_init(&r->n_albums, 0);
  atomic_init(&r->n_kept, 0);
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
  pthread_mutex_destroy(&r->filter_lock);
  filter_strings_free(&r->filter);
  filter_strings_free(&r->pending_filter);
  immich_album_list_free(&r->albums);
  free(r->order);
  free(r->sample);
  free(r);
}

void immich_random_album_picture_refresh(
    struct immich_random_album_picture *r) {
  atomic_store(&r->stale, true);
}

// (Re-)selects the albums in the rotation from the cached album list, applying
// the filter. Never touches the network, so a filter change costs no request.
// r->order is sized for the whole list, so this always fits. Must be called
// with lock held.
static void rebuild_order(struct immich_random_album_picture *r) {
  const struct immich_album_filter f = {
      .name = r->filter.name,
      .exclude = r->filter.exclude,
      .from_year = r->filter.from_year,
      .to_year = r->filter.to_year,
  };

  size_t with_assets = 0;
  r->order_len = 0;
  for (size_t i = 0; i < r->albums.count; i++) {
    if (r->albums.items[i].asset_count > 0) {
      with_assets++;
    }
    if (immich_album_filter_match(&f, &r->albums.items[i])) {
      r->order[r->order_len++] = i;
    }
  }
  r->order_pos = r->order_len; // Makes the next pick start a new round
  r->warned_empty = false;     // A new selection is worth complaining about

  enum immich_album_selection sel;
  if (r->order_len > 0) {
    sel = IMMICH_ALBUMS_OK;
  } else if (with_assets == 0) {
    sel = IMMICH_ALBUMS_NONE_WITH_PICTURES;
  } else {
    sel = IMMICH_ALBUMS_NONE_MATCH_FILTER;
  }

  // The counts go first: a reader loads the verdict before them, so one that
  // sees this rotation's verdict cannot then read counts from before it.
  atomic_store(&r->n_albums, r->albums.count);
  atomic_store(&r->n_kept, r->order_len);
  atomic_store(&r->selection, (int)sel);
}

// Picks up a filter set from another thread. Must be called with lock held.
static void apply_pending_filter(struct immich_random_album_picture *r) {
  if (!atomic_exchange(&r->filter_changed, false)) {
    return;
  }

  pthread_mutex_lock(&r->filter_lock);
  const struct immich_album_filter pending = {
      .name = r->pending_filter.name,
      .exclude = r->pending_filter.exclude,
      .from_year = r->pending_filter.from_year,
      .to_year = r->pending_filter.to_year,
  };
  int rc = filter_strings_set(&r->filter, &pending);
  pthread_mutex_unlock(&r->filter_lock);
  if (rc != 0) {
    // The old filter stays in force, which is the safe direction: it shows
    // pictures the user may not have asked for rather than none at all.
    fprintf(stderr,
            "Out of memory applying the album filter; keeping the previous "
            "one\n");
    return;
  }

  // Whatever is left of the current album's sample may come from an album the
  // new filter excludes
  r->sample_pos = r->sample_len;
  if (r->have_albums) {
    rebuild_order(r);
  }
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
  r->have_albums = true;
  rebuild_order(r);
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
      r->warned_empty = false;
      return 0;
    }
  }

  // Said once per selection rather than once per call: callers typically
  // retry a failed pick every couple of seconds, and the answer can't change
  // until the album list or the filter does.
  if (!r->warned_empty) {
    r->warned_empty = true;
    if (atomic_load(&r->selection) == IMMICH_ALBUMS_NONE_MATCH_FILTER) {
      fprintf(stderr, "No album matches the album filter\n");
    } else {
      // Either the server has nothing, or every album in the rotation holds
      // only videos -- which the loop above discovers one album at a time
      fprintf(stderr, "No album has any pictures\n");
    }
  }
  return -1;
}

int immich_get_random_album_picture(struct immich_random_album_picture *r,
                                    char id[IMMICH_ID_SIZE]) {
  pthread_mutex_lock(&r->lock);
  apply_pending_filter(r);
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

int immich_random_album_picture_set_filter(
    struct immich_random_album_picture *r,
    const struct immich_album_filter *f) {
  if (f) {
    if (f->from_year > 9999 || f->to_year > 9999) {
      fprintf(stderr, "Album filter: years must be 0..9999, got %u..%u\n",
              f->from_year, f->to_year);
      return -1;
    }
    if (f->from_year != 0 && f->to_year != 0 && f->from_year > f->to_year) {
      fprintf(stderr,
              "Album filter: from_year %u is after to_year %u. The year test "
              "is an overlap test, so this would select the albums straddling "
              "the boundary rather than nothing\n",
              f->from_year, f->to_year);
      return -1;
    }
  }

  pthread_mutex_lock(&r->filter_lock);
  int rc = filter_strings_set(&r->pending_filter, f);
  pthread_mutex_unlock(&r->filter_lock);
  if (rc != 0) {
    fprintf(stderr, "Album filter: out of memory\n");
    return -1;
  }

  atomic_store(&r->filter_changed, true);
  return 0;
}

enum immich_album_selection
immich_random_album_picture_status(struct immich_random_album_picture *r,
                                   size_t *total, size_t *kept) {
  // Loaded before the counts, so that a verdict other than NONE_KNOWN
  // guarantees the counts below are from that rotation or a later one, never
  // from before it
  enum immich_album_selection sel =
      (enum immich_album_selection)atomic_load(&r->selection);
  if (total) {
    *total = atomic_load(&r->n_albums);
  }
  if (kept) {
    *kept = atomic_load(&r->n_kept);
  }
  return sel;
}
