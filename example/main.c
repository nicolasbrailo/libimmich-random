#include "immich.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Set these to your server and an API key (Account Settings > API Keys)
#define IMMICH_HOST "http://bati.casa:2222"
#define IMMICH_API_KEY "YOUR_API_KEY"

static int print_albums(struct immich_client *client) {
  struct immich_album_list albums;
  if (immich_list_albums(client, &albums) != 0) {
    return -1;
  }

  printf("%-36s  %6s  %s\n", "ID", "ASSETS", "NAME");
  for (size_t i = 0; i < albums.count; i++) {
    const struct immich_album *a = &albums.items[i];
    printf("%-36s  %6d  %s\n", a->id, a->asset_count, a->name);
  }
  printf("%zu albums\n", albums.count);

  immich_album_list_free(&albums);
  return 0;
}

static int print_album_pictures(struct immich_client *client,
                                const char *album_id) {
  struct immich_picture_list pics;
  if (immich_list_album_pictures(client, album_id, &pics) != 0) {
    return -1;
  }

  printf("%-36s  %-10s  %s\n", "ID", "TAKEN", "FILE");
  for (size_t i = 0; i < pics.count; i++) {
    const struct immich_picture *p = &pics.items[i];
    // taken is ISO 8601; the first 10 chars are the date
    printf("%-36s  %-10.10s  %s\n", p->id, p->taken, p->file_name);
  }
  printf("%zu pictures\n", pics.count);

  immich_picture_list_free(&pics);
  return 0;
}

// Prints "label value", unless value is empty
static void print_str(const char *label, const char *value) {
  if (value[0]) {
    printf("%-14s %s\n", label, value);
  }
}

static int print_picture(struct immich_client *client, const char *picture_id) {
  struct immich_picture_info p;
  if (immich_get_picture_metadata(client, picture_id, &p) != 0) {
    return -1;
  }

  print_str("ID", p.id);
  print_str("File", p.file_name);
  print_str("Path", p.original_path);
  print_str("MIME type", p.mime_type);
  print_str("Type", p.type);
  print_str("Taken (local)", p.taken);
  print_str("Taken (UTC)", p.taken_utc);
  if (p.width && p.height) {
    printf("%-14s %dx%d\n", "Size", p.width, p.height);
  }
  if (p.is_favorite) {
    printf("%-14s yes\n", "Favorite");
  }

  print_str("Description", p.description);
  print_str("City", p.city);
  print_str("State", p.state);
  print_str("Country", p.country);
  if (p.has_location) {
    printf("%-14s %.6f, %.6f\n", "Location", p.latitude, p.longitude);
  }
  print_str("Camera make", p.camera_make);
  print_str("Camera model", p.camera_model);
  print_str("Lens", p.lens);
  if (p.f_number) {
    printf("%-14s f/%g\n", "Aperture", p.f_number);
  }
  if (p.focal_length) {
    printf("%-14s %g mm\n", "Focal length", p.focal_length);
  }
  if (p.exposure_time[0]) {
    printf("%-14s %s s\n", "Exposure", p.exposure_time);
  }
  if (p.iso) {
    printf("%-14s %d\n", "ISO", p.iso);
  }
  if (p.rating) {
    printf("%-14s %d\n", "Rating", p.rating);
  }

  if (p.people_count) {
    printf("%-14s", "People");
    for (size_t i = 0; i < p.people_count; i++) {
      printf("%s%s", i ? ", " : " ", p.people[i]);
    }
    printf("\n");
  }

  immich_picture_info_free(&p);
  return 0;
}

// Picks count random pictures and prints each with enough metadata to check
// the sampling: the server path shows which album/folder it came from, and
// the date (UTC, which albums are sorted by) shows the order within an album
static int print_random_pictures(struct immich_client *client,
                                 unsigned long count, size_t max_pictures,
                                 unsigned percent) {
  struct immich_random_album_picture *r =
      immich_random_album_picture_new(client, max_pictures, percent);
  if (!r) {
    return -1;
  }

  int ret = 0;
  printf("%4s  %-19s  %-36s  %s\n", "#", "TAKEN (UTC)", "ID", "PATH");
  for (unsigned long i = 0; i < count; i++) {
    char id[IMMICH_ID_SIZE];
    struct immich_picture_info p;
    if (immich_get_random_album_picture(r, id) != 0 ||
        immich_get_picture_metadata(client, id, &p) != 0) {
      ret = -1;
      break;
    }
    // taken_utc is ISO 8601; the first 19 chars are the date and time
    printf("%4lu  %-19.19s  %-36s  %s\n", i + 1, p.taken_utc, p.id,
           p.original_path[0] ? p.original_path : p.file_name);
    immich_picture_info_free(&p);
  }

  immich_random_album_picture_free(r);
  return ret;
}

// Parses a non-negative decimal number. Returns -1 if s isn't one.
static int parse_number(const char *s, unsigned long *out) {
  if (s[0] < '0' || s[0] > '9') {
    return -1;
  }
  char *end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || *end != '\0') {
    return -1;
  }
  *out = v;
  return 0;
}

int main(int argc, char **argv) {
  int ret = EXIT_FAILURE;
  // 0 lists albums, 'a' lists an album, 'p' shows a picture, 'd' downloads one,
  // 'r' picks random pictures
  char mode = 0;
  const char *id = NULL;
  const char *out_path = NULL;
  unsigned long count = 0, max_pictures = 5, percent = 50;
  bool args_ok = true;
  if (argc == 1) {
    // List albums
  } else if (argc == 3 &&
             (strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "-p") == 0)) {
    mode = argv[1][1];
    id = argv[2];
  } else if (argc == 4 && strcmp(argv[1], "-d") == 0) {
    mode = 'd';
    id = argv[2];
    out_path = argv[3];
  } else if (argc >= 3 && argc <= 5 && strcmp(argv[1], "-r") == 0) {
    mode = 'r';
    args_ok = parse_number(argv[2], &count) == 0 && count > 0 &&
              (argc < 4 || parse_number(argv[3], &max_pictures) == 0) &&
              (argc < 5 || (parse_number(argv[4], &percent) == 0 &&
                            percent <= 100));
  } else {
    args_ok = false;
  }
  if (!args_ok) {
    fprintf(stderr,
            "Usage: %s                       list albums\n"
            "       %s -a album-id           list an album\n"
            "       %s -p picture-id         show metadata\n"
            "       %s -d picture-id file    download a picture\n"
            "       %s -r count [max [pct]]  pick random pictures\n"
            "\n"
            "-r samples at most max (default 5) and pct%% (default 50) of\n"
            "each album; 0 means no limit.\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
    return EXIT_FAILURE;
  }

  struct immich_client *client = immich_client_new(IMMICH_HOST, IMMICH_API_KEY);
  if (client) {
    int r;
    switch (mode) {
    case 'a':
      r = print_album_pictures(client, id);
      break;
    case 'p':
      r = print_picture(client, id);
      break;
    case 'd':
      r = immich_fetch_picture_to_file(client, id, IMMICH_PICTURE_PREVIEW,
                                       out_path);
      if (r == 0) {
        printf("Saved %s\n", out_path);
      }
      break;
    case 'r':
      r = print_random_pictures(client, count, max_pictures,
                                (unsigned)percent);
      break;
    default:
      r = print_albums(client);
      break;
    }
    if (r == 0) {
      ret = EXIT_SUCCESS;
    }
  }

  immich_client_free(client);
  return ret;
}
