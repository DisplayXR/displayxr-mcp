// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Transport abstraction for the MCP server — unix socket on POSIX,
 *         named pipe on Windows (Phase A slice 7).
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mcp_listener;
struct mcp_conn;

/*!
 * Return the current process id as a long. Hides the
 * getpid() / GetCurrentProcessId() platform split so callers don't
 * need <unistd.h> (MSVC has neither).
 */
long
mcp_self_pid(void);

/*!
 * Bind a per-PID listener. Returns NULL on failure.
 *
 * On POSIX this creates `/tmp/displayxr-mcp-<pid>.sock` with 0600 perms.
 * The caller unlinks via mcp_listener_close().
 */
struct mcp_listener *
mcp_listener_open(long pid);

/*!
 * Bind a well-known named listener. Returns NULL on failure.
 *
 * Intended for singleton endpoints like @c "service" where the PID is
 * not a stable identifier for adapters. On POSIX this creates
 * `/tmp/displayxr-mcp-<role>.sock` with 0600 perms; on Windows the
 * pipe is `\\.\pipe\displayxr-mcp-<role>`.
 *
 * @p role must be a short, PID-unambiguous string (non-numeric) — callers
 * must not pass names that could collide with enumerated PID sessions.
 */
struct mcp_listener *
mcp_listener_open_named(const char *role);

/*!
 * Accept one connection. Blocks. Returns NULL when the listener is closed
 * (mcp_listener_close from another thread wakes us via shutdown()).
 */
struct mcp_conn *
mcp_listener_accept(struct mcp_listener *listener);

/*!
 * Close the listener. Safe to call from another thread to unblock accept().
 */
void
mcp_listener_close(struct mcp_listener *listener);

/*!
 * Blocking read of exactly @p len bytes. Returns false on EOF / error.
 */
bool
mcp_conn_read(struct mcp_conn *conn, void *buf, size_t len);

/*!
 * Blocking write of exactly @p len bytes. Returns false on error.
 */
bool
mcp_conn_write(struct mcp_conn *conn, const void *buf, size_t len);

/*!
 * Bounded write of exactly @p len bytes: gives up after @p timeout_ms.
 *
 * On timeout the connection is ABORTED (as by mcp_conn_abort) before
 * returning false — a timed-out write may have landed partially, which
 * corrupts Content-Length framing, so the stream must not be reused.
 * The connection's serve thread notices and unwinds the slot.
 *
 * For fire-and-forget traffic (notifications) sent from threads the
 * embedder cannot afford to stall: a peer that stops reading costs the
 * caller at most one timeout, then loses its connection — it never
 * wedges the embedding process (the DisplayXR runtime embeds this on
 * app main threads inside OpenXR calls; see displayxr-runtime#928).
 */
bool
mcp_conn_write_bounded(struct mcp_conn *conn, const void *buf, size_t len, uint32_t timeout_ms);

/*!
 * Abort the connection: wake any thread blocked in mcp_conn_read /
 * mcp_conn_write on it (they return false) and make all subsequent
 * I/O on it fail. Does NOT free — ownership stays with whoever calls
 * mcp_conn_close. Safe to call from any thread; the multi-client
 * server uses it at stop to unwind per-connection serve threads.
 */
void
mcp_conn_abort(struct mcp_conn *conn);

/*!
 * Close and free the connection.
 */
void
mcp_conn_close(struct mcp_conn *conn);

/*!
 * Connect to a per-PID listener as a client (used by the displayxr-mcp adapter).
 */
struct mcp_conn *
mcp_conn_connect(long pid);

/*!
 * Connect to a well-known named listener (e.g. @c "service").
 */
struct mcp_conn *
mcp_conn_connect_named(const char *role);

/*!
 * Raw fd / handle for poll()-style multiplexing by the adapter.
 * Returns -1 when unavailable (e.g. Windows named-pipe slice).
 */
int
mcp_conn_fd(struct mcp_conn *conn);

/*!
 * Enumerate running MCP sessions by scanning for socket files.
 * Fills @p out_pids (up to @p cap entries), returns count found.
 */
size_t
mcp_enumerate_sessions(long *out_pids, size_t cap);

#ifdef __cplusplus
}
#endif
