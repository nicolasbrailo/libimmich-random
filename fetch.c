// For memfd_create, O_CLOEXEC and file sealing
#define _GNU_SOURCE

#include "immich.h"
#include "client.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Writes the API path that serves the picture at the given size into path.
// Returns -1 (after printing to stderr) on invalid arguments.
static int picture_path(const char *picture_id, enum immich_picture_size size,
                        char *path, size_t path_size) {
  // The ID goes into the URL path, so only accept a UUID
  if (!immich_is_valid_id(picture_id)) {
    fprintf(stderr, "Invalid picture ID: %s\n", picture_id);
    return -1;
  }

  switch (size) {
  case IMMICH_PICTURE_THUMBNAIL:
    snprintf(path, path_size, "/assets/%s/thumbnail?size=thumbnail",
             picture_id);
    return 0;
  case IMMICH_PICTURE_PREVIEW:
    snprintf(path, path_size, "/assets/%s/thumbnail?size=preview", picture_id);
    return 0;
  case IMMICH_PICTURE_ORIGINAL:
    snprintf(path, path_size, "/assets/%s/original", picture_id);
    return 0;
  }
  fprintf(stderr, "Invalid picture size: %d\n", (int)size);
  return -1;
}

int immich_fetch_picture_to_file(struct immich_client *c,
                                 const char *picture_id,
                                 enum immich_picture_size size,
                                 const char *path) {
  char api_path[128];
  if (picture_path(picture_id, size, api_path, sizeof(api_path)) != 0) {
    return -1;
  }

  size_t tmp_len = strlen(path) + sizeof(".part");
  char *tmp = malloc(tmp_len);
  if (!tmp) {
    return -1;
  }
  snprintf(tmp, tmp_len, "%s.part", path);

  int ret = -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    perror(tmp);
    goto out;
  }

  ret = immich_get_to_fd(c, api_path, fd);
  if (close(fd) != 0 && ret == 0) {
    perror(tmp);
    ret = -1;
  }
  if (ret == 0 && rename(tmp, path) != 0) {
    perror(path);
    ret = -1;
  }
  if (ret != 0) {
    unlink(tmp);
  }

out:
  free(tmp);
  return ret;
}

int immich_fetch_picture_to_fd(struct immich_client *c, const char *picture_id,
                               enum immich_picture_size size) {
  char api_path[128];
  if (picture_path(picture_id, size, api_path, sizeof(api_path)) != 0) {
    return -1;
  }

  // The name is only for debugging: it shows in /proc/<pid>/fd
  char name[64];
  snprintf(name, sizeof(name), "immich-%s", picture_id);
  int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    perror("memfd_create");
    return -1;
  }

  if (immich_get_to_fd(c, api_path, fd) != 0) {
    goto fail;
  }

  // Freeze the contents, so receivers of the fd can trust (and mmap) them
  // without worrying about them changing underneath
  if (fcntl(fd, F_ADD_SEALS,
            F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
    perror("Sealing picture memfd");
    goto fail;
  }
  if (lseek(fd, 0, SEEK_SET) != 0) {
    perror("lseek");
    goto fail;
  }
  return fd;

fail:
  close(fd);
  return -1;
}
