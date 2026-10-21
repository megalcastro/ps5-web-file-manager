#pragma once

#include <stddef.h>
#include <stdint.h>

#include <microhttpd.h>

#include "http_range.h"

typedef struct pkg_details {
  char content_id[37];
  char title[512];
  int platform; /* 4 = PS4, 5 = PS5; not an input to pkg_info.platform. */
  int has_icon;
} pkg_details_t;

int pkg_read_details(const char *path, pkg_details_t *details);
/* Reads at most size bytes at offset into buffer; 0 on success. */
typedef int (*pkg_read_fn)(void *ctx, void *buffer, size_t size, uint64_t offset);
int pkg_read_details_reader(pkg_read_fn reader, void *ctx, uint64_t size,
                            pkg_details_t *details);
/* On failure *error is HTTP_OK when the response was not a valid PKG. */
int pkg_read_details_url(const http_url_t *url, pkg_details_t *details,
                         uint64_t *size, http_result_t *error, int *status);
enum MHD_Result pkg_icon_response(struct MHD_Connection *conn, const char *path);
/* Borrows an already-open installation source. */
enum MHD_Result pkg_icon_response_fd(struct MHD_Connection *conn, int fd);
enum MHD_Result api_pkg_info(struct MHD_Connection *conn);
enum MHD_Result api_pkg_icon(struct MHD_Connection *conn);
