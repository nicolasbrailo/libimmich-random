#include "client.h"

#include <json-c/json.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct buf {
  char *data;
  size_t len;
};

bool immich_is_valid_id(const char *id) {
  // A UUID is 32 hex digits and 4 hyphens. Using IMMICH_ID_SIZE keeps this
  // check and the size of ID buffers from disagreeing: a valid ID always fits.
  const size_t len = IMMICH_ID_SIZE - 1;
  return strlen(id) == len && strspn(id, "0123456789abcdefABCDEF-") == len;
}

static size_t buf_write_cb(char *ptr, size_t size, size_t nmemb,
                           void *userdata) {
  struct buf *b = userdata;
  size_t n = size * nmemb;
  char *p = realloc(b->data, b->len + n + 1);
  if (!p) {
    return 0; // Makes curl abort the transfer with CURLE_WRITE_ERROR
  }
  memcpy(p + b->len, ptr, n);
  b->data = p;
  b->len += n;
  b->data[b->len] = '\0';
  return n;
}

// Writes successful (2xx) response bodies to fd. Error bodies (JSON with a
// message) go to err instead, so they don't end up in fd and can be reported.
struct fd_sink {
  CURL *curl;
  int fd;
  int write_errno;
  struct buf err;
};

static size_t fd_write_cb(char *ptr, size_t size, size_t nmemb,
                          void *userdata) {
  struct fd_sink *s = userdata;
  long status = 0;
  curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
  if (status < 200 || status >= 300) {
    return buf_write_cb(ptr, size, nmemb, &s->err);
  }

  size_t n = size * nmemb;
  for (size_t done = 0; done < n;) {
    ssize_t w = write(s->fd, ptr + done, n - done);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w < 0) {
      s->write_errno = errno;
      return 0; // Makes curl abort the transfer with CURLE_WRITE_ERROR
    }
    done += (size_t)w;
  }
  return n;
}

// Sends a GET, or a POST of req_body if it's not NULL, passing the response
// body to write_cb. err_body is where write_cb leaves non-2xx bodies, so they
// can be included in the error message. Returns 0 on success; on transport
// errors or non-2xx responses prints them to stderr and returns -1.
static int perform(struct immich_client *c, const char *path,
                   const char *req_body, struct curl_slist *headers,
                   curl_write_callback write_cb, void *userdata,
                   const struct buf *err_body) {
  const char *method = req_body ? "POST" : "GET";
  size_t url_len = strlen(c->base_url) + strlen(path) + 1;
  char *url = malloc(url_len);
  if (!url) {
    return -1;
  }
  snprintf(url, url_len, "%s%s", c->base_url, path);

  char errbuf[CURL_ERROR_SIZE] = "";

  curl_easy_reset(c->curl);
  curl_easy_setopt(c->curl, CURLOPT_URL, url);
  if (req_body) {
    curl_easy_setopt(c->curl, CURLOPT_POSTFIELDS, req_body);
  } else {
    curl_easy_setopt(c->curl, CURLOPT_HTTPGET, 1L);
  }
  curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, userdata);
  curl_easy_setopt(c->curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT, 10L);
  // Abort transfers that stall for 30s, without capping how long a large
  // download may take
  curl_easy_setopt(c->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c->curl, CURLOPT_LOW_SPEED_TIME, 30L);

  int ret = -1;
  CURLcode res = curl_easy_perform(c->curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "%s %s failed: %s\n", method, url,
            errbuf[0] ? errbuf : curl_easy_strerror(res));
    goto out;
  }

  long status = 0;
  curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
  if (status < 200 || status >= 300) {
    fprintf(stderr, "%s %s: HTTP %ld: %s\n", method, url, status,
            err_body->data ? err_body->data : "");
    goto out;
  }
  ret = 0;

out:
  free(url);
  return ret;
}

static json_object *request_json(struct immich_client *c, const char *path,
                                 const char *req_body) {
  struct buf body = {0};
  json_object *json = NULL;
  if (perform(c, path, req_body, c->json_headers, buf_write_cb, &body,
              &body) == 0) {
    enum json_tokener_error err = json_tokener_success;
    json = json_tokener_parse_verbose(body.data ? body.data : "", &err);
    if (!json) {
      fprintf(stderr, "%s %s%s: invalid JSON response: %s\n",
              req_body ? "POST" : "GET", c->base_url, path,
              json_tokener_error_desc(err));
    }
  }
  free(body.data);
  return json;
}

json_object *immich_get_json(struct immich_client *c, const char *path) {
  return request_json(c, path, NULL);
}

json_object *immich_post_json(struct immich_client *c, const char *path,
                              json_object *body) {
  return request_json(
      c, path, json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
}

int immich_get_to_fd(struct immich_client *c, const char *path, int fd) {
  struct fd_sink sink = {.curl = c->curl, .fd = fd};
  int ret = perform(c, path, NULL, c->headers, fd_write_cb, &sink, &sink.err);
  if (sink.write_errno) {
    fprintf(stderr, "GET %s%s: writing to fd %d: %s\n", c->base_url, path, fd,
            strerror(sink.write_errno));
  }
  free(sink.err.data);
  return ret;
}
