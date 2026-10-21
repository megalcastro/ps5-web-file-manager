#pragma once

#include <stddef.h>
#include <stdint.h>

/* Minimal HTTP/1.1 client for validating remote PKG URLs with byte ranges.
   It never follows redirects and never accepts a full-body 200 response. */

#define HTTP_URL_MAX 2048
#define HTTP_HOST_MAX 254
#define HTTP_RANGE_MAX (4u * 1024u * 1024u)
#define HTTP_HEADER_MAX (16u * 1024u)

typedef enum http_result {
  HTTP_OK = 0,
  HTTP_ERR_URL_INVALID,
  HTTP_ERR_URL_SCHEME,
  HTTP_ERR_URL_CREDENTIALS,
  HTTP_ERR_URL_NOT_PKG,
  HTTP_ERR_RESOLVE,
  HTTP_ERR_CONNECT,
  HTTP_ERR_TIMEOUT,
  HTTP_ERR_IO,
  HTTP_ERR_PROTOCOL,
  HTTP_ERR_STATUS,
  HTTP_ERR_REDIRECT,
  HTTP_ERR_RANGE_IGNORED,
  HTTP_ERR_RANGE_INVALID,
  HTTP_ERR_INCOMPLETE,
  HTTP_ERR_NO_MEMORY,
} http_result_t;

typedef struct http_url {
  char host[HTTP_HOST_MAX + 1];
  unsigned short port;
  char target[HTTP_URL_MAX]; /* Path plus optional query, sent verbatim. */
  char canonical[HTTP_URL_MAX + HTTP_HOST_MAX + 16];
  char redacted[HTTP_URL_MAX + HTTP_HOST_MAX + 16]; /* Query removed. */
} http_url_t;

typedef struct http_range_response {
  int status;
  uint64_t total; /* Complete resource size from Content-Range. */
  size_t received;
} http_range_response_t;

/* Accepts only http://host[:port]/path[?query] with a path ending in .pkg. */
http_result_t http_url_parse(const char *text, http_url_t *url);

/* Requests bytes [offset, offset + length). Only a matching 206 succeeds; a
   short range is accepted only when it ends at the advertised resource end. */
http_result_t http_range_get(const http_url_t *url, uint64_t offset,
                             size_t length, void *buffer, int timeout_ms,
                             http_range_response_t *response);

/* Stable API error code for the web UI, e.g. "pkg_url_range_unsupported". */
const char *http_result_code(http_result_t result);
const char *http_result_message(http_result_t result);

/* Exposed for tests: parses "bytes first-last/total". */
int http_parse_content_range(const char *value, uint64_t *first,
                             uint64_t *last, uint64_t *total);
