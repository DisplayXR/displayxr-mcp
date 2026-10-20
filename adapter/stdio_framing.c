// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Agent-side (stdio) message framing — see stdio_framing.h.
 */

#include "stdio_framing.h"

#include "displayxr_mcp/mcp_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define CL_KEY "content-length:"
#define CL_KEY_LEN 15

// ---------------------------------------------------------------- reader

void
dxr_stdio_reader_init(struct dxr_stdio_reader *r, dxr_stdio_read_fn fn, void *ctx)
{
	memset(r, 0, sizeof(*r));
	r->read = fn;
	r->ctx = ctx;
}

void
dxr_stdio_reader_fini(struct dxr_stdio_reader *r)
{
	free(r->buf);
	r->buf = NULL;
	r->len = r->pos = r->cap = 0;
}

// Pull more bytes into the buffer. Returns false at EOF / error / cap.
static bool
fill(struct dxr_stdio_reader *r)
{
	if (r->eof) {
		return false;
	}
	// Compact consumed bytes away before growing.
	if (r->pos > 0) {
		memmove(r->buf, r->buf + r->pos, r->len - r->pos);
		r->len -= r->pos;
		r->pos = 0;
	}
	if (r->len == r->cap) {
		size_t ncap = r->cap ? r->cap * 2 : 4096;
		if (ncap > DXR_STDIO_MAX_MESSAGE_BYTES + 4096) {
			return false; // oversized message
		}
		char *nb = realloc(r->buf, ncap);
		if (nb == NULL) {
			return false;
		}
		r->buf = nb;
		r->cap = ncap;
	}
	long n = r->read(r->ctx, r->buf + r->len, r->cap - r->len);
	if (n <= 0) {
		r->eof = true;
		return false;
	}
	r->len += (size_t)n;
	return true;
}

static int
lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool
is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Decide the framing from the first non-whitespace bytes. Returns false
// if the stream ended before anything was seen.
static bool
detect(struct dxr_stdio_reader *r)
{
	for (;;) {
		// Skip leading whitespace and a UTF-8 BOM.
		while (r->pos < r->len && is_ws(r->buf[r->pos])) {
			r->pos++;
		}
		if (r->len - r->pos >= 3 && (unsigned char)r->buf[r->pos] == 0xEF &&
		    (unsigned char)r->buf[r->pos + 1] == 0xBB && (unsigned char)r->buf[r->pos + 2] == 0xBF) {
			r->pos += 3;
			continue;
		}
		size_t avail = r->len - r->pos;
		// A BOM split across reads: wait for the rest before deciding.
		static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
		bool partial_bom = avail > 0 && avail < 3 && memcmp(r->buf + r->pos, bom, avail) == 0;
		if (avail > 0 && !partial_bom) {
			// Compare as much of the header key as we have so far.
			size_t n = avail < CL_KEY_LEN ? avail : CL_KEY_LEN;
			bool prefix_match = true;
			for (size_t i = 0; i < n; i++) {
				if (lower((unsigned char)r->buf[r->pos + i]) != CL_KEY[i]) {
					prefix_match = false;
					break;
				}
			}
			if (!prefix_match) {
				r->framing = DXR_STDIO_FRAMING_NDJSON;
				return true;
			}
			if (n == CL_KEY_LEN) {
				r->framing = DXR_STDIO_FRAMING_CONTENT_LENGTH;
				return true;
			}
		}
		if (!fill(r)) {
			// EOF mid-detection: whatever is left is an ndjson fragment.
			if (r->len > r->pos) {
				r->framing = DXR_STDIO_FRAMING_NDJSON;
				return true;
			}
			return false;
		}
	}
}

static char *
take(struct dxr_stdio_reader *r, size_t start, size_t len, size_t *out_len)
{
	char *out = malloc(len + 1);
	if (out == NULL) {
		return NULL;
	}
	memcpy(out, r->buf + start, len);
	out[len] = '\0';
	if (out_len != NULL) {
		*out_len = len;
	}
	return out;
}

static char *
next_ndjson(struct dxr_stdio_reader *r, size_t *out_len)
{
	size_t scan = r->pos;
	for (;;) {
		char *nl = r->len > scan ? memchr(r->buf + scan, '\n', r->len - scan) : NULL;
		if (nl != NULL) {
			size_t start = r->pos;
			size_t end = (size_t)(nl - r->buf);
			r->pos = end + 1;
			// Trim surrounding whitespace (incl. a CRLF terminator).
			while (start < end && is_ws(r->buf[start])) {
				start++;
			}
			while (end > start && is_ws(r->buf[end - 1])) {
				end--;
			}
			if (end == start) {
				scan = r->pos; // blank line — keep going
				continue;
			}
			if (end - start > DXR_STDIO_MAX_MESSAGE_BYTES) {
				return NULL;
			}
			return take(r, start, end - start, out_len);
		}
		size_t scanned = r->len - r->pos; // relative: fill() may compact
		if (!fill(r)) {
			// EOF: accept a final unterminated line.
			size_t start = r->pos, end = r->len;
			while (start < end && is_ws(r->buf[start])) {
				start++;
			}
			while (end > start && is_ws(r->buf[end - 1])) {
				end--;
			}
			r->pos = r->len;
			if (end == start || !r->eof) {
				return NULL;
			}
			return take(r, start, end - start, out_len);
		}
		scan = r->pos + scanned;
	}
}

// Read one header line (through '\n') starting at r->pos. Returns its
// length incl. terminator, or 0 on EOF / overlong header.
static size_t
header_line(struct dxr_stdio_reader *r)
{
	for (;;) {
		char *nl = r->len > r->pos ? memchr(r->buf + r->pos, '\n', r->len - r->pos) : NULL;
		if (nl != NULL) {
			return (size_t)(nl - (r->buf + r->pos)) + 1;
		}
		if (r->len - r->pos > 1024 || !fill(r)) {
			return 0;
		}
	}
}

static char *
next_content_length(struct dxr_stdio_reader *r, size_t *out_len)
{
	size_t content_length = 0;
	bool have_length = false;
	bool any_header = false;
	for (;;) {
		size_t n = header_line(r);
		if (n == 0) {
			return NULL;
		}
		const char *line = r->buf + r->pos;
		bool blank = (n == 1) || (n == 2 && line[0] == '\r');
		if (blank) {
			r->pos += n;
			if (any_header) {
				break;
			}
			continue; // stray blank line between frames
		}
		any_header = true;
		if (n > CL_KEY_LEN) {
			bool match = true;
			for (size_t i = 0; i < CL_KEY_LEN; i++) {
				if (lower((unsigned char)line[i]) != CL_KEY[i]) {
					match = false;
					break;
				}
			}
			if (match) {
				content_length = (size_t)strtoul(line + CL_KEY_LEN, NULL, 10);
				have_length = true;
			}
		}
		r->pos += n;
	}
	if (!have_length || content_length == 0 || content_length > DXR_STDIO_MAX_MESSAGE_BYTES) {
		return NULL;
	}
	while (r->len - r->pos < content_length) {
		if (!fill(r)) {
			return NULL;
		}
	}
	char *out = take(r, r->pos, content_length, out_len);
	r->pos += content_length;
	return out;
}

char *
dxr_stdio_reader_next(struct dxr_stdio_reader *r, size_t *out_len)
{
	if (r->framing == DXR_STDIO_FRAMING_UNKNOWN && !detect(r)) {
		return NULL;
	}
	if (r->framing == DXR_STDIO_FRAMING_CONTENT_LENGTH) {
		return next_content_length(r, out_len);
	}
	return next_ndjson(r, out_len);
}

// ---------------------------------------------------------------- writer

char *
dxr_stdio_encode(enum dxr_stdio_framing framing, const char *body, size_t len, size_t *out_len)
{
	if (framing == DXR_STDIO_FRAMING_CONTENT_LENGTH) {
		char header[64];
		int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
		if (hlen <= 0) {
			return NULL;
		}
		char *out = malloc((size_t)hlen + len + 1);
		if (out == NULL) {
			return NULL;
		}
		memcpy(out, header, (size_t)hlen);
		memcpy(out + hlen, body, len);
		out[hlen + len] = '\0';
		*out_len = (size_t)hlen + len;
		return out;
	}
	char *out = malloc(len + 2);
	if (out == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < len; i++) {
		char c = body[i];
		out[i] = (c == '\n' || c == '\r') ? ' ' : c;
	}
	out[len] = '\n';
	out[len + 1] = '\0';
	*out_len = len + 1;
	return out;
}

// ---------------------------------------------------------------- latch

void
dxr_stdio_framing_latch_init(struct dxr_stdio_framing_latch *l)
{
	pthread_mutex_init(&l->mutex, NULL);
	pthread_cond_init(&l->cond, NULL);
	l->framing = DXR_STDIO_FRAMING_UNKNOWN;
}

void
dxr_stdio_framing_latch_set(struct dxr_stdio_framing_latch *l, enum dxr_stdio_framing framing)
{
	pthread_mutex_lock(&l->mutex);
	if (l->framing == DXR_STDIO_FRAMING_UNKNOWN) {
		l->framing = framing;
		pthread_cond_broadcast(&l->cond);
	}
	pthread_mutex_unlock(&l->mutex);
}

enum dxr_stdio_framing
dxr_stdio_framing_latch_wait(struct dxr_stdio_framing_latch *l)
{
	pthread_mutex_lock(&l->mutex);
	while (l->framing == DXR_STDIO_FRAMING_UNKNOWN) {
		pthread_cond_wait(&l->cond, &l->mutex);
	}
	enum dxr_stdio_framing f = l->framing;
	pthread_mutex_unlock(&l->mutex);
	return f;
}

// ---------------------------------------------------------------- stdio

long
dxr_stdio_read_stdin(void *ctx, void *buf, size_t cap)
{
	(void)ctx;
#ifdef _WIN32
	unsigned n = cap > 0x40000000u ? 0x40000000u : (unsigned)cap;
	return (long)_read(_fileno(stdin), buf, n);
#else
	return (long)read(STDIN_FILENO, buf, cap);
#endif
}

bool
dxr_stdio_write_stdout(const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
#ifdef _WIN32
		unsigned chunk = len > 0x40000000u ? 0x40000000u : (unsigned)len;
		int w = _write(_fileno(stdout), p, chunk);
#else
		ssize_t w = write(STDOUT_FILENO, p, len);
#endif
		if (w <= 0) {
			return false;
		}
		p += w;
		len -= (size_t)w;
	}
	return true;
}

// ---------------------------------------------------------------- conn (Content-Length)

static bool
conn_read_line(struct mcp_conn *conn, char *buf, size_t cap)
{
	size_t n = 0;
	while (n + 1 < cap) {
		char c;
		if (!mcp_conn_read(conn, &c, 1)) {
			return false;
		}
		buf[n++] = c;
		if (c == '\n') {
			buf[n] = '\0';
			return true;
		}
	}
	return false;
}

char *
dxr_conn_read_frame(struct mcp_conn *conn, size_t *out_len)
{
	char line[256];
	size_t content_length = 0;
	bool have_length = false;
	while (conn_read_line(conn, line, sizeof(line))) {
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		bool match = strlen(line) > CL_KEY_LEN;
		for (size_t i = 0; match && i < CL_KEY_LEN; i++) {
			if (lower((unsigned char)line[i]) != CL_KEY[i]) {
				match = false;
			}
		}
		if (match) {
			content_length = (size_t)strtoul(line + CL_KEY_LEN, NULL, 10);
			have_length = true;
		}
	}
	if (!have_length || content_length == 0 || content_length > DXR_STDIO_MAX_MESSAGE_BYTES) {
		return NULL;
	}
	char *body = malloc(content_length + 1);
	if (body == NULL) {
		return NULL;
	}
	if (!mcp_conn_read(conn, body, content_length)) {
		free(body);
		return NULL;
	}
	body[content_length] = '\0';
	if (out_len != NULL) {
		*out_len = content_length;
	}
	return body;
}

bool
dxr_conn_write_frame(struct mcp_conn *conn, const char *body, size_t len)
{
	char header[64];
	int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
	return hlen > 0 && mcp_conn_write(conn, header, (size_t)hlen) && mcp_conn_write(conn, body, len);
}
