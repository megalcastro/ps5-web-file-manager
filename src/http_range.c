#include "http_range.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION_TAG
#define VERSION_TAG "dev"
#endif

#ifdef MSG_NOSIGNAL
#define HTTP_SEND_FLAGS MSG_NOSIGNAL
#else
#define HTTP_SEND_FLAGS 0
#endif

typedef struct http_conn {
  int fd;
  struct timespec deadline;
  unsigned char buf[HTTP_HEADER_MAX];
  size_t pos;
  size_t len;
  /* Kept off the stack: MHD runs handlers on per-connection threads. */
  char line[HTTP_HEADER_MAX];
  char request[HTTP_URL_MAX + 512];
} http_conn_t;

const char *
http_result_code(http_result_t result) {
  switch(result) {
  case HTTP_OK: return "ok";
  case HTTP_ERR_URL_INVALID: return "pkg_url_invalid";
  case HTTP_ERR_URL_SCHEME: return "pkg_url_scheme";
  case HTTP_ERR_URL_CREDENTIALS: return "pkg_url_credentials";
  case HTTP_ERR_URL_NOT_PKG: return "pkg_type_invalid";
  case HTTP_ERR_RESOLVE: return "pkg_url_resolve";
  case HTTP_ERR_CONNECT: return "pkg_url_connect";
  case HTTP_ERR_TIMEOUT: return "pkg_url_timeout";
  case HTTP_ERR_IO: return "pkg_url_io";
  case HTTP_ERR_PROTOCOL: return "pkg_url_protocol";
  case HTTP_ERR_STATUS: return "pkg_url_status";
  case HTTP_ERR_REDIRECT: return "pkg_url_redirect";
  case HTTP_ERR_RANGE_IGNORED: return "pkg_url_range_unsupported";
  case HTTP_ERR_RANGE_INVALID: return "pkg_url_range_invalid";
  case HTTP_ERR_INCOMPLETE: return "pkg_url_incomplete";
  case HTTP_ERR_NO_MEMORY: return "out_of_memory";
  default: return "system_error";
  }
}

const char *
http_result_message(http_result_t result) {
  switch(result) {
  case HTTP_OK: return "ok";
  case HTTP_ERR_URL_INVALID: return "invalid package URL";
  case HTTP_ERR_URL_SCHEME: return "only http:// package URLs are supported";
  case HTTP_ERR_URL_CREDENTIALS: return "package URLs must not contain credentials";
  case HTTP_ERR_URL_NOT_PKG: return "file is not a PKG package";
  case HTTP_ERR_RESOLVE: return "could not resolve package server";
  case HTTP_ERR_CONNECT: return "could not connect to package server";
  case HTTP_ERR_TIMEOUT: return "package server timed out";
  case HTTP_ERR_IO: return "package server connection failed";
  case HTTP_ERR_PROTOCOL: return "package server sent an invalid HTTP response";
  case HTTP_ERR_STATUS: return "package server returned an error status";
  case HTTP_ERR_REDIRECT: return "package server redirects are not supported";
  case HTTP_ERR_RANGE_IGNORED: return "package server does not support byte-range requests";
  case HTTP_ERR_RANGE_INVALID: return "package server returned an invalid byte range";
  case HTTP_ERR_INCOMPLETE: return "package server closed the connection early";
  case HTTP_ERR_NO_MEMORY: return "out of memory";
  default: return "package server error";
  }
}

static int
host_char_valid(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-';
}

http_result_t
http_url_parse(const char *text, http_url_t *url) {
  const char *authority;
  const char *authority_end;
  const char *host_end;
  const char *path_end;
  size_t length;
  size_t host_length;
  unsigned long port = 80;

  memset(url, 0, sizeof(*url));
  if(!text || !(length = strlen(text)) || length >= HTTP_URL_MAX) return HTTP_ERR_URL_INVALID;
  for(size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)text[i];
    /* Non-ASCII must be percent-encoded; whitespace and controls are never valid. */
    if(c <= 0x20 || c >= 0x7f || c == '\\') return HTTP_ERR_URL_INVALID;
  }
  if(strncasecmp(text, "http://", 7)) {
    return strstr(text, "://") ? HTTP_ERR_URL_SCHEME : HTTP_ERR_URL_INVALID;
  }
  if(strchr(text, '#')) return HTTP_ERR_URL_INVALID;

  authority = text + 7;
  authority_end = authority + strcspn(authority, "/?");
  if(memchr(authority, '@', (size_t)(authority_end - authority))) return HTTP_ERR_URL_CREDENTIALS;
  if(*authority == '[') return HTTP_ERR_URL_INVALID; /* IPv6 literals are not supported. */
  host_end = memchr(authority, ':', (size_t)(authority_end - authority));
  if(!host_end) host_end = authority_end;
  host_length = (size_t)(host_end - authority);
  if(!host_length || host_length > HTTP_HOST_MAX - 1 ||
     authority[0] == '.' || authority[0] == '-' ||
     host_end[-1] == '.' || host_end[-1] == '-') return HTTP_ERR_URL_INVALID;
  for(const char *p = authority; p < host_end; p++) {
    if(!host_char_valid(*p)) return HTTP_ERR_URL_INVALID;
  }
  if(host_end < authority_end) {
    const char *digits = host_end + 1;
    size_t count = (size_t)(authority_end - digits);
    if(!count || count > 5) return HTTP_ERR_URL_INVALID;
    port = 0;
    for(size_t i = 0; i < count; i++) {
      if(digits[i] < '0' || digits[i] > '9') return HTTP_ERR_URL_INVALID;
      port = port * 10 + (unsigned long)(digits[i] - '0');
    }
    if(!port || port > 65535) return HTTP_ERR_URL_INVALID;
  }
  if(*authority_end != '/') return HTTP_ERR_URL_NOT_PKG;

  path_end = authority_end + strcspn(authority_end, "?");
  if(path_end - authority_end < 5 || strncasecmp(path_end - 4, ".pkg", 4)) return HTTP_ERR_URL_NOT_PKG;

  memcpy(url->host, authority, host_length);
  for(size_t i = 0; i < host_length; i++) url->host[i] = (char)tolower((unsigned char)url->host[i]);
  url->port = (unsigned short)port;
  snprintf(url->target, sizeof(url->target), "%s", authority_end);
  if(port == 80) {
    snprintf(url->canonical, sizeof(url->canonical), "http://%s%s", url->host, url->target);
    snprintf(url->redacted, sizeof(url->redacted), "http://%s%.*s%s", url->host,
             (int)(path_end - authority_end), authority_end, *path_end ? "?..." : "");
  } else {
    snprintf(url->canonical, sizeof(url->canonical), "http://%s:%u%s", url->host, url->port, url->target);
    snprintf(url->redacted, sizeof(url->redacted), "http://%s:%u%.*s%s", url->host, url->port,
             (int)(path_end - authority_end), authority_end, *path_end ? "?..." : "");
  }
  return HTTP_OK;
}

static int
parse_u64(const char **cursor, uint64_t *value) {
  const char *p = *cursor;
  uint64_t result = 0;

  if(*p < '0' || *p > '9') return -1;
  for(; *p >= '0' && *p <= '9'; p++) {
    unsigned int digit = (unsigned int)(*p - '0');
    if(result > (UINT64_MAX - digit) / 10) return -1;
    result = result * 10 + digit;
  }
  *value = result;
  *cursor = p;
  return 0;
}

int
http_parse_content_range(const char *value, uint64_t *first, uint64_t *last,
                         uint64_t *total) {
  const char *p = value;

  if(strncasecmp(p, "bytes", 5)) return -1;
  p += 5;
  if(*p != ' ' && *p != '\t') return -1;
  while(*p == ' ' || *p == '\t') p++;
  if(parse_u64(&p, first) || *p++ != '-' || parse_u64(&p, last) ||
     *p++ != '/' || parse_u64(&p, total)) return -1;
  while(*p == ' ' || *p == '\t') p++;
  if(*p || *last < *first || *last >= *total) return -1;
  return 0;
}

static long
remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  long long ms;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (long long)(deadline->tv_sec - now.tv_sec) * 1000 +
       (deadline->tv_nsec - now.tv_nsec) / 1000000;
  return ms > 0 ? (long)ms : 0;
}

static http_result_t
wait_fd(http_conn_t *conn, short events) {
  while(1) {
    struct pollfd pfd = {.fd = conn->fd, .events = events};
    long timeout = remaining_ms(&conn->deadline);
    int ready;

    if(!timeout) return HTTP_ERR_TIMEOUT;
    ready = poll(&pfd, 1, (int)timeout);
    if(ready > 0) return HTTP_OK;
    if(!ready) return HTTP_ERR_TIMEOUT;
    if(errno != EINTR) return HTTP_ERR_IO;
  }
}

static http_result_t
conn_open(http_conn_t *conn, const http_url_t *url) {
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *addresses = NULL;
  char port[8];
  http_result_t result = HTTP_ERR_CONNECT;

  snprintf(port, sizeof(port), "%u", url->port);
  if(getaddrinfo(url->host, port, &hints, &addresses) || !addresses) return HTTP_ERR_RESOLVE;
  for(struct addrinfo *address = addresses; address; address = address->ai_next) {
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    int flags;
    int error = 0;
    socklen_t error_size = sizeof(error);

    if(fd < 0) continue;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#endif
    if((flags = fcntl(fd, F_GETFL, 0)) < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      close(fd);
      continue;
    }
    conn->fd = fd;
    if(!connect(fd, address->ai_addr, address->ai_addrlen) ||
       (errno == EINPROGRESS &&
        (result = wait_fd(conn, POLLOUT)) == HTTP_OK &&
        !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) && !error)) {
      result = HTTP_OK;
      break;
    }
    if(result != HTTP_ERR_TIMEOUT) result = HTTP_ERR_CONNECT;
    close(fd);
    conn->fd = -1;
    if(result == HTTP_ERR_TIMEOUT) break;
  }
  freeaddrinfo(addresses);
  return result;
}

static http_result_t
conn_send(http_conn_t *conn, const char *data, size_t size) {
  while(size) {
    ssize_t sent = send(conn->fd, data, size, HTTP_SEND_FLAGS);
    if(sent > 0) {
      data += sent;
      size -= (size_t)sent;
    } else if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      http_result_t result = wait_fd(conn, POLLOUT);
      if(result) return result;
    } else {
      return HTTP_ERR_IO;
    }
  }
  return HTTP_OK;
}

/* Appends at least one byte to the buffer; EOF is reported as HTTP_ERR_INCOMPLETE. */
static http_result_t
conn_fill(http_conn_t *conn) {
  if(conn->pos) {
    memmove(conn->buf, conn->buf + conn->pos, conn->len - conn->pos);
    conn->len -= conn->pos;
    conn->pos = 0;
  }
  if(conn->len == sizeof(conn->buf)) return HTTP_ERR_PROTOCOL;
  while(1) {
    ssize_t count = recv(conn->fd, conn->buf + conn->len, sizeof(conn->buf) - conn->len, 0);
    if(count > 0) {
      conn->len += (size_t)count;
      return HTTP_OK;
    }
    if(!count) return HTTP_ERR_INCOMPLETE;
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      http_result_t result = wait_fd(conn, POLLIN);
      if(result) return result;
    } else {
      return HTTP_ERR_IO;
    }
  }
}

static http_result_t
conn_read(http_conn_t *conn, unsigned char *out, size_t size) {
  while(size) {
    size_t available = conn->len - conn->pos;
    if(!available) {
      http_result_t result = conn_fill(conn);
      if(result) return result;
      continue;
    }
    if(available > size) available = size;
    memcpy(out, conn->buf + conn->pos, available);
    conn->pos += available;
    out += available;
    size -= available;
  }
  return HTTP_OK;
}

/* Reads one CRLF-terminated line of at most size - 1 bytes. */
static http_result_t
conn_line(http_conn_t *conn, char *line, size_t size) {
  while(1) {
    unsigned char *start = conn->buf + conn->pos;
    unsigned char *end = memchr(start, '\n', conn->len - conn->pos);
    if(end) {
      size_t length = (size_t)(end - start);
      if(!length || start[length - 1] != '\r' || length > size) return HTTP_ERR_PROTOCOL;
      memcpy(line, start, length - 1);
      line[length - 1] = 0;
      conn->pos += length + 1;
      return HTTP_OK;
    }
    if(conn->len - conn->pos >= size) return HTTP_ERR_PROTOCOL;
    http_result_t result = conn_fill(conn);
    if(result) return result == HTTP_ERR_INCOMPLETE ? HTTP_ERR_INCOMPLETE : result;
  }
}

static char *
trim(char *value) {
  char *end;

  while(*value == ' ' || *value == '\t') value++;
  end = value + strlen(value);
  while(end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
  return value;
}

static http_result_t
read_chunked(http_conn_t *conn, unsigned char *out, size_t expected) {
  size_t received = 0;
  char chunk_line[128];

  while(received < expected) {
    uint64_t chunk = 0;
    char *p;
    http_result_t result = conn_line(conn, chunk_line, sizeof(chunk_line));

    if(result) return result;
    p = chunk_line;
    if(!isxdigit((unsigned char)*p)) return HTTP_ERR_PROTOCOL;
    for(; isxdigit((unsigned char)*p); p++) {
      unsigned int digit = (unsigned int)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
      if(chunk > (UINT64_MAX >> 4)) return HTTP_ERR_PROTOCOL;
      chunk = chunk << 4 | digit;
    }
    if(*p && *p != ';' && *p != ' ' && *p != '\t') return HTTP_ERR_PROTOCOL;
    if(!chunk) return HTTP_ERR_INCOMPLETE; /* Body ended before the advertised range. */
    if(chunk > expected - received) return HTTP_ERR_RANGE_INVALID;
    if((result = conn_read(conn, out + received, (size_t)chunk))) return result;
    received += (size_t)chunk;
    if((result = conn_line(conn, chunk_line, sizeof(chunk_line)))) return result;
    if(chunk_line[0]) return HTTP_ERR_PROTOCOL;
  }
  return HTTP_OK;
}

static http_result_t
range_exchange(http_conn_t *conn, const http_url_t *url, uint64_t offset,
               size_t length, void *buffer, http_range_response_t *response) {
  char *request = conn->request;
  char *line = conn->line;
  char host[HTTP_HOST_MAX + 8];
  uint64_t requested_last = offset + length - 1;
  uint64_t first = 0, last = 0, total = 0, content_length = 0;
  int have_range = 0, have_length = 0, chunked = 0;
  size_t header_bytes = 0;
  size_t expected;
  int request_size;
  http_result_t result;

  if(url->port == 80) snprintf(host, sizeof(host), "%s", url->host);
  else snprintf(host, sizeof(host), "%s:%u", url->host, url->port);
  request_size = snprintf(request, sizeof(conn->request),
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Range: bytes=%llu-%llu\r\n"
    "Accept-Encoding: identity\r\n"
    "User-Agent: ps5-web-file-manager/" VERSION_TAG "\r\n"
    "Connection: close\r\n\r\n",
    url->target, host, (unsigned long long)offset, (unsigned long long)requested_last);
  if(request_size < 0 || (size_t)request_size >= sizeof(conn->request)) return HTTP_ERR_URL_INVALID;
  if((result = conn_send(conn, request, (size_t)request_size))) return result;

  if((result = conn_line(conn, line, sizeof(conn->line)))) return result == HTTP_ERR_INCOMPLETE ? HTTP_ERR_PROTOCOL : result;
  if(strncmp(line, "HTTP/1.", 7) || !isdigit((unsigned char)line[7]) || line[8] != ' ' ||
     !isdigit((unsigned char)line[9]) || !isdigit((unsigned char)line[10]) ||
     !isdigit((unsigned char)line[11]) || (line[12] && line[12] != ' ')) return HTTP_ERR_PROTOCOL;
  response->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');

  while(1) {
    char *value;
    if((result = conn_line(conn, line, sizeof(conn->line)))) return result == HTTP_ERR_INCOMPLETE ? HTTP_ERR_PROTOCOL : result;
    if(!line[0]) break;
    header_bytes += strlen(line) + 2;
    if(header_bytes > HTTP_HEADER_MAX || !(value = strchr(line, ':'))) return HTTP_ERR_PROTOCOL;
    *value++ = 0;
    value = trim(value);
    if(!strcasecmp(line, "Content-Range")) {
      if(have_range || http_parse_content_range(value, &first, &last, &total)) {
        have_range = -1;
      } else {
        have_range = 1;
      }
    } else if(!strcasecmp(line, "Content-Length")) {
      const char *p = value;
      uint64_t parsed;
      if(parse_u64(&p, &parsed) || *p || (have_length && parsed != content_length)) return HTTP_ERR_PROTOCOL;
      content_length = parsed;
      have_length = 1;
    } else if(!strcasecmp(line, "Transfer-Encoding")) {
      if(strcasecmp(value, "chunked") || chunked) return HTTP_ERR_PROTOCOL;
      chunked = 1;
    } else if(!strcasecmp(line, "Content-Encoding")) {
      if(strcasecmp(value, "identity")) return HTTP_ERR_PROTOCOL;
    } else if(!strcasecmp(line, "Content-Type")) {
      if(!strncasecmp(value, "multipart/byteranges", 20)) return HTTP_ERR_RANGE_INVALID;
    }
  }

  /* Decide from the status line only; a 200 body is never read. */
  if(response->status == 200) return HTTP_ERR_RANGE_IGNORED;
  if(response->status >= 300 && response->status < 400) return HTTP_ERR_REDIRECT;
  if(response->status == 416) return HTTP_ERR_RANGE_INVALID;
  if(response->status != 206) return HTTP_ERR_STATUS;
  if(have_range != 1 || first != offset || last > requested_last ||
     (last < requested_last && last != total - 1)) return HTTP_ERR_RANGE_INVALID;
  expected = (size_t)(last - first + 1);
  if(!chunked && have_length && content_length != expected) return HTTP_ERR_RANGE_INVALID;

  response->total = total;
  result = chunked ? read_chunked(conn, buffer, expected) : conn_read(conn, buffer, expected);
  if(result) return result;
  response->received = expected;
  return HTTP_OK;
}

http_result_t
http_range_get(const http_url_t *url, uint64_t offset, size_t length,
               void *buffer, int timeout_ms, http_range_response_t *response) {
  http_conn_t *conn;
  http_result_t result;

  memset(response, 0, sizeof(*response));
  if(!length || length > HTTP_RANGE_MAX || offset > UINT64_MAX - (length - 1)) return HTTP_ERR_RANGE_INVALID;
  if(!(conn = calloc(1, sizeof(*conn)))) return HTTP_ERR_NO_MEMORY;
  conn->fd = -1;
  clock_gettime(CLOCK_MONOTONIC, &conn->deadline);
  conn->deadline.tv_sec += timeout_ms / 1000;
  conn->deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if(conn->deadline.tv_nsec >= 1000000000L) {
    conn->deadline.tv_sec++;
    conn->deadline.tv_nsec -= 1000000000L;
  }
  result = conn_open(conn, url);
  if(!result) result = range_exchange(conn, url, offset, length, buffer, response);
  if(conn->fd >= 0) close(conn->fd);
  free(conn);
  return result;
}
