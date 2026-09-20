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
  // ISO 8601, when the album's oldest / newest asset was taken. Both are ""
  // if the server gave neither, which it may do for an album with no assets.
  char *start_date;
  char *end_date;
};

struct immich_album_list {
  struct immich_album *items;
  size_t count;
};

// Fetches every album visible to the API key. Returns 0 on success; on failure
// leaves *out empty and returns -1.
int immich_list_albums(struct immich_client *c, struct immich_album_list *out);
void immich_album_list_free(struct immich_album_list *list);

// ---- Album filter ----------------------------------------------------------

// Selects which albums are worth drawing pictures from. Immich's /albums can
// only filter by exact name, so anything more expressive has to be decided
// here, on the list the server already returned. Every field is optional: the
// zero value means "no constraint of that kind", so an all-zero filter keeps
// every album that has pictures.
//
// `name` and `exclude` are comma-separated lists of patterns. Each piece is
// trimmed and empty pieces are dropped, so " , ," holds no patterns at all.
// What is left is matched against the WHOLE album name, case-insensitively:
//   *  matches any run of characters, including none
//   ?  matches exactly one character
//   every other character is literal, including the '[' that fnmatch() would
//   read as a set and the '\' it would read as an escape, so an album named
//   "Trip (2019)" is matched by writing "Trip (2019)"
// Anchoring to the whole name is the important decision: with substring
// matching, "Pets" would silently also select "Pets 2019 backup" and there
// would be no way to ask for exactly one album.
struct immich_album_filter {
  const char *name;    // Keep an album whose name matches ANY of these
  const char *exclude; // Drop an album whose name matches any of these
  unsigned from_year;  // Only albums holding pictures from this year onwards
  unsigned to_year;    // Only albums holding pictures up to this year
};

// True if the album passes the filter. A NULL f, NULL strings and 0 years all
// mean "no constraint". An album is kept when it has pictures AND its name
// passes AND its dates overlap the year range:
//
//  - `exclude` is evaluated after `name`, so exclude wins on a conflict.
//  - The year test is an OVERLAP test: an album running 2010-2026 matches
//    from_year=2019, to_year=2021 even though neither of its own endpoints is
//    in that window, which is what "pictures from around 2020" means. Note
//    this makes a reversed range non-empty rather than empty -- it selects the
//    albums straddling the boundary -- so callers taking a range from a user
//    should reject from_year > to_year.
//  - An album the server gave no dates for is dropped as soon as either bound
//    is set, and kept when neither is. Keeping it would mean the albums we
//    know least about are the ones that always get through.
//  - An album with no assets never passes, whatever the rest of the filter
//    says: it would yield nothing, which shows up as a stall, not an error.
//
// Album names are not evidence about dates: real libraries hold an album
// called "2024 - office" whose assets run 2019-10-31 to 2024-08-01. This uses
// the server's dates, and users should be told it does.
//
// Pure: no network, no allocation, no shared state.
bool immich_album_filter_match(const struct immich_album_filter *f,
                               const struct immich_album *a);

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
int immich_get_picture_metadata(struct immich_client *c, const char *picture_id,
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

// Restricts the sampler to the albums matching f (see immich_album_filter); a
// NULL f, or an all-zero one, clears the restriction. The strings are copied,
// so neither f nor what it points at has to outlive the call.
//
// The album list is NOT re-fetched: the filter is applied to the cached list
// when the next picture is picked, and re-applied whenever the list is
// refreshed, so setting a filter never costs a request of its own. Whatever is
// left of the current album's sample is discarded, so the next picture always
// comes from an album the new filter allows.
//
// Returns -1, changing nothing, if the filter is invalid: a year above 9999,
// or from_year > to_year. The latter is rejected because, the year test being
// an overlap test, a reversed range quietly selects the albums straddling the
// boundary instead of nothing -- which looks exactly like the filter being
// ignored, since pictures keep appearing.
//
// This neither blocks nor uses the network, so it can be called from any
// thread at any time (e.g. from a D-Bus handler), even while
// immich_get_random_album_picture() is running.
int immich_random_album_picture_set_filter(
    struct immich_random_album_picture *r, const struct immich_album_filter *f);

// Why the sampler does, or doesn't, have albums to draw from
enum immich_album_selection {
  IMMICH_ALBUMS_OK,                 // At least one album is in the rotation
  IMMICH_ALBUMS_NONE_KNOWN,         // No album list yet: never fetched, or
                                    // every fetch so far failed
  IMMICH_ALBUMS_NONE_WITH_PICTURES, // The server has albums, none has pictures
  IMMICH_ALBUMS_NONE_MATCH_FILTER,  // Albums have pictures, but the filter
                                    // drops every one of them
};

// Reports the state of the album rotation and, through total / kept (either
// may be NULL), how many albums the server returned and how many of them are
// in the rotation. Together those two numbers turn every "is the filter
// working?" question into a one-line answer.
//
// It describes the last rotation that was built, so it reads NONE_KNOWN until
// the first immich_get_random_album_picture(). Lock-free: callable from any
// thread at any time, including while a picture is being picked.
//
// The returned status is always one rotation's own verdict, never a mix of
// two, so it is safe to branch on. total and kept are read separately and are
// meant for logging: a caller racing a rebuild can see them lag the status by
// one rotation, or disagree with each other.
enum immich_album_selection
immich_random_album_picture_status(struct immich_random_album_picture *r,
                                   size_t *total, size_t *kept);
