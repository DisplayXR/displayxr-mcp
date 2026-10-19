// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*
 * Unit test for the adapter's agent-side stdio framing
 * (adapter/stdio_framing.c): auto-detection of newline-delimited JSON
 * (the MCP stdio transport) vs the legacy Content-Length framing, UTF-8
 * bodies, and messages split across reads at every possible byte
 * boundary. Pure in-memory — no sockets, no fork — so it is portable.
 */

#include "stdio_framing.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void
check(bool cond, const char *label)
{
	if (cond) {
		printf("  ok: %s\n", label);
	} else {
		fprintf(stderr, "FAIL: %s\n", label);
		g_failures++;
	}
}

// In-memory source handing out at most `chunk` bytes per read; when
// `split_at` is non-zero the first read stops exactly there.
struct mem_src
{
	const char *data;
	size_t len;
	size_t off;
	size_t chunk;
	size_t split_at;
};

static long
mem_read(void *ctx, void *buf, size_t cap)
{
	struct mem_src *s = ctx;
	if (s->off >= s->len) {
		return 0;
	}
	size_t n = s->len - s->off;
	if (s->chunk > 0 && n > s->chunk) {
		n = s->chunk;
	}
	if (s->split_at > s->off && s->off + n > s->split_at) {
		n = s->split_at - s->off;
	}
	if (n > cap) {
		n = cap;
	}
	memcpy(buf, s->data + s->off, n);
	s->off += n;
	return (long)n;
}

// Read every message from @p data; compare against @p expect[0..n).
static bool
read_all(const char *data, size_t chunk, size_t split_at, enum dxr_stdio_framing want_framing,
         const char *const *expect, size_t n)
{
	struct mem_src src = {.data = data, .len = strlen(data), .chunk = chunk, .split_at = split_at};
	struct dxr_stdio_reader r;
	dxr_stdio_reader_init(&r, mem_read, &src);
	bool ok = true;
	for (size_t i = 0; i < n && ok; i++) {
		size_t len = 0;
		char *body = dxr_stdio_reader_next(&r, &len);
		if (body == NULL || len != strlen(expect[i]) || strcmp(body, expect[i]) != 0) {
			fprintf(stderr, "    msg %zu (chunk=%zu split=%zu): got '%s'\n", i, chunk, split_at,
			        body ? body : "(null)");
			ok = false;
		}
		free(body);
	}
	if (ok) {
		size_t len = 0;
		char *extra = dxr_stdio_reader_next(&r, &len);
		if (extra != NULL) {
			fprintf(stderr, "    unexpected extra message '%s'\n", extra);
			ok = false;
		}
		free(extra);
	}
	if (ok && r.framing != want_framing) {
		fprintf(stderr, "    framing %d, want %d\n", (int)r.framing, (int)want_framing);
		ok = false;
	}
	dxr_stdio_reader_fini(&r);
	return ok;
}

// Every chunk size 1..len and every single split point.
static bool
read_all_splits(const char *data, enum dxr_stdio_framing want, const char *const *expect, size_t n)
{
	size_t len = strlen(data);
	for (size_t chunk = 1; chunk <= len; chunk++) {
		if (!read_all(data, chunk, 0, want, expect, n)) {
			return false;
		}
	}
	for (size_t split = 1; split < len; split++) {
		if (!read_all(data, 0, split, want, expect, n)) {
			return false;
		}
	}
	return true;
}

// "你好，世界" — 3-byte UTF-8 sequences, so byte splits land mid-character.
#define ZH "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c"

static const char *const MSGS[] = {
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
    "\"arguments\":{\"text\":\"" ZH "\"}}}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
};

int
main(void)
{
	printf("--- ndjson ---\n");
	{
		char data[1024];
		snprintf(data, sizeof(data), "%s\n%s\n%s\n", MSGS[0], MSGS[1], MSGS[2]);
		check(read_all(data, 0, 0, DXR_STDIO_FRAMING_NDJSON, MSGS, 3), "ndjson: three messages, one read");
		check(read_all_splits(data, DXR_STDIO_FRAMING_NDJSON, MSGS, 3),
		      "ndjson: every chunk size + every split point (incl. mid-UTF-8)");

		snprintf(data, sizeof(data), "\r\n  %s\r\n\r\n%s\r\n\n%s\r\n", MSGS[0], MSGS[1], MSGS[2]);
		check(read_all_splits(data, DXR_STDIO_FRAMING_NDJSON, MSGS, 3),
		      "ndjson: CRLF terminators + blank lines tolerated");

		snprintf(data, sizeof(data), "\xef\xbb\xbf%s\n%s\n%s", MSGS[0], MSGS[1], MSGS[2]);
		check(read_all_splits(data, DXR_STDIO_FRAMING_NDJSON, MSGS, 3),
		      "ndjson: leading BOM + unterminated final line at EOF");
	}

	printf("--- Content-Length (legacy) ---\n");
	{
		char data[2048];
		size_t off = 0;
		for (int i = 0; i < 3; i++) {
			// Mixed header case + an extra header on the middle frame.
			const char *key = i == 1 ? "content-length" : "Content-Length";
			const char *extra = i == 1 ? "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n" : "";
			off += (size_t)snprintf(data + off, sizeof(data) - off, "%s: %zu\r\n%s\r\n%s", key,
			                        strlen(MSGS[i]), extra, MSGS[i]);
		}
		check(read_all(data, 0, 0, DXR_STDIO_FRAMING_CONTENT_LENGTH, MSGS, 3),
		      "content-length: three frames, one read");
		check(read_all_splits(data, DXR_STDIO_FRAMING_CONTENT_LENGTH, MSGS, 3),
		      "content-length: every chunk size + every split point (header key split too)");
	}

	printf("--- encode ---\n");
	{
		size_t n = 0;
		char *out = dxr_stdio_encode(DXR_STDIO_FRAMING_NDJSON, "{\"a\":\n1}", 8, &n);
		check(out != NULL && n == 9 && strcmp(out, "{\"a\": 1}\n") == 0,
		      "ndjson encode: raw newline flattened, single \\n terminator");
		free(out);

		const char *body = "{\"t\":\"" ZH "\"}";
		out = dxr_stdio_encode(DXR_STDIO_FRAMING_CONTENT_LENGTH, body, strlen(body), &n);
		char want[128];
		snprintf(want, sizeof(want), "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
		check(out != NULL && strlen(body) == 23 && strcmp(out, want) == 0,
		      "content-length encode: length counts UTF-8 bytes (23), not characters");
		free(out);

		// Round trip each framing through the reader.
		for (int f = DXR_STDIO_FRAMING_NDJSON; f <= DXR_STDIO_FRAMING_CONTENT_LENGTH; f++) {
			char data[2048] = {0};
			for (int i = 0; i < 3; i++) {
				char *enc = dxr_stdio_encode((enum dxr_stdio_framing)f, MSGS[i], strlen(MSGS[i]), &n);
				strncat(data, enc, sizeof(data) - strlen(data) - 1);
				free(enc);
			}
			check(read_all_splits(data, (enum dxr_stdio_framing)f, MSGS, 3),
			      f == DXR_STDIO_FRAMING_NDJSON ? "round trip: ndjson" : "round trip: content-length");
		}
	}

	printf("--- edge cases ---\n");
	{
		struct mem_src src = {.data = "", .len = 0};
		struct dxr_stdio_reader r;
		dxr_stdio_reader_init(&r, mem_read, &src);
		size_t n = 0;
		check(dxr_stdio_reader_next(&r, &n) == NULL && r.framing == DXR_STDIO_FRAMING_UNKNOWN,
		      "empty stdin: NULL, framing stays unknown");
		dxr_stdio_reader_fini(&r);

		const char *trunc = "Content-Length: 50\r\n\r\n{\"short\":1}";
		src = (struct mem_src){.data = trunc, .len = strlen(trunc)};
		dxr_stdio_reader_init(&r, mem_read, &src);
		check(dxr_stdio_reader_next(&r, &n) == NULL, "content-length: truncated body -> NULL");
		dxr_stdio_reader_fini(&r);
	}

	if (g_failures == 0) {
		printf("=== stdio framing test PASSED ===\n");
		return 0;
	}
	printf("=== stdio framing test FAILED (%d) ===\n", g_failures);
	return 1;
}
