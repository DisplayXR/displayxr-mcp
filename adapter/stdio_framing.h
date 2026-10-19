// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Agent-side (stdio) message framing for the displayxr-mcp adapter.
 *
 * The MCP stdio transport is **newline-delimited JSON**: one JSON-RPC
 * message per line, UTF-8, no embedded newlines. That is what Claude
 * Code and every other spec-conformant MCP client speaks.
 *
 * Adapters up to v0.5.5 only spoke LSP-style `Content-Length:` header
 * framing on stdio, so spec-conformant clients hung at connect. The
 * reader below auto-detects the framing from the first bytes the client
 * sends (a leading `Content-Length:` header selects the legacy framing,
 * anything else is newline-delimited JSON) and the writer replies in the
 * same framing. The legacy framing is kept for one release for any
 * existing client that uses it.
 *
 * The adapter ↔ in-process server leg (unix socket / named pipe) is a
 * private wire and stays Content-Length-framed: changing it would break
 * every runtime / shell already built against an older framework.
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mcp_conn;

//! Largest single message accepted in either framing.
#define DXR_STDIO_MAX_MESSAGE_BYTES (16 * 1024 * 1024)

enum dxr_stdio_framing
{
	DXR_STDIO_FRAMING_UNKNOWN = 0,
	//! MCP stdio transport: one JSON-RPC message per `\n`-terminated line.
	DXR_STDIO_FRAMING_NDJSON,
	//! Legacy LSP-style `Content-Length: N\r\n\r\n<body>` (adapters <= v0.5.5).
	DXR_STDIO_FRAMING_CONTENT_LENGTH,
};

/*!
 * Byte source. Returns the number of bytes read (> 0), or <= 0 on EOF /
 * error. May return fewer bytes than asked for.
 */
typedef long (*dxr_stdio_read_fn)(void *ctx, void *buf, size_t cap);

struct dxr_stdio_reader
{
	dxr_stdio_read_fn read;
	void *ctx;
	char *buf;
	size_t len; //!< valid bytes in buf
	size_t pos; //!< consumed bytes in buf
	size_t cap;
	bool eof;
	//! Detected on the first message, fixed afterwards.
	enum dxr_stdio_framing framing;
};

void
dxr_stdio_reader_init(struct dxr_stdio_reader *r, dxr_stdio_read_fn fn, void *ctx);

void
dxr_stdio_reader_fini(struct dxr_stdio_reader *r);

/*!
 * Read the next message body. Returns a NUL-terminated malloc'd buffer
 * (caller frees) and its byte length in @p out_len, or NULL on EOF, read
 * error, or an unrecoverable framing error. Blank ndjson lines are
 * skipped. After the first successful call, @c r->framing holds the
 * detected framing.
 */
char *
dxr_stdio_reader_next(struct dxr_stdio_reader *r, size_t *out_len);

/*!
 * Encode @p body for the given framing. Returns a malloc'd buffer (caller
 * frees) with its length in @p out_len. For ndjson, any raw CR/LF in the
 * body is replaced by a space (valid JSON can only carry them as
 * insignificant whitespace — inside strings they must already be escaped)
 * and a single `\n` terminator is appended.
 */
char *
dxr_stdio_encode(enum dxr_stdio_framing framing, const char *body, size_t len, size_t *out_len);

/*!
 * One-shot latch publishing the detected framing from the stdin reader
 * thread to whichever threads write to stdout. Writers block until the
 * client's first message has fixed the framing (a server-initiated
 * notification before that would otherwise have to guess).
 */
struct dxr_stdio_framing_latch
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	enum dxr_stdio_framing framing;
};

void
dxr_stdio_framing_latch_init(struct dxr_stdio_framing_latch *l);

//! First call wins; later calls are ignored.
void
dxr_stdio_framing_latch_set(struct dxr_stdio_framing_latch *l, enum dxr_stdio_framing framing);

enum dxr_stdio_framing
dxr_stdio_framing_latch_wait(struct dxr_stdio_framing_latch *l);

//! dxr_stdio_read_fn over the process's stdin (ctx unused).
long
dxr_stdio_read_stdin(void *ctx, void *buf, size_t cap);

//! Write all of @p buf to the process's stdout. Returns false on error.
bool
dxr_stdio_write_stdout(const void *buf, size_t len);

/*!
 * Content-Length frame I/O on the adapter ↔ server connection (the
 * private socket / pipe leg). Read returns a NUL-terminated malloc'd body
 * or NULL on EOF / error.
 */
char *
dxr_conn_read_frame(struct mcp_conn *conn, size_t *out_len);

bool
dxr_conn_write_frame(struct mcp_conn *conn, const char *body, size_t len);

#ifdef __cplusplus
}
#endif
