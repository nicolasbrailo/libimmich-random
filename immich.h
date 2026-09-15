#pragma once

// Immich client library. This is the only header users need.
//
// Functions that can fail print the reason to stderr and return -1 (or NULL).
// Results own all their strings; release everything with the matching *_free().
// Strings are never NULL: missing fields are "". Freeing an empty or
// already-freed result is safe.

#include <stdbool.h>
#include <stddef.h>

// ---- Client ----------------------------------------------------------------

struct immich_client;

// host is the server's base URL, e.g. "http://bati.casa:2222" (a trailing '/'
// is fine). Returns NULL on failure. A client may only be used by one thread
// at a time; create one per thread if needed.
struct immich_client *immich_client_new(const char *host, const char *api_key);
void immich_client_free(struct immich_client *c);

// ---- Albums ----------------------------------------------------------------

struct immich_album {
  char *id;
  char *name;
  int asset_count;
};

struct immich_album_list {
  struct immich_album *items;
  size_t count;
};

// Fetches every album visible to the API key. Returns 0 on success; on failure
// leaves *out empty and returns -1.
int immich_list_albums(struct immich_client *c, struct immich_album_list *out);
void immich_album_list_free(struct immich_album_list *list);

// ---- Pictures in an album --------------------------------------------------

struct immich_picture {
  char *id;
  char *taken; // localDateTime: when it was taken, local time, ISO 8601
  char *file_name;
};

struct immich_picture_list {
  struct immich_picture *items;
  size_t count;
};

// Fetches every image (no videos) in an album, oldest first, following
// pagination. Returns 0 on success; on failure leaves *out empty and returns
// -1.
int immich_list_album_pictures(struct immich_client *c, const char *album_id,
                               struct immich_picture_list *out);
void immich_picture_list_free(struct immich_picture_list *list);

// ---- Picture metadata ------------------------------------------------------

// Metadata for one picture. Numbers are 0 if unknown.
struct immich_picture_info {
  char *id;
  char *file_name;
  char *original_path; // Where the original is stored on the server
  char *mime_type;
  char *type;      // "IMAGE", "VIDEO", ...
  char *taken;     // localDateTime: when it was taken, local time, ISO 8601
  char *taken_utc; // fileCreatedAt: when it was taken, UTC, ISO 8601
  int width;
  int height;
  bool is_favorite;

  // From EXIF
  char *description;
  char *city;
  char *state;
  char *country;
  bool has_location; // latitude/longitude are only valid if set
  double latitude;
  double longitude;
  char *camera_make;
  char *camera_model;
  char *lens;
  double f_number;
  double focal_length; // mm
  char *exposure_time; // e.g. "1/250"
  int iso;
  int rating;

  char **people; // Names of recognised people; unnamed faces are skipped
  size_t people_count;
};

// Fetches the metadata of one picture. Returns 0 on success; on failure leaves
// *out empty and returns -1.
int immich_get_picture_metadata(struct immich_client *c,
                                const char *picture_id,
                                struct immich_picture_info *out);
void immich_picture_info_free(struct immich_picture_info *info);

// ---- Downloading pictures --------------------------------------------------

enum immich_picture_size {
  IMMICH_PICTURE_THUMBNAIL, // Small thumbnail (~250px)
  IMMICH_PICTURE_PREVIEW,   // Display size (~1440px), JPEG or WebP
  IMMICH_PICTURE_ORIGINAL,  // As uploaded (may be HEIC, RAW, ...). Needs the
                            // asset.download permission on the API key
};

// Downloads a picture to path, replacing it if it exists. The download goes to
// "<path>.part" first and is renamed when complete, so path never holds a
// partial picture. Returns 0 on success; on failure removes the partial file
// and returns -1.
int immich_fetch_picture_to_file(struct immich_client *c,
                                 const char *picture_id,
                                 enum immich_picture_size size,
                                 const char *path);

// Downloads a picture into a new in-memory file (memfd) and returns its fd, or
// -1 on failure. The caller owns the fd and must close() it; fstat() gives the
// picture's size.
//
// The fd is sealed, so its contents can never change. That makes it safe to
// hand to other processes (e.g. over D-Bus), which can mmap() it. Its file
// offset is shared with every process holding it, so readers should use
// mmap() or pread() rather than read().
int immich_fetch_picture_to_fd(struct immich_client *c, const char *picture_id,
                               enum immich_picture_size size);

// ---- Random pictures, album by album ---------------------------------------

// Size of a buffer that holds an Immich ID (a UUID) and its terminating NUL
#define IMMICH_ID_SIZE 37

struct immich_random_album_picture;

// Creates the state for immich_get_random_album_picture(). From each album it
// samples:
//  - max_pictures: at most this many pictures. 0 means no limit.
//  - percent: this percentage of its pictures (rounded, but at least 1). 0
//    means 100.
// If both are set, the percentage is applied first and then capped: with
// max_pictures=20 and percent=50, an album of 30 pictures yields 15 and one of
// 100 yields 20. c must outlive the returned state. Returns NULL on failure.
struct immich_random_album_picture *
immich_random_album_picture_new(struct immich_client *c, size_t max_pictures,
                                unsigned percent);

// Must not be called while another thread is using r.
void immich_random_album_picture_free(struct immich_random_album_picture *r);

// Writes the ID of the next picture to show into id, for use with
// immich_fetch_picture_to_*(). Pictures come album by album: an album is
// picked at random and sampled, its sampled pictures are returned in album
// order (oldest first), then the next album is picked. Every album is visited
// once, in random order, before any album repeats. The album list is fetched
// on the first call and cached; the pictures of an album are fetched when it's
// picked.
//
// Returns 0 on success, or -1 if no picture could be picked (e.g. the server
// is unreachable, or no album has any pictures). Calls from several threads
// are serialised. It uses the client, so the client must not be used by
// another thread at the same time.
int immich_get_random_album_picture(struct immich_random_album_picture *r,
                                    char id[IMMICH_ID_SIZE]);

// Marks the cached album list as stale. It is re-fetched when the next album
// is picked; pictures already sampled from the current album are still
// returned first. If the re-fetch fails, the old list is kept and the fetch is
// retried at the next album. This neither blocks nor uses the network, so it
// can be called from any thread at any time (e.g. from a timer or a D-Bus
// handler), even while immich_get_random_album_picture() is running.
void immich_random_album_picture_refresh(struct immich_random_album_picture *r);
