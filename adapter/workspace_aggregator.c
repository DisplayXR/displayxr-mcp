// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  `--target workspace`: MCP-terminating aggregator over every
 *         workspace MCP endpoint (shell role + per-PID app sessions).
 *
 * Unlike the 1:1 byte-shuttle modes, this mode speaks MCP on both
 * sides: it initializes each backend itself, merges their tools/list
 * under `<prefix>__<tool>` names, routes tools/call by prefix with
 * JSON-RPC id rewriting, mirrors membership (pipe enumeration, ~1 s
 * poll), and relays notifications/tools/list_changed.
 *
 * Design: displayxr-runtime docs/roadmap/per-app-mcp-tools.md §6.
 *
 * Threads:
 *  - main: stdin frame loop (the agent side).
 *  - poll: membership discovery; spawns one setup thread per candidate
 *    so a hung backend can never stall discovery.
 *  - per-backend reader: routes responses + notifications upstream.
 *
 * Locking: g.lock guards the tables and is never held across backend
 * I/O. A write to a backend takes only that backend's write_mutex,
 * holding a reference (backend.refs) so the slot and its conn outlive
 * the unlocked write even if the reader tears the backend down
 * meanwhile; the last reference frees it. One slow app therefore
 * stalls only the calls routed to it.
 */

#include "stdio_framing.h"
#include "workspace_aggregator.h"

#include "displayxr_mcp/mcp_transport.h"

#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
// MSVC has no strncasecmp/strcasecmp; alias the underscore variants.
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif

// Set by CMakeLists.txt from the release tag (cmake/DisplayXRMCPVersion.cmake).
// No in-source default: a hard-coded one is what went stale (#41).
#ifndef DISPLAYXR_MCP_VERSION
#error "DISPLAYXR_MCP_VERSION must be defined by the build (see CMakeLists.txt)"
#endif

#define MAX_BACKENDS 32
#define MAX_INFLIGHT 128
#define MAX_PREFIXES 128
#define POLL_INTERVAL_MS 1000
#define NS_SEP "__"

// ---------------------------------------------------------------- state

struct backend
{
	bool used;
	bool alive;        //!< conn usable; false once the reader saw EOF.
	bool is_shell;
	long pid;          //!< 0 for the shell role.
	char prefix[40];   //!< Sticky namespace prefix (no separator).
	char app_id[33];   //!< Last _meta-reported app id; "" = none.
	struct mcp_conn *conn;
	pthread_t reader;
	pthread_mutex_t write_mutex;
	cJSON *tools;      //!< Cached tools array (backend-side names).
	int refs;          //!< Writers using conn outside g.lock (see backend_ref).
	bool dead;         //!< Reader gone; the slot is freed when refs drops to 0.
};

enum inflight_kind
{
	INFLIGHT_AGENT,         //!< Forwarded agent request; re-emit with orig id.
	INFLIGHT_TOOLS_REFRESH, //!< Internal tools/list after list_changed.
};

struct inflight
{
	bool used;
	uint64_t local_id;
	int backend;
	enum inflight_kind kind;
	cJSON *orig_id; //!< Owned dup of the agent's id (INFLIGHT_AGENT only).
};

static struct
{
	bool expose_diagnostics;
	bool agent_initialized;

	pthread_mutex_t lock; //!< Guards backends, inflight, prefixes, next_local_id.
	struct backend backends[MAX_BACKENDS];
	struct inflight inflight[MAX_INFLIGHT];
	uint64_t next_local_id;

	//! Prefixes in use: one per live backend, plus the reserved names
	//! ("shell", "workspace"). A backend keeps its prefix for as long as
	//! it is connected; it is released when the backend goes away.
	char *used_prefixes[MAX_PREFIXES];
	size_t used_prefix_count;

	//! Candidate pids a setup thread is currently handshaking with.
	long connecting[MAX_BACKENDS];
	size_t connecting_count;
	bool shell_connecting;

	pthread_mutex_t stdout_mutex;

	//! Agent-side framing, fixed by the first stdin message.
	struct dxr_stdio_framing_latch framing;
} g;

// ---------------------------------------------------------------- framing

// Agent side: MCP stdio transport (newline-delimited JSON), or the
// legacy Content-Length framing when the client opened with it — see
// stdio_framing.h. Replies always use the client's framing.
static bool
write_frame_stdout(const char *body)
{
	enum dxr_stdio_framing f = dxr_stdio_framing_latch_wait(&g.framing);
	size_t out_len = 0;
	char *out = dxr_stdio_encode(f, body, strlen(body), &out_len);
	if (out == NULL) {
		return false;
	}
	pthread_mutex_lock(&g.stdout_mutex);
	bool ok = dxr_stdio_write_stdout(out, out_len);
	pthread_mutex_unlock(&g.stdout_mutex);
	free(out);
	return ok;
}

// Backend side: always Content-Length (the private socket / pipe wire).
static char *
read_frame_conn(struct mcp_conn *conn)
{
	return dxr_conn_read_frame(conn, NULL);
}

static bool
write_frame_conn(struct mcp_conn *conn, const char *body)
{
	return dxr_conn_write_frame(conn, body, strlen(body));
}

// ---------------------------------------------------------------- JSON-RPC helpers

static char *
jsonrpc_error_str(const cJSON *id, int code, const char *message)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "jsonrpc", "2.0");
	if (id != NULL) {
		cJSON_AddItemToObject(env, "id", cJSON_Duplicate(id, 1));
	}
	cJSON *err = cJSON_CreateObject();
	cJSON_AddNumberToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message);
	cJSON_AddItemToObject(env, "error", err);
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

static char *
jsonrpc_result_str(const cJSON *id, cJSON *result /* owned */)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "jsonrpc", "2.0");
	if (id != NULL) {
		cJSON_AddItemToObject(env, "id", cJSON_Duplicate(id, 1));
	}
	cJSON_AddItemToObject(env, "result", result != NULL ? result : cJSON_CreateObject());
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

// Wrap a structured payload the way mcp_server wraps tool results.
static cJSON *
wrap_tool_result(cJSON *inner /* owned */)
{
	cJSON *result = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "text");
	char *inner_str = cJSON_PrintUnformatted(inner);
	cJSON_AddStringToObject(item, "text", inner_str != NULL ? inner_str : "");
	free(inner_str);
	cJSON_AddItemToArray(content, item);
	cJSON_AddItemToObject(result, "content", content);
	cJSON_AddItemToObject(result, "structured", inner);
	return result;
}

static void
emit_list_changed_upstream(void)
{
	pthread_mutex_lock(&g.lock);
	bool send = g.agent_initialized;
	pthread_mutex_unlock(&g.lock);
	if (send) {
		write_frame_stdout(
		    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}");
	}
}

// ---------------------------------------------------------------- prefixes

static bool
prefix_in_use(const char *p)
{
	for (size_t i = 0; i < g.used_prefix_count; i++) {
		if (strcmp(g.used_prefixes[i], p) == 0) {
			return true;
		}
	}
	return false;
}

static bool
reserve_prefix(const char *p)
{
	if (g.used_prefix_count >= MAX_PREFIXES) {
		return false;
	}
	char *dup = strdup(p);
	if (dup == NULL) {
		return false;
	}
	g.used_prefixes[g.used_prefix_count++] = dup;
	return true;
}

// Caller holds g.lock. No-op for a prefix that is not in the table.
static void
release_prefix(const char *p)
{
	for (size_t i = 0; i < g.used_prefix_count; i++) {
		if (strcmp(g.used_prefixes[i], p) == 0) {
			free(g.used_prefixes[i]);
			g.used_prefixes[i] = g.used_prefixes[--g.used_prefix_count];
			return;
		}
	}
}

/*!
 * Slugify into the app-id charset: lowercase, [a-z0-9-], must start
 * alphanumeric, max 32 chars. '_' and other separators map to '-'.
 */
static void
slugify(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	for (const char *p = in; *p != '\0' && n + 1 < cap && n < 32; p++) {
		char c = *p;
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (alnum) {
			out[n++] = c;
		} else if (n > 0 && out[n - 1] != '-') {
			out[n++] = '-';
		}
	}
	while (n > 0 && out[n - 1] == '-') {
		n--;
	}
	out[n] = '\0';
	if (out[0] == '\0') {
		snprintf(out, cap, "app");
	}
}

/*! Best-effort exe basename for a pid; empty string when unavailable. */
static void
exe_basename_for_pid(long pid, char *out, size_t cap)
{
	out[0] = '\0';
	char path[1024] = {0};
#if defined(_WIN32)
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h != NULL) {
		DWORD len = (DWORD)sizeof(path);
		if (QueryFullProcessImageNameA(h, 0, path, &len) == 0) {
			path[0] = '\0';
		}
		CloseHandle(h);
	}
#elif defined(__APPLE__)
	if (proc_pidpath((int)pid, path, sizeof(path)) <= 0) {
		path[0] = '\0';
	}
#else
	char link[64];
	snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
	ssize_t n = readlink(link, path, sizeof(path) - 1);
	if (n > 0) {
		path[n] = '\0';
	}
#endif
	if (path[0] == '\0') {
		return;
	}
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	snprintf(out, cap, "%s", base);
	// Strip a trailing ".exe".
	size_t blen = strlen(out);
	if (blen > 4 && strcasecmp(out + blen - 4, ".exe") == 0) {
		out[blen - 4] = '\0';
	}
}

/*!
 * Assign a prefix from a desired slug: bare name first, then -2, -3, …
 * on collision. Caller holds g.lock and has released the backend's
 * previous prefix, if any (so a re-prefix to the same slug keeps the
 * bare name instead of colliding with itself).
 *
 * Every live prefix is in the table and the table holds at most
 * MAX_BACKENDS + the reserved names (< MAX_PREFIXES), so one of the
 * first MAX_PREFIXES suffixes is always free: no ambiguous fallback.
 */
static void
assign_prefix(const char *desired, char *out, size_t cap)
{
	char candidate[48];
	snprintf(candidate, sizeof(candidate), "%s", desired);
	for (int n = 2; prefix_in_use(candidate) && n < MAX_PREFIXES + 2; n++) {
		snprintf(candidate, sizeof(candidate), "%s-%d", desired, n);
	}
	snprintf(out, cap, "%s", candidate);
	if (!reserve_prefix(out)) {
		fprintf(stderr, "displayxr-mcp: prefix table full; '%s' may be ambiguous\n", out);
	}
}

// ---------------------------------------------------------------- in-flight map

// Caller holds g.lock.
static struct inflight *
inflight_alloc(int backend, enum inflight_kind kind, const cJSON *orig_id)
{
	for (size_t i = 0; i < MAX_INFLIGHT; i++) {
		if (!g.inflight[i].used) {
			g.inflight[i].used = true;
			g.inflight[i].local_id = ++g.next_local_id;
			g.inflight[i].backend = backend;
			g.inflight[i].kind = kind;
			g.inflight[i].orig_id = orig_id != NULL ? cJSON_Duplicate(orig_id, 1) : NULL;
			return &g.inflight[i];
		}
	}
	return NULL;
}

// Caller holds g.lock.
static struct inflight *
inflight_find(uint64_t local_id)
{
	for (size_t i = 0; i < MAX_INFLIGHT; i++) {
		if (g.inflight[i].used && g.inflight[i].local_id == local_id) {
			return &g.inflight[i];
		}
	}
	return NULL;
}

// Caller holds g.lock.
static void
inflight_release(struct inflight *fl)
{
	if (fl->orig_id != NULL) {
		cJSON_Delete(fl->orig_id);
	}
	memset(fl, 0, sizeof(*fl));
}

// ---------------------------------------------------------------- exposure

/*!
 * Group of a backend tool entry: explicit _meta displayxr/group wins;
 * untagged tools default to "diagnostic" except capture_frame, which
 * is special-cased to "capture" so pre-v0.4.0 runtimes keep their
 * verification primitive visible (design doc §5).
 */
static const char *
tool_group(const cJSON *tool_entry)
{
	const cJSON *meta = cJSON_GetObjectItemCaseSensitive(tool_entry, "_meta");
	const cJSON *group = meta != NULL ? cJSON_GetObjectItemCaseSensitive(meta, "displayxr/group") : NULL;
	if (cJSON_IsString(group)) {
		return group->valuestring;
	}
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(tool_entry, "name");
	if (cJSON_IsString(name) && strcmp(name->valuestring, "capture_frame") == 0) {
		return "capture";
	}
	return "diagnostic";
}

static bool
tool_exposed(const cJSON *tool_entry)
{
	return g.expose_diagnostics || strcmp(tool_group(tool_entry), "diagnostic") != 0;
}

// ---------------------------------------------------------------- backend teardown

// Caller holds g.lock; refs == 0. Closes the conn and frees the slot.
static void
backend_free_locked(int idx)
{
	struct backend *b = &g.backends[idx];
	if (b->tools != NULL) {
		cJSON_Delete(b->tools);
	}
	struct mcp_conn *conn = b->conn;
	pthread_mutex_destroy(&b->write_mutex);
	memset(b, 0, sizeof(*b));
	// Nobody else can be using the conn: the reader has exited (or never
	// started) and no writer holds a reference.
	if (conn != NULL) {
		mcp_conn_close(conn);
	}
}

/*!
 * Caller holds g.lock. Take a reference that keeps the slot, its conn
 * and its write_mutex valid while the caller writes WITHOUT g.lock.
 * Release with backend_unref().
 */
static void
backend_ref_locked(int idx)
{
	g.backends[idx].refs++;
}

static void
backend_unref(int idx)
{
	pthread_mutex_lock(&g.lock);
	struct backend *b = &g.backends[idx];
	if (--b->refs == 0 && b->dead) {
		backend_free_locked(idx);
	}
	pthread_mutex_unlock(&g.lock);
}

/*!
 * Caller holds g.lock. Fails outstanding agent calls and retires the
 * slot: it is freed now if no writer holds a reference, else by the
 * last backend_unref(). Until then it stays `used` (not reused, and the
 * pid stays known to the poller) but not `alive` (no new calls).
 */
static void
backend_teardown_locked(int idx)
{
	struct backend *b = &g.backends[idx];
	// Release the prefix now: a restarted app gets its bare name back.
	if (!b->is_shell && b->prefix[0] != '\0') {
		release_prefix(b->prefix);
	}
	for (size_t i = 0; i < MAX_INFLIGHT; i++) {
		struct inflight *fl = &g.inflight[i];
		if (!fl->used || fl->backend != idx) {
			continue;
		}
		if (fl->kind == INFLIGHT_AGENT) {
			char *err = jsonrpc_error_str(fl->orig_id, -32000,
			                              "backend exited with the call in flight");
			write_frame_stdout(err);
			free(err);
		}
		inflight_release(fl);
	}
	b->alive = false;
	b->dead = true;
	if (b->refs == 0) {
		backend_free_locked(idx);
	}
}

/*!
 * Write @p req to backend @p idx outside g.lock. Caller holds g.lock and
 * has allocated the in-flight entry @p local_id; g.lock is RELEASED on
 * return. On failure the entry is released, unless teardown already
 * answered it. Returns whether the write went through.
 */
static bool
backend_write_unlocked(int idx, const char *req, uint64_t local_id)
{
	struct backend *b = &g.backends[idx];
	struct mcp_conn *conn = b->conn;
	backend_ref_locked(idx);
	pthread_mutex_unlock(&g.lock);

	pthread_mutex_lock(&b->write_mutex);
	bool ok = req != NULL && write_frame_conn(conn, req);
	pthread_mutex_unlock(&b->write_mutex);

	if (!ok) {
		pthread_mutex_lock(&g.lock);
		struct inflight *fl = inflight_find(local_id);
		if (fl != NULL && fl->backend == idx) {
			inflight_release(fl);
		} else {
			ok = true; // Teardown already failed this call to the agent.
		}
		pthread_mutex_unlock(&g.lock);
	}
	backend_unref(idx);
	return ok;
}

// ---------------------------------------------------------------- backend reader

static void
refresh_backend_tools(int idx)
{
	pthread_mutex_lock(&g.lock);
	struct backend *b = &g.backends[idx];
	if (!b->used || !b->alive) {
		pthread_mutex_unlock(&g.lock);
		return;
	}
	struct inflight *fl = inflight_alloc(idx, INFLIGHT_TOOLS_REFRESH, NULL);
	if (fl == NULL) {
		pthread_mutex_unlock(&g.lock);
		return;
	}
	char req[128];
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"tools/list\"}",
	         (unsigned long long)fl->local_id);
	(void)backend_write_unlocked(idx, req, fl->local_id); // Drops g.lock.
}

/*!
 * Apply a fresh tools/list result to the backend cache; re-prefix if
 * the app id changed (late xrSetMCPAppInfoDXR). Returns true if the
 * agent-visible tool surface changed.
 */
static bool
apply_tools_result(int idx, const cJSON *result)
{
	const cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
	const cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
	const cJSON *app_id = meta != NULL ? cJSON_GetObjectItemCaseSensitive(meta, "displayxr/appId") : NULL;

	pthread_mutex_lock(&g.lock);
	struct backend *b = &g.backends[idx];
	if (!b->used) {
		pthread_mutex_unlock(&g.lock);
		return false;
	}
	if (b->tools != NULL) {
		cJSON_Delete(b->tools);
	}
	b->tools = cJSON_IsArray(tools) ? cJSON_Duplicate(tools, 1) : cJSON_CreateArray();

	// Late or changed app id → re-prefix (sticky rules still apply).
	if (!b->is_shell && cJSON_IsString(app_id) &&
	    strcmp(b->app_id, app_id->valuestring) != 0) {
		snprintf(b->app_id, sizeof(b->app_id), "%s", app_id->valuestring);
		char slug[40];
		slugify(b->app_id, slug, sizeof(slug));
		// Release our own prefix first: an app named "foo" after its exe
		// whose app id then arrives as "foo" must keep "foo", not become
		// "foo-2" by colliding with itself.
		release_prefix(b->prefix);
		assign_prefix(slug, b->prefix, sizeof(b->prefix));
	}
	pthread_mutex_unlock(&g.lock);
	return true;
}

static void *
backend_reader(void *arg)
{
	int idx = (int)(intptr_t)arg;

	for (;;) {
		pthread_mutex_lock(&g.lock);
		struct mcp_conn *conn = g.backends[idx].conn;
		pthread_mutex_unlock(&g.lock);
		if (conn == NULL) {
			break;
		}

		char *body = read_frame_conn(conn);
		if (body == NULL) {
			break; // EOF / backend died.
		}
		cJSON *msg = cJSON_Parse(body);
		free(body);
		if (msg == NULL) {
			continue;
		}

		const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
		const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");

		if (id == NULL && cJSON_IsString(method)) {
			// Backend notification. Only list_changed is meaningful to
			// relay; it also covers app-id changes.
			if (strcmp(method->valuestring, "notifications/tools/list_changed") == 0) {
				refresh_backend_tools(idx);
			}
			cJSON_Delete(msg);
			continue;
		}

		if (cJSON_IsNumber(id)) {
			// Our local ids are positive integers well below 2^53; anything
			// else cannot match an in-flight entry. Range-check before the
			// cast (a negative / NaN / huge double -> uint64_t is UB).
			double idv = id->valuedouble;
			uint64_t local_id = 0;
			if (idv >= 1.0 && idv <= 9007199254740992.0) {
				local_id = (uint64_t)idv;
				if ((double)local_id != idv) {
					local_id = 0;
				}
			}
			pthread_mutex_lock(&g.lock);
			struct inflight *fl = inflight_find(local_id);
			enum inflight_kind kind = fl != NULL ? fl->kind : INFLIGHT_AGENT;
			cJSON *orig_id = NULL;
			int fl_backend = fl != NULL ? fl->backend : -1;
			if (fl != NULL) {
				orig_id = fl->orig_id;
				fl->orig_id = NULL; // Transfer ownership.
				inflight_release(fl);
			}
			pthread_mutex_unlock(&g.lock);

			if (fl_backend != idx) {
				// Unknown or cross-wired id; drop.
				if (orig_id != NULL) {
					cJSON_Delete(orig_id);
				}
				cJSON_Delete(msg);
				continue;
			}

			if (kind == INFLIGHT_TOOLS_REFRESH) {
				const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
				if (result != NULL && apply_tools_result(idx, result)) {
					emit_list_changed_upstream();
				}
				cJSON_Delete(msg);
				continue;
			}

			// Agent response: restore the agent's original id and relay.
			if (orig_id != NULL) {
				cJSON_ReplaceItemInObjectCaseSensitive(msg, "id", orig_id);
			}
			char *out = cJSON_PrintUnformatted(msg);
			cJSON_Delete(msg);
			if (out != NULL) {
				write_frame_stdout(out);
				free(out);
			}
			continue;
		}

		cJSON_Delete(msg);
	}

	// Backend died: fail in-flight calls, free the slot, tell the agent.
	pthread_mutex_lock(&g.lock);
	backend_teardown_locked(idx);
	pthread_mutex_unlock(&g.lock);
	emit_list_changed_upstream();
	return NULL;
}

// ---------------------------------------------------------------- backend setup

struct setup_args
{
	bool is_shell;
	long pid;
};

/*!
 * Synchronous request/response on a fresh, not-yet-shared conn (no
 * reader thread to race with yet). Skips interleaved notifications.
 * Returns the parsed response (caller frees) or NULL.
 */
static cJSON *
sync_request(struct mcp_conn *conn, const char *req, double want_id)
{
	if (!write_frame_conn(conn, req)) {
		return NULL;
	}
	for (int guard = 0; guard < 16; guard++) {
		char *body = read_frame_conn(conn);
		if (body == NULL) {
			return NULL;
		}
		cJSON *msg = cJSON_Parse(body);
		free(body);
		if (msg == NULL) {
			return NULL;
		}
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
		if (cJSON_IsNumber(id) && id->valuedouble == want_id) {
			return msg;
		}
		cJSON_Delete(msg); // Notification or stale frame; keep reading.
	}
	return NULL;
}

static void
connecting_remove(bool is_shell, long pid)
{
	pthread_mutex_lock(&g.lock);
	if (is_shell) {
		g.shell_connecting = false;
	} else {
		for (size_t i = 0; i < g.connecting_count; i++) {
			if (g.connecting[i] == pid) {
				g.connecting[i] = g.connecting[--g.connecting_count];
				break;
			}
		}
	}
	pthread_mutex_unlock(&g.lock);
}

static void *
backend_setup(void *arg)
{
	struct setup_args *sa = arg;
	bool is_shell = sa->is_shell;
	long pid = sa->pid;
	free(sa);

	struct mcp_conn *conn =
	    is_shell ? mcp_conn_connect_named("shell") : mcp_conn_connect(pid);
	if (conn == NULL) {
		connecting_remove(is_shell, pid);
		return NULL;
	}

	// Handshake: initialize → initialized → tools/list. Local ids 1/2
	// are private to this conn (the shared id space starts only when
	// the reader thread takes over).
	cJSON *init = sync_request(
	    conn,
	    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
	    "{\"protocolVersion\":\"2024-11-05\",\"clientInfo\":{\"name\":\"displayxr-mcp-workspace\"}}}",
	    1);
	if (init == NULL) {
		mcp_conn_close(conn);
		connecting_remove(is_shell, pid);
		return NULL;
	}
	char app_id[33] = {0};
	const cJSON *result = cJSON_GetObjectItemCaseSensitive(init, "result");
	const cJSON *sinfo = result != NULL ? cJSON_GetObjectItemCaseSensitive(result, "serverInfo") : NULL;
	const cJSON *aid = sinfo != NULL ? cJSON_GetObjectItemCaseSensitive(sinfo, "appId") : NULL;
	if (cJSON_IsString(aid)) {
		snprintf(app_id, sizeof(app_id), "%s", aid->valuestring);
	}
	cJSON_Delete(init);

	(void)write_frame_conn(conn, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

	cJSON *tools_resp =
	    sync_request(conn, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", 2);
	if (tools_resp == NULL) {
		mcp_conn_close(conn);
		connecting_remove(is_shell, pid);
		return NULL;
	}
	const cJSON *tresult = cJSON_GetObjectItemCaseSensitive(tools_resp, "result");
	const cJSON *tools = tresult != NULL ? cJSON_GetObjectItemCaseSensitive(tresult, "tools") : NULL;
	const cJSON *tmeta = tresult != NULL ? cJSON_GetObjectItemCaseSensitive(tresult, "_meta") : NULL;
	const cJSON *taid = tmeta != NULL ? cJSON_GetObjectItemCaseSensitive(tmeta, "displayxr/appId") : NULL;
	if (app_id[0] == '\0' && cJSON_IsString(taid)) {
		snprintf(app_id, sizeof(app_id), "%s", taid->valuestring);
	}

	// Register the backend.
	pthread_mutex_lock(&g.lock);
	struct backend *b = NULL;
	int idx = -1;
	for (int i = 0; i < MAX_BACKENDS; i++) {
		if (!g.backends[i].used) {
			b = &g.backends[i];
			idx = i;
			break;
		}
	}
	if (b == NULL) {
		pthread_mutex_unlock(&g.lock);
		fprintf(stderr, "displayxr-mcp: backend limit (%d) reached; ignoring %s%ld\n",
		        MAX_BACKENDS, is_shell ? "shell" : "pid ", pid);
		cJSON_Delete(tools_resp);
		mcp_conn_close(conn);
		connecting_remove(is_shell, pid);
		return NULL;
	}
	memset(b, 0, sizeof(*b));
	b->used = true;
	b->alive = true;
	b->is_shell = is_shell;
	b->pid = pid;
	b->conn = conn;
	pthread_mutex_init(&b->write_mutex, NULL);
	b->tools = cJSON_IsArray(tools) ? cJSON_Duplicate(tools, 1) : cJSON_CreateArray();
	snprintf(b->app_id, sizeof(b->app_id), "%s", app_id);

	if (is_shell) {
		// Fixed prefix; the shell is a singleton role.
		snprintf(b->prefix, sizeof(b->prefix), "shell");
	} else {
		char desired[40];
		if (app_id[0] != '\0') {
			slugify(app_id, desired, sizeof(desired));
		} else {
			char base[64];
			exe_basename_for_pid(pid, base, sizeof(base));
			if (base[0] != '\0') {
				slugify(base, desired, sizeof(desired));
			} else {
				snprintf(desired, sizeof(desired), "app");
			}
		}
		assign_prefix(desired, b->prefix, sizeof(b->prefix));
	}

	int rc = pthread_create(&b->reader, NULL, backend_reader, (void *)(intptr_t)idx);
	if (rc != 0) {
		backend_teardown_locked(idx);
		pthread_mutex_unlock(&g.lock);
		cJSON_Delete(tools_resp);
		connecting_remove(is_shell, pid);
		return NULL;
	}
	pthread_detach(b->reader);
	pthread_mutex_unlock(&g.lock);

	cJSON_Delete(tools_resp);
	connecting_remove(is_shell, pid);
	emit_list_changed_upstream();
	return NULL;
}

// ---------------------------------------------------------------- membership poll

static bool
backend_known(bool is_shell, long pid)
{
	for (int i = 0; i < MAX_BACKENDS; i++) {
		const struct backend *b = &g.backends[i];
		if (!b->used) {
			continue;
		}
		if (is_shell ? b->is_shell : (!b->is_shell && b->pid == pid)) {
			return true;
		}
	}
	if (is_shell) {
		return g.shell_connecting;
	}
	for (size_t i = 0; i < g.connecting_count; i++) {
		if (g.connecting[i] == pid) {
			return true;
		}
	}
	return false;
}

static void
spawn_setup(bool is_shell, long pid)
{
	struct setup_args *sa = malloc(sizeof(*sa));
	if (sa == NULL) {
		return;
	}
	sa->is_shell = is_shell;
	sa->pid = pid;
	pthread_t t;
	if (pthread_create(&t, NULL, backend_setup, sa) == 0) {
		pthread_detach(t);
	} else {
		free(sa);
		connecting_remove(is_shell, pid);
	}
}

static void *
poll_thread(void *arg)
{
	(void)arg;
	for (;;) {
		// Per-PID app sessions. NOTE: the allowlist is pid pipes + the
		// `shell` role only — never `service` (a ghost endpoint from
		// pre-extraction runtime installs must not be aggregated).
		long pids[64];
		size_t n = mcp_enumerate_sessions(pids, 64);

		pthread_mutex_lock(&g.lock);
		long fresh[64];
		size_t fresh_count = 0;
		for (size_t i = 0; i < n; i++) {
			if (pids[i] == mcp_self_pid()) {
				continue;
			}
			if (!backend_known(false, pids[i]) &&
			    g.connecting_count < MAX_BACKENDS) {
				g.connecting[g.connecting_count++] = pids[i];
				fresh[fresh_count++] = pids[i];
			}
		}
		bool try_shell = !backend_known(true, 0);
		if (try_shell) {
			g.shell_connecting = true;
		}
		pthread_mutex_unlock(&g.lock);

		for (size_t i = 0; i < fresh_count; i++) {
			spawn_setup(false, fresh[i]);
		}
		if (try_shell) {
			spawn_setup(true, 0);
		}

#ifdef _WIN32
		Sleep(POLL_INTERVAL_MS);
#else
		usleep(POLL_INTERVAL_MS * 1000);
#endif
	}
	return NULL;
}

// ---------------------------------------------------------------- agent-side handlers

static cJSON *
build_list_apps(void)
{
	cJSON *apps = cJSON_CreateArray();
	bool shell_running = false;
	pthread_mutex_lock(&g.lock);
	for (int i = 0; i < MAX_BACKENDS; i++) {
		const struct backend *b = &g.backends[i];
		if (!b->used || !b->alive) {
			continue;
		}
		if (b->is_shell) {
			shell_running = true;
			continue;
		}
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "prefix", b->prefix);
		cJSON_AddNumberToObject(e, "pid", (double)b->pid);
		if (b->app_id[0] != '\0') {
			cJSON_AddStringToObject(e, "app_id", b->app_id);
		}
		// Count exposed vs total so an agent can tell when diagnostics
		// are hidden.
		int exposed = 0, total = 0;
		cJSON *t = NULL;
		cJSON_ArrayForEach(t, b->tools) {
			total++;
			if (tool_exposed(t)) {
				exposed++;
			}
		}
		cJSON_AddNumberToObject(e, "tools_exposed", exposed);
		cJSON_AddNumberToObject(e, "tools_total", total);
		cJSON_AddItemToArray(apps, e);
	}
	pthread_mutex_unlock(&g.lock);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddItemToObject(r, "apps", apps);
	cJSON_AddStringToObject(r, "shell", shell_running ? "running" : "not_running");
	cJSON_AddBoolToObject(r, "diagnostics_exposed", g.expose_diagnostics);
	return r;
}

static cJSON *
build_merged_tools(void)
{
	cJSON *arr = cJSON_CreateArray();

	// Aggregator built-ins first.
	cJSON *meta_tool = cJSON_CreateObject();
	cJSON_AddStringToObject(meta_tool, "name", "workspace" NS_SEP "list_apps");
	cJSON_AddStringToObject(
	    meta_tool, "description",
	    "List aggregated workspace members: tool-name prefix, pid, app id, and tool counts "
	    "per app, plus whether the shell is running. The prefix joins app tools "
	    "(<prefix>__<tool>) to shell window operations.");
	cJSON *schema = cJSON_CreateObject();
	cJSON_AddStringToObject(schema, "type", "object");
	cJSON_AddItemToObject(meta_tool, "inputSchema", schema);
	cJSON_AddItemToArray(arr, meta_tool);

	pthread_mutex_lock(&g.lock);
	for (int i = 0; i < MAX_BACKENDS; i++) {
		const struct backend *b = &g.backends[i];
		if (!b->used || !b->alive || b->tools == NULL) {
			continue;
		}
		cJSON *t = NULL;
		cJSON_ArrayForEach(t, b->tools) {
			if (!tool_exposed(t)) {
				continue;
			}
			const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
			if (!cJSON_IsString(name)) {
				continue;
			}
			cJSON *e = cJSON_Duplicate(t, 1);
			char prefixed[128];
			snprintf(prefixed, sizeof(prefixed), "%s" NS_SEP "%s", b->prefix,
			         name->valuestring);
			cJSON_ReplaceItemInObjectCaseSensitive(e, "name", cJSON_CreateString(prefixed));
			cJSON *emeta = cJSON_GetObjectItemCaseSensitive(e, "_meta");
			if (emeta == NULL) {
				emeta = cJSON_CreateObject();
				cJSON_AddItemToObject(e, "_meta", emeta);
			}
			cJSON_AddStringToObject(emeta, "displayxr/source", b->prefix);
			cJSON_AddItemToArray(arr, e);
		}
	}
	pthread_mutex_unlock(&g.lock);
	return arr;
}

static void
handle_tools_call(const cJSON *id, const cJSON *params)
{
	const cJSON *name_node =
	    params != NULL ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
	if (!cJSON_IsString(name_node)) {
		char *err = jsonrpc_error_str(id, -32602, "tools/call: missing string 'name'");
		write_frame_stdout(err);
		free(err);
		return;
	}
	const char *full = name_node->valuestring;

	if (strcmp(full, "workspace" NS_SEP "list_apps") == 0) {
		char *out = jsonrpc_result_str(id, wrap_tool_result(build_list_apps()));
		write_frame_stdout(out);
		free(out);
		return;
	}

	const char *sep = strstr(full, NS_SEP);
	if (sep == NULL || sep == full || sep[2] == '\0') {
		char *err = jsonrpc_error_str(id, -32601,
		                              "tool not found (expected <prefix>__<tool>)");
		write_frame_stdout(err);
		free(err);
		return;
	}
	size_t plen = (size_t)(sep - full);
	const char *bare = sep + 2;

	pthread_mutex_lock(&g.lock);
	struct backend *b = NULL;
	int idx = -1;
	for (int i = 0; i < MAX_BACKENDS; i++) {
		struct backend *cand = &g.backends[i];
		if (cand->used && cand->alive && strlen(cand->prefix) == plen &&
		    strncmp(cand->prefix, full, plen) == 0) {
			b = cand;
			idx = i;
			break;
		}
	}
	if (b == NULL) {
		pthread_mutex_unlock(&g.lock);
		char *err = jsonrpc_error_str(id, -32601, "tool not found (no such app prefix)");
		write_frame_stdout(err);
		free(err);
		return;
	}

	struct inflight *fl = inflight_alloc(idx, INFLIGHT_AGENT, id);
	if (fl == NULL) {
		pthread_mutex_unlock(&g.lock);
		char *err = jsonrpc_error_str(id, -32000, "too many calls in flight");
		write_frame_stdout(err);
		free(err);
		return;
	}
	uint64_t local_id = fl->local_id;

	// Rebuild the request with the bare tool name + our local id.
	cJSON *fwd = cJSON_CreateObject();
	cJSON_AddStringToObject(fwd, "jsonrpc", "2.0");
	cJSON_AddNumberToObject(fwd, "id", (double)local_id);
	cJSON_AddStringToObject(fwd, "method", "tools/call");
	cJSON *fparams = cJSON_CreateObject();
	cJSON_AddStringToObject(fparams, "name", bare);
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
	if (args != NULL) {
		cJSON_AddItemToObject(fparams, "arguments", cJSON_Duplicate(args, 1));
	}
	cJSON_AddItemToObject(fwd, "params", fparams);
	char *fwd_str = cJSON_PrintUnformatted(fwd);
	cJSON_Delete(fwd);

	// Only this backend's write_mutex is held for the I/O: a backend
	// that stops reading stalls calls to it, not every app's traffic.
	bool ok = backend_write_unlocked(idx, fwd_str, local_id); // Drops g.lock.
	free(fwd_str);

	if (!ok) {
		char *err = jsonrpc_error_str(id, -32000, "backend write failed");
		write_frame_stdout(err);
		free(err);
	}
	// Otherwise the response is relayed asynchronously by the reader.
}

static void
handle_agent_frame(const char *body, size_t len)
{
	cJSON *req = cJSON_ParseWithLength(body, len);
	if (req == NULL) {
		char *err = jsonrpc_error_str(NULL, -32700, "parse error");
		write_frame_stdout(err);
		free(err);
		return;
	}
	const cJSON *method_node = cJSON_GetObjectItemCaseSensitive(req, "method");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
	const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
	const char *method = cJSON_IsString(method_node) ? method_node->valuestring : NULL;

	if (method == NULL || id == NULL) {
		// Notification (e.g. notifications/initialized) or junk; ignore.
		cJSON_Delete(req);
		return;
	}

	if (strcmp(method, "initialize") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
		cJSON *caps = cJSON_CreateObject();
		cJSON *tools_cap = cJSON_CreateObject();
		cJSON_AddBoolToObject(tools_cap, "listChanged", true);
		cJSON_AddItemToObject(caps, "tools", tools_cap);
		cJSON_AddItemToObject(result, "capabilities", caps);
		cJSON *info = cJSON_CreateObject();
		cJSON_AddStringToObject(info, "name", "displayxr-mcp-workspace");
		cJSON_AddStringToObject(info, "version", DISPLAYXR_MCP_VERSION);
		cJSON_AddItemToObject(result, "serverInfo", info);
		char *out = jsonrpc_result_str(id, result);
		write_frame_stdout(out);
		free(out);
		pthread_mutex_lock(&g.lock);
		g.agent_initialized = true;
		pthread_mutex_unlock(&g.lock);
	} else if (strcmp(method, "ping") == 0) {
		char *out = jsonrpc_result_str(id, NULL);
		write_frame_stdout(out);
		free(out);
	} else if (strcmp(method, "tools/list") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "tools", build_merged_tools());
		char *out = jsonrpc_result_str(id, result);
		write_frame_stdout(out);
		free(out);
	} else if (strcmp(method, "tools/call") == 0) {
		handle_tools_call(id, params);
	} else {
		char *err = jsonrpc_error_str(id, -32601, "method not found");
		write_frame_stdout(err);
		free(err);
	}
	cJSON_Delete(req);
}

// ---------------------------------------------------------------- entry

int
workspace_aggregator_run(bool expose_diagnostics)
{
	pthread_mutex_init(&g.lock, NULL);
	pthread_mutex_init(&g.stdout_mutex, NULL);
	dxr_stdio_framing_latch_init(&g.framing);
	g.expose_diagnostics = expose_diagnostics;
	// Never assigned to an app: "shell" is the shell's fixed prefix and
	// "workspace" carries the aggregator's built-ins (workspace__list_apps).
	reserve_prefix("shell");
	reserve_prefix("workspace");

	pthread_t poller;
	if (pthread_create(&poller, NULL, poll_thread, NULL) != 0) {
		fprintf(stderr, "displayxr-mcp: failed to start membership poll thread\n");
		return 1;
	}
	pthread_detach(poller);

	// Agent frame loop until stdin EOF.
	struct dxr_stdio_reader reader;
	dxr_stdio_reader_init(&reader, dxr_stdio_read_stdin, NULL);
	for (;;) {
		size_t len = 0;
		char *body = dxr_stdio_reader_next(&reader, &len);
		if (body == NULL) {
			break;
		}
		dxr_stdio_framing_latch_set(&g.framing, reader.framing);
		handle_agent_frame(body, len);
		free(body);
	}
	dxr_stdio_reader_fini(&reader);

	// Stdin closed: abort backends so their readers unwind, then exit.
	// Process teardown reclaims the rest; no graceful drain needed for
	// a CLI adapter.
	pthread_mutex_lock(&g.lock);
	for (int i = 0; i < MAX_BACKENDS; i++) {
		if (g.backends[i].used && g.backends[i].conn != NULL) {
			mcp_conn_abort(g.backends[i].conn);
		}
	}
	pthread_mutex_unlock(&g.lock);
	return 0;
}
