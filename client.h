#pragma once

// Internal to the immich library. Users only need immich.h.

#include "immich.h"

#include <curl/curl.h>
#include <stdbool.h>

typedef struct json_object json_object;

struct immich_client {
  CURL *curl;
  struct curl_slist *headers;      // Just the API key, for downloads
  struct curl_slist *json_headers; // API key plus JSON Accept/Content-Type
  char *base_url;                  // <host>/api
};

// GET <host>/api<path> and parse the response as JSON. Returns NULL on
// transport, HTTP or parse errors (after printing them to stderr). The caller
// owns the result and must json_object_put it.
json_object *immich_get_json(struct immich_client *c, const char *path);

// Like immich_get_json, but POSTs body (serialized as JSON). body is not
// consumed; the caller still owns it.
json_object *immich_post_json(struct immich_client *c, const char *path,
                              json_object *body);

// GET <host>/api<path> and write the response body to fd, from its current
// offset. Returns 0 on success; on transport, HTTP or write errors prints them
// to stderr and returns -1, in which case fd may hold a partial body. Error
// bodies are never written to fd.
int immich_get_to_fd(struct immich_client *c, const char *path, int fd);

// True if id looks like an Immich ID (a UUID). Check IDs before putting them
// into a URL path.
bool immich_is_valid_id(const char *id);
