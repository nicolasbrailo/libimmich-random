# libimmich-random

C library that retrieves a random sample of a random album hosted by
[Immich](https://immich.app), e.g. for a slideshow. Include only `immich.h`.

Build with `make` (needs libcurl and json-c: `make systemdeps`); link
`libimmich.a -lcurl -ljson-c -pthread`. `make test` runs the album-filter
tests, which need neither a server nor those libraries. The API key needs `album.read`,
`asset.read`, `asset.view` (and `asset.download` for originals).

## Interface

On failure, functions print why to stderr and return -1 (or NULL). Results are
released with their `*_free()`. See `immich.h` for details, `example/` for use.

- `immich_client_new(host, api_key)`, `immich_client_free(c)`: connect to a
  server. Use a client from one thread at a time.
- `immich_list_albums(c, &list)`, `immich_album_list_free(&list)`: all albums,
  each with its name, asset count and the dates of its oldest and newest asset.
- `immich_list_album_pictures(c, album_id, &list)`,
  `immich_picture_list_free(&list)`: an album's images, oldest first.
- `immich_get_picture_metadata(c, id, &info)`, `immich_picture_info_free(&info)`:
  dates, size, location, camera, people...
- `immich_fetch_picture_to_file(c, id, size, path)`: download a thumbnail,
  preview or original to a file.
- `immich_fetch_picture_to_fd(c, id, size)`: download into a sealed memfd, e.g.
  to pass over D-Bus. The caller closes it.
- `immich_random_album_picture_new(c, max, percent)`, `..._free(r)`: sampler
  that takes at most `max` and `percent`% of each album (0 = no limit).
- `immich_get_random_album_picture(r, id)`: next picture ID: a random sample of
  a random album, in album order, then another album.
- `immich_random_album_picture_refresh(r)`: re-fetch the album list at the next
  album. Non-blocking, callable from any thread.
- `immich_random_album_picture_set_filter(r, &filter)`: restrict the rotation to
  the albums whose name matches a comma-separated list of globs and whose dates
  overlap a year range. Applied to the cached album list, so it costs no
  request; non-blocking, callable from any thread.
- `immich_random_album_picture_status(r, &total, &kept)`: how many albums the
  server returned and how many are in the rotation, and whether an empty
  rotation means the server has nothing or the filter dropped everything.
- `immich_album_filter_match(&filter, &album)`: the selection rule on its own,
  as a pure function. `make test` exercises it.

Album names are matched against the whole name, case-insensitively, with `*`
and `?` as the only metacharacters; the year test is an overlap test, so an
album running 2010-2026 matches `from_year=2019, to_year=2021`. See
`immich.h` for the details.
