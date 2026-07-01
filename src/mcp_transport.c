// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  POSIX unix-socket transport for the MCP server.
 * @ingroup aux_util
 */


#include "displayxr_mcp/mcp_transport.h"
#include "displayxr_mcp/os_compat.h"
#include "displayxr_mcp/os_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define LOG_PFX "[mcp-transport] "
#define SOCK_PREFIX "/tmp/displayxr-mcp-"
#define SOCK_SUFFIX ".sock"

#ifndef _WIN32

long
mcp_self_pid(void)
{
	return (long)getpid();
}

struct mcp_listener
{
	int fd;
	char path[128];
};

struct mcp_conn
{
	int fd;
};

/*!
 * A peer that disconnects while we write must surface as a failed
 * write, not a process-killing SIGPIPE — the server lives inside the
 * embedding app (a handle app, the shell), and a library must never
 * touch process-global signal disposition. macOS: per-fd SO_NOSIGPIPE.
 * Linux: MSG_NOSIGNAL per send() in mcp_conn_write.
 */
static void
suppress_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int on = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
	(void)fd;
#endif
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void
build_sock_path(char *out, size_t cap, long pid)
{
	snprintf(out, cap, "%s%ld%s", SOCK_PREFIX, (long)pid, SOCK_SUFFIX);
}

static void
build_sock_path_named(char *out, size_t cap, const char *role)
{
	snprintf(out, cap, "%s%s%s", SOCK_PREFIX, role, SOCK_SUFFIX);
}

static struct mcp_listener *
listener_open_path(const char *path)
{
	struct mcp_listener *l = MCP_TYPED_CALLOC(struct mcp_listener);
	l->fd = -1;
	snprintf(l->path, sizeof(l->path), "%s", path);

	// Always unlink any stale socket; we own the named path.
	(void)unlink(l->path);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		MCP_LOG_W(LOG_PFX "socket() failed: %s", strerror(errno));
		free(l);
		return NULL;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", l->path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		MCP_LOG_W(LOG_PFX "bind(%s) failed: %s", l->path, strerror(errno));
		close(fd);
		free(l);
		return NULL;
	}

	(void)chmod(l->path, 0600);

	if (listen(fd, 4) != 0) {
		MCP_LOG_W(LOG_PFX "listen() failed: %s", strerror(errno));
		close(fd);
		(void)unlink(l->path);
		free(l);
		return NULL;
	}

	l->fd = fd;
	MCP_LOG_I(LOG_PFX "listening on %s", l->path);
	return l;
}

struct mcp_listener *
mcp_listener_open(long pid)
{
	char path[128];
	build_sock_path(path, sizeof(path), pid);
	return listener_open_path(path);
}

struct mcp_listener *
mcp_listener_open_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char path[128];
	build_sock_path_named(path, sizeof(path), role);
	return listener_open_path(path);
}

struct mcp_conn *
mcp_listener_accept(struct mcp_listener *listener)
{
	if (listener == NULL || listener->fd < 0) {
		return NULL;
	}
	int cfd = accept(listener->fd, NULL, NULL);
	if (cfd < 0) {
		return NULL;
	}
	suppress_sigpipe(cfd);
	struct mcp_conn *c = MCP_TYPED_CALLOC(struct mcp_conn);
	c->fd = cfd;
	return c;
}

void
mcp_listener_close(struct mcp_listener *listener)
{
	if (listener == NULL) {
		return;
	}
	if (listener->fd >= 0) {
		// shutdown() wakes a blocking accept() in the server thread.
		(void)shutdown(listener->fd, SHUT_RDWR);
		close(listener->fd);
	}
	(void)unlink(listener->path);
	free(listener);
}

bool
mcp_conn_read(struct mcp_conn *conn, void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(conn->fd, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

bool
mcp_conn_write(struct mcp_conn *conn, const void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	const uint8_t *p = buf;
	while (len > 0) {
		// send(MSG_NOSIGNAL), not write(): see suppress_sigpipe().
		ssize_t n = send(conn->fd, p, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

void
mcp_conn_abort(struct mcp_conn *conn)
{
	if (conn == NULL || conn->fd < 0) {
		return;
	}
	// shutdown() wakes any thread blocked in read()/write() on this fd
	// (read returns 0, write fails) and poisons all subsequent I/O —
	// without freeing, so the owning thread can still mcp_conn_close().
	(void)shutdown(conn->fd, SHUT_RDWR);
}

void
mcp_conn_close(struct mcp_conn *conn)
{
	if (conn == NULL) {
		return;
	}
	if (conn->fd >= 0) {
		// shutdown() before close() unblocks any thread currently
		// inside read()/write() on this fd — the adapter's down pump
		// thread is one such caller, and the server thread's read in
		// serve() is another. On macOS close() alone happens to wake
		// the reader; on Linux it does NOT (POSIX leaves this
		// undefined and Linux keeps the underlying socket alive
		// until the in-flight syscall returns), which is why the
		// Linux CI smoke test hung forever in pthread_join.
		(void)shutdown(conn->fd, SHUT_RDWR);
		close(conn->fd);
	}
	free(conn);
}

int
mcp_conn_fd(struct mcp_conn *conn)
{
	return conn != NULL ? conn->fd : -1;
}

static struct mcp_conn *
conn_connect_path(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return NULL;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return NULL;
	}
	suppress_sigpipe(fd);
	struct mcp_conn *c = MCP_TYPED_CALLOC(struct mcp_conn);
	c->fd = fd;
	return c;
}

struct mcp_conn *
mcp_conn_connect(long pid)
{
	char path[128];
	build_sock_path(path, sizeof(path), pid);
	return conn_connect_path(path);
}

struct mcp_conn *
mcp_conn_connect_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char path[128];
	build_sock_path_named(path, sizeof(path), role);
	return conn_connect_path(path);
}

size_t
mcp_enumerate_sessions(long *out_pids, size_t cap)
{
	size_t n = 0;
	DIR *d = opendir("/tmp");
	if (d == NULL) {
		return 0;
	}
	const char *prefix = "displayxr-mcp-";
	const char *suffix = ".sock";
	size_t plen = strlen(prefix);
	size_t slen = strlen(suffix);
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < cap) {
		const char *name = de->d_name;
		size_t nlen = strlen(name);
		if (nlen <= plen + slen) {
			continue;
		}
		if (strncmp(name, prefix, plen) != 0) {
			continue;
		}
		if (strcmp(name + nlen - slen, suffix) != 0) {
			continue;
		}
		long pid = strtol(name + plen, NULL, 10);
		if (pid <= 0) {
			continue;
		}
		out_pids[n++] = (long)pid;
	}
	closedir(d);
	return n;
}

#else // _WIN32 — named pipe transport
//
// Uses \\.\pipe\displayxr-mcp-<pid> with PIPE_TYPE_BYTE; framing (Content-
// Length) is handled at a higher layer just like on POSIX. Enumeration
// uses FindFirstFile over \\.\pipe\* (Named Pipe filesystem).

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

long
mcp_self_pid(void)
{
	return (long)GetCurrentProcessId();
}

#define PIPE_PREFIX "\\\\.\\pipe\\displayxr-mcp-"

struct mcp_listener
{
	HANDLE pipe; //!< The *pending* (not-yet-connected) instance.
	char name[128];
	volatile LONG closed;
};

struct mcp_conn
{
	HANDLE pipe;
	volatile LONG aborted;
};

static void
build_pipe_name(char *out, size_t cap, long pid)
{
	snprintf(out, cap, PIPE_PREFIX "%ld", (long)pid);
}

static void
build_pipe_name_named(char *out, size_t cap, const char *role)
{
	snprintf(out, cap, PIPE_PREFIX "%s", role);
}

/*!
 * Create one pipe instance for @p name. Multi-instance
 * (PIPE_UNLIMITED_INSTANCES) so several clients — e.g. the workspace
 * aggregator plus a manual `--target pid:N` debug session — can be
 * connected at once; accept() hands the connected instance to the conn
 * and immediately creates a fresh instance to keep listening on.
 */
static HANDLE
create_pipe_instance(const char *name)
{
	return CreateNamedPipeA(
	    name,
	    PIPE_ACCESS_DUPLEX,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
	    PIPE_UNLIMITED_INSTANCES,
	    65536, // Out buffer.
	    65536, // In buffer.
	    0,
	    NULL);
}

static struct mcp_listener *
listener_open_pipe_name(const char *name)
{
	struct mcp_listener *l = MCP_TYPED_CALLOC(struct mcp_listener);
	snprintf(l->name, sizeof(l->name), "%s", name);
	l->pipe = create_pipe_instance(l->name);
	if (l->pipe == INVALID_HANDLE_VALUE) {
		MCP_LOG_W(LOG_PFX "CreateNamedPipe(%s) failed: %lu", l->name, GetLastError());
		free(l);
		return NULL;
	}
	MCP_LOG_I(LOG_PFX "listening on %s", l->name);
	return l;
}

struct mcp_listener *
mcp_listener_open_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char name[128];
	build_pipe_name_named(name, sizeof(name), role);
	return listener_open_pipe_name(name);
}

struct mcp_listener *
mcp_listener_open(long pid)
{
	char name[128];
	build_pipe_name(name, sizeof(name), pid);
	return listener_open_pipe_name(name);
}

struct mcp_conn *
mcp_listener_accept(struct mcp_listener *listener)
{
	if (listener == NULL) {
		return NULL;
	}
	BOOL ok = ConnectNamedPipe(listener->pipe, NULL);
	if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
		return NULL;
	}
	if (InterlockedCompareExchange(&listener->closed, 0, 0)) {
		return NULL;
	}

	// Hand the connected instance to the conn (it owns the handle now)
	// and stand up a fresh instance for the next client. If instance
	// creation fails we keep serving the connected client — the next
	// accept() will fail and the server stops listening, which beats
	// dropping the live connection.
	HANDLE connected = listener->pipe;
	listener->pipe = create_pipe_instance(listener->name);
	if (listener->pipe == INVALID_HANDLE_VALUE) {
		MCP_LOG_W(LOG_PFX "CreateNamedPipe(%s) re-arm failed: %lu", listener->name, GetLastError());
	}

	struct mcp_conn *c = MCP_TYPED_CALLOC(struct mcp_conn);
	c->pipe = connected;
	return c;
}

void
mcp_listener_close(struct mcp_listener *listener)
{
	if (listener == NULL) {
		return;
	}
	InterlockedExchange(&listener->closed, 1);
	if (listener->pipe != INVALID_HANDLE_VALUE) {
		// The server thread is parked in synchronous ConnectNamedPipe
		// on this handle (the listener pipe is created without
		// FILE_FLAG_OVERLAPPED). On Windows, CloseHandle from another
		// thread does NOT wake a synchronous ConnectNamedPipe — the
		// kernel keeps the I/O parked and pthread_join in
		// mcp_server_stop deadlocks. CancelIoEx cancels any pending
		// I/O on the handle from any thread; the parked
		// ConnectNamedPipe returns with ERROR_OPERATION_ABORTED and
		// the server thread exits cleanly. DisconnectNamedPipe alone
		// is a no-op when no client is connected.
		CancelIoEx(listener->pipe, NULL);
		DisconnectNamedPipe(listener->pipe);
		CloseHandle(listener->pipe);
	}
	free(listener);
}

bool
mcp_conn_read(struct mcp_conn *conn, void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	char *p = buf;
	while (len > 0) {
		if (InterlockedCompareExchange(&conn->aborted, 0, 0)) {
			return false; // Aborted by another thread; don't re-issue I/O.
		}
		OVERLAPPED ov = {0};
		ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		DWORD got = 0;
		BOOL ok = ReadFile(conn->pipe, p, (DWORD)len, &got, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok = GetOverlappedResult(conn->pipe, &ov, &got, FALSE);
		}
		CloseHandle(ov.hEvent);
		if (!ok || got == 0) {
			return false;
		}
		p += got;
		len -= got;
	}
	return true;
}

bool
mcp_conn_write(struct mcp_conn *conn, const void *buf, size_t len)
{
	if (conn == NULL) {
		return false;
	}
	const char *p = buf;
	while (len > 0) {
		if (InterlockedCompareExchange(&conn->aborted, 0, 0)) {
			return false; // Aborted by another thread; don't re-issue I/O.
		}
		OVERLAPPED ov = {0};
		ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		DWORD wrote = 0;
		BOOL ok = WriteFile(conn->pipe, p, (DWORD)len, &wrote, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok = GetOverlappedResult(conn->pipe, &ov, &wrote, FALSE);
		}
		CloseHandle(ov.hEvent);
		if (!ok || wrote == 0) {
			return false;
		}
		p += wrote;
		len -= wrote;
	}
	return true;
}

void
mcp_conn_abort(struct mcp_conn *conn)
{
	if (conn == NULL || conn->pipe == INVALID_HANDLE_VALUE) {
		return;
	}
	// Poison first so a reader that wakes and loops fails its flag
	// check instead of re-issuing ReadFile on the still-open handle.
	InterlockedExchange(&conn->aborted, 1);
	CancelIoEx(conn->pipe, NULL);
}

void
mcp_conn_close(struct mcp_conn *conn)
{
	if (conn == NULL) {
		return;
	}
	if (conn->pipe != INVALID_HANDLE_VALUE) {
		// Cancel any in-flight ReadFile/WriteFile a sibling thread
		// may have parked on this handle. The per-connection I/O on
		// Windows uses OVERLAPPED, so CloseHandle would eventually
		// complete the wait, but CancelIoEx makes it deterministic
		// and avoids waiting on a remote client that has gone silent
		// without sending FIN.
		InterlockedExchange(&conn->aborted, 1);
		CancelIoEx(conn->pipe, NULL);
		// Every conn owns its handle since the multi-instance rework:
		// server-side conns receive the connected instance from
		// accept(), client-side conns own their CreateFile handle.
		DisconnectNamedPipe(conn->pipe);
		CloseHandle(conn->pipe);
	}
	free(conn);
}

int
mcp_conn_fd(struct mcp_conn *conn)
{
	(void)conn;
	return -1; // Windows clients cannot poll() a pipe HANDLE; adapter uses threads.
}

static struct mcp_conn *
conn_connect_pipe_name(const char *name)
{
	if (!WaitNamedPipeA(name, 5000)) {
		return NULL;
	}
	HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
	                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	DWORD mode = PIPE_READMODE_BYTE;
	(void)SetNamedPipeHandleState(h, &mode, NULL, NULL);
	struct mcp_conn *c = MCP_TYPED_CALLOC(struct mcp_conn);
	c->pipe = h;
	return c;
}

struct mcp_conn *
mcp_conn_connect(long pid)
{
	char name[128];
	build_pipe_name(name, sizeof(name), pid);
	return conn_connect_pipe_name(name);
}

struct mcp_conn *
mcp_conn_connect_named(const char *role)
{
	if (role == NULL || role[0] == '\0') {
		return NULL;
	}
	char name[128];
	build_pipe_name_named(name, sizeof(name), role);
	return conn_connect_pipe_name(name);
}

size_t
mcp_enumerate_sessions(long *out_pids, size_t cap)
{
	size_t n = 0;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("\\\\.\\pipe\\displayxr-mcp-*", &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return 0;
	}
	do {
		const char *prefix = "displayxr-mcp-";
		const char *p = strstr(fd.cFileName, prefix);
		if (p == NULL) {
			continue;
		}
		long pid = strtol(p + strlen(prefix), NULL, 10);
		if (pid > 0 && n < cap) {
			out_pids[n++] = (long)pid;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}

#endif
