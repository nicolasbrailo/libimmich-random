#include "immich.h"
#include "client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct immich_client *immich_client_new(const char *host, const char *api_key) {
  // Reference counted, so each client can init and clean up independently and
  // users never need to deal with curl themselves
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    fprintf(stderr, "curl_global_init failed\n");
    return NULL;
  }

  struct immich_client *c = calloc(1, sizeof(*c));
  if (!c) {
    curl_global_cleanup();
    return NULL;
  }

  // Accept "http://host:2222/" as well as "http://host:2222"
  size_t host_len = strlen(host);
  while (host_len > 0 && host[host_len - 1] == '/') {
    host_len--;
  }

  c->curl = curl_easy_init();
  size_t url_len = host_len + sizeof("/api");
  c->base_url = malloc(url_len);
  size_t hdr_len = strlen(api_key) + sizeof("x-api-key: ");
  char *key_hdr = malloc(hdr_len);
  if (!c->curl || !c->base_url || !key_hdr) {
    fprintf(stderr, "Failed to create Immich client\n");
    free(key_hdr);
    immich_client_free(c);
    return NULL;
  }
  snprintf(c->base_url, url_len, "%.*s/api", (int)host_len, host);
  snprintf(key_hdr, hdr_len, "x-api-key: %s", api_key);

  c->headers = curl_slist_append(c->headers, key_hdr);
  c->json_headers = curl_slist_append(c->json_headers, key_hdr);
  c->json_headers =
      curl_slist_append(c->json_headers, "Accept: application/json");
  c->json_headers =
      curl_slist_append(c->json_headers, "Content-Type: application/json");
  free(key_hdr);

  return c;
}

void immich_client_free(struct immich_client *c) {
  if (!c) {
    return;
  }
  curl_slist_free_all(c->headers);
  curl_slist_free_all(c->json_headers);
  curl_easy_cleanup(c->curl);
  free(c->base_url);
  free(c);
  curl_global_cleanup();
}
