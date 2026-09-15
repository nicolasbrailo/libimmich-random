#include "immich.h"
#include "client.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Returns obj[key], or NULL if obj is NULL or the key is missing or null. The
// json-c getters map NULL to ""/0, so the result can be passed to them as is.
static json_object *field(json_object *obj, const char *key) {
  json_object *v = NULL;
  json_object_object_get_ex(obj, key, &v);
  return v;
}

// Returns a malloc'd copy of s ("" if s is NULL), or NULL if out of memory
static char *dup_str(const char *s) {
  if (!s) {
    s = "";
  }
  size_t len = strlen(s) + 1;
  char *copy = malloc(len);
  if (copy) {
    memcpy(copy, s, len);
  }
  return copy;
}

// Returns a malloc'd copy of obj[key] as a string ("" if missing or null). If
// out of memory, returns NULL and clears *ok.
static char *dup_field(json_object *obj, const char *key, bool *ok) {
  char *copy = dup_str(json_object_get_string(field(obj, key)));
  if (!copy) {
    *ok = false;
  }
  return copy;
}

int immich_list_albums(struct immich_client *c, struct immich_album_list *out) {
  out->items = NULL;
  out->count = 0;

  json_object *albums = immich_get_json(c, "/albums");
  if (!albums) {
    return -1;
  }

  int ret = -1;
  if (!json_object_is_type(albums, json_type_array)) {
    fprintf(stderr, "Unexpected /albums response: not an array\n");
    goto out;
  }

  size_t n = json_object_array_length(albums);
  if (n > 0) {
    out->items = calloc(n, sizeof(*out->items));
    if (!out->items) {
      goto out;
    }
  }
  for (size_t i = 0; i < n; i++) {
    json_object *album = json_object_array_get_idx(albums, i);
    struct immich_album *a = &out->items[out->count++];
    bool ok = true;
    a->id = dup_field(album, "id", &ok);
    a->name = dup_field(album, "albumName", &ok);
    a->asset_count = json_object_get_int(field(album, "assetCount"));
    if (!ok) {
      goto out;
    }
  }
  ret = 0;

out:
  json_object_put(albums);
  if (ret != 0) {
    immich_album_list_free(out);
  }
  return ret;
}

void immich_album_list_free(struct immich_album_list *list) {
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].id);
    free(list->items[i].name);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

// Appends the pictures in one /search/metadata response to out and sets req's
// cursor for the next page. Returns 1 if there are more pages, 0 if this was
// the last one, -1 on error.
static int append_page(json_object *resp, json_object *req,
                       struct immich_picture_list *out) {
  json_object *assets = field(resp, "assets");
  json_object *items = field(assets, "items");
  json_object *next = field(assets, "nextCursor");
  if (!json_object_is_type(items, json_type_array)) {
    fprintf(stderr, "Unexpected /search/metadata response: no assets.items\n");
    return -1;
  }

  size_t n = json_object_array_length(items);
  if (n > 0) {
    struct immich_picture *grown =
        realloc(out->items, (out->count + n) * sizeof(*out->items));
    if (!grown) {
      return -1;
    }
    out->items = grown;
  }
  for (size_t i = 0; i < n; i++) {
    json_object *asset = json_object_array_get_idx(items, i);
    struct immich_picture *p = &out->items[out->count++];
    bool ok = true;
    p->id = dup_field(asset, "id", &ok);
    p->taken = dup_field(asset, "localDateTime", &ok);
    p->file_name = dup_field(asset, "originalFileName", &ok);
    if (!ok) {
      return -1;
    }
  }

  // nextCursor is null on the last page. Otherwise send it back as "cursor";
  // take a ref so it outlives resp.
  if (!json_object_is_type(next, json_type_string)) {
    return 0;
  }
  json_object_object_add(req, "cursor", json_object_get(next));
  return 1;
}

int immich_list_album_pictures(struct immich_client *c, const char *album_id,
                               struct immich_picture_list *out) {
  out->items = NULL;
  out->count = 0;

  // {"size": 1000,
  //  "filter": {"albumIds": {"any": [album_id]}, "type": {"eq": "IMAGE"}},
  //  "orderBy": {"field": "fileCreatedAt", "direction": "asc"}}
  json_object *any = json_object_new_array();
  json_object_array_add(any, json_object_new_string(album_id));
  json_object *album_ids = json_object_new_object();
  json_object_object_add(album_ids, "any", any);
  json_object *type = json_object_new_object();
  json_object_object_add(type, "eq", json_object_new_string("IMAGE"));
  json_object *filter = json_object_new_object();
  json_object_object_add(filter, "albumIds", album_ids);
  json_object_object_add(filter, "type", type);
  json_object *req = json_object_new_object();
  json_object_object_add(req, "size", json_object_new_int(1000));
  json_object_object_add(req, "filter", filter);
  // Oldest first: the default is newest first. fileCreatedAt is when the
  // picture was taken, in UTC, so this is chronological across time zones.
  json_object *order_by = json_object_new_object();
  json_object_object_add(order_by, "field",
                         json_object_new_string("fileCreatedAt"));
  json_object_object_add(order_by, "direction", json_object_new_string("asc"));
  json_object_object_add(req, "orderBy", order_by);

  int r;
  do {
    json_object *resp = immich_post_json(c, "/search/metadata", req);
    r = resp ? append_page(resp, req, out) : -1;
    json_object_put(resp);
  } while (r > 0);
  json_object_put(req);

  if (r < 0) {
    immich_picture_list_free(out);
    return -1;
  }
  return 0;
}

void immich_picture_list_free(struct immich_picture_list *list) {
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].id);
    free(list->items[i].taken);
    free(list->items[i].file_name);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

// Copies the names in the asset's "people" array, skipping unnamed faces.
// Returns -1 if out of memory.
static int collect_people(json_object *people,
                          struct immich_picture_info *out) {
  size_t n = json_object_is_type(people, json_type_array)
                 ? json_object_array_length(people)
                 : 0;
  if (n == 0) {
    return 0;
  }
  out->people = calloc(n, sizeof(*out->people));
  if (!out->people) {
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    const char *name = json_object_get_string(
        field(json_object_array_get_idx(people, i), "name"));
    if (!name || !name[0]) {
      continue;
    }
    char *copy = dup_str(name);
    if (!copy) {
      return -1;
    }
    out->people[out->people_count++] = copy;
  }
  return 0;
}

int immich_get_picture_metadata(struct immich_client *c,
                                const char *picture_id,
                                struct immich_picture_info *out) {
  memset(out, 0, sizeof(*out));

  // The ID goes into the URL path, so only accept a UUID
  if (!immich_is_valid_id(picture_id)) {
    fprintf(stderr, "Invalid picture ID: %s\n", picture_id);
    return -1;
  }
  char path[64];
  snprintf(path, sizeof(path), "/assets/%s", picture_id);

  json_object *asset = immich_get_json(c, path);
  if (!asset) {
    return -1;
  }

  int ret = -1;
  if (!json_object_is_type(asset, json_type_object)) {
    fprintf(stderr, "Unexpected %s response: not an object\n", path);
    goto out;
  }

  bool ok = true;
  out->id = dup_field(asset, "id", &ok);
  out->file_name = dup_field(asset, "originalFileName", &ok);
  out->original_path = dup_field(asset, "originalPath", &ok);
  out->mime_type = dup_field(asset, "originalMimeType", &ok);
  out->type = dup_field(asset, "type", &ok);
  out->taken = dup_field(asset, "localDateTime", &ok);
  out->taken_utc = dup_field(asset, "fileCreatedAt", &ok);
  out->width = json_object_get_int(field(asset, "width"));
  out->height = json_object_get_int(field(asset, "height"));
  out->is_favorite = json_object_get_boolean(field(asset, "isFavorite"));

  // May be NULL (no EXIF); field() and the getters then yield ""/0
  json_object *exif = field(asset, "exifInfo");
  out->description = dup_field(exif, "description", &ok);
  out->city = dup_field(exif, "city", &ok);
  out->state = dup_field(exif, "state", &ok);
  out->country = dup_field(exif, "country", &ok);
  json_object *lat = field(exif, "latitude");
  json_object *lon = field(exif, "longitude");
  out->has_location = lat && lon;
  out->latitude = json_object_get_double(lat);
  out->longitude = json_object_get_double(lon);
  out->camera_make = dup_field(exif, "make", &ok);
  out->camera_model = dup_field(exif, "model", &ok);
  out->lens = dup_field(exif, "lensModel", &ok);
  out->f_number = json_object_get_double(field(exif, "fNumber"));
  out->focal_length = json_object_get_double(field(exif, "focalLength"));
  out->exposure_time = dup_field(exif, "exposureTime", &ok);
  out->iso = json_object_get_int(field(exif, "iso"));
  out->rating = json_object_get_int(field(exif, "rating"));

  if (!ok || collect_people(field(asset, "people"), out) != 0) {
    goto out;
  }
  ret = 0;

out:
  json_object_put(asset);
  if (ret != 0) {
    immich_picture_info_free(out);
  }
  return ret;
}

void immich_picture_info_free(struct immich_picture_info *info) {
  free(info->id);
  free(info->file_name);
  free(info->original_path);
  free(info->mime_type);
  free(info->type);
  free(info->taken);
  free(info->taken_utc);
  free(info->description);
  free(info->city);
  free(info->state);
  free(info->country);
  free(info->camera_make);
  free(info->camera_model);
  free(info->lens);
  free(info->exposure_time);
  for (size_t i = 0; i < info->people_count; i++) {
    free(info->people[i]);
  }
  free(info->people);
  memset(info, 0, sizeof(*info));
}
