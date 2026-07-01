// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  MCP server thread, JSON-RPC dispatch, tool registry.
 * @ingroup aux_util
 */

#include "displayxr_mcp/mcp_server.h"
#include "displayxr_mcp/mcp_transport.h"
#include "displayxr_mcp/mcp_log_ring.h"
#include "displayxr_mcp/mcp_allowlist.h"

#include "displayxr_mcp/os_compat.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC has no strncasecmp; the POSIX-named wrapper for _strnicmp is
// the simplest cross-platform alias.
#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

#define LOG_PFX "[mcp] "
#define MAX_TOOLS 64
#define MAX_CONNS 8
#define MAX_FRAME_BYTES (4 * 1024 * 1024)

#ifndef DISPLAYXR_MCP_VERSION
#define DISPLAYXR_MCP_VERSION "0.4.0-dev"
#endif

/*!
 * One connected client. Slots are owned by the conns_mutex; the serve
 * thread clears its own slot on exit. write_mutex serializes response
 * writes (serve thread) against notification broadcasts (any thread).
 */
struct mcp_client
{
	struct mcp_conn *conn;       //!< NULL = free slot.
	pthread_t thread;
	bool initialized;            //!< Saw `initialize` — eligible for notifications.
	pthread_mutex_t write_mutex;
};

struct mcp_server
{
	pthread_t thread;
	bool thread_started;
	struct mcp_listener *listener;

	pthread_mutex_t tools_mutex;
	const struct mcp_tool *tools[MAX_TOOLS];
	size_t tool_count;
	char app_id[33]; //!< Manifest `id` slug; empty = unset.

	pthread_mutex_t conns_mutex;
	struct mcp_client clients[MAX_CONNS];
	size_t active_clients;
	pthread_cond_t conns_drained; //!< Signaled when active_clients hits 0.
};

static struct mcp_server g_server = {
    .tools_mutex = PTHREAD_MUTEX_INITIALIZER,
    .conns_mutex = PTHREAD_MUTEX_INITIALIZER,
    .conns_drained = PTHREAD_COND_INITIALIZER,
};

// ---------- Frame I/O (Content-Length framing, LSP/MCP style) ----------

static bool
read_line(struct mcp_conn *conn, char *buf, size_t cap)
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

static char *
read_frame(struct mcp_conn *conn, size_t *out_len)
{
	char line[256];
	size_t content_length = 0;
	bool have_length = false;

	while (read_line(conn, line, sizeof(line))) {
		// Header end: blank line (CRLF or LF).
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		size_t key_len = strlen("Content-Length:");
		if (strncasecmp(line, "Content-Length:", key_len) == 0) {
			content_length = (size_t)strtoul(line + key_len, NULL, 10);
			have_length = true;
		}
		// Ignore other headers (e.g. Content-Type).
	}

	if (!have_length || content_length == 0 || content_length > MAX_FRAME_BYTES) {
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
	*out_len = content_length;
	return body;
}

static bool
write_frame(struct mcp_conn *conn, const char *body)
{
	if (body == NULL) {
		return false;
	}
	size_t len = strlen(body);
	char header[64];
	int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
	if (hlen < 0) {
		return false;
	}
	if (!mcp_conn_write(conn, header, (size_t)hlen)) {
		return false;
	}
	return mcp_conn_write(conn, body, len);
}

// ---------- JSON-RPC helpers ----------

static cJSON *
jsonrpc_envelope(const cJSON *id)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "jsonrpc", "2.0");
	if (id != NULL) {
		cJSON_AddItemToObject(env, "id", cJSON_Duplicate(id, 1));
	}
	return env;
}

static char *
jsonrpc_result(const cJSON *id, cJSON *result)
{
	cJSON *env = jsonrpc_envelope(id);
	cJSON_AddItemToObject(env, "result", result != NULL ? result : cJSON_CreateObject());
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

static char *
jsonrpc_error(const cJSON *id, int code, const char *message)
{
	cJSON *env = jsonrpc_envelope(id);
	cJSON *err = cJSON_CreateObject();
	cJSON_AddNumberToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message ? message : "unknown error");
	cJSON_AddItemToObject(env, "error", err);
	char *out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	return out;
}

// ---------- Notifications ----------

/*!
 * Send a JSON-RPC notification to every initialized client. Tolerates
 * write failures (the client's serve thread notices the dead conn on
 * its next read and unwinds the slot).
 */
static void
broadcast_notification(const char *method)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "jsonrpc", "2.0");
	cJSON_AddStringToObject(env, "method", method);
	char *body = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
	if (body == NULL) {
		return;
	}

	pthread_mutex_lock(&g_server.conns_mutex);
	for (size_t i = 0; i < MAX_CONNS; i++) {
		struct mcp_client *cl = &g_server.clients[i];
		if (cl->conn == NULL || !cl->initialized) {
			continue;
		}
		pthread_mutex_lock(&cl->write_mutex);
		(void)write_frame(cl->conn, body);
		pthread_mutex_unlock(&cl->write_mutex);
	}
	pthread_mutex_unlock(&g_server.conns_mutex);
	free(body);
}

static void
notify_tools_changed(void)
{
	if (!g_server.thread_started) {
		return; // Pre-start registration: nothing to notify yet.
	}
	broadcast_notification("notifications/tools/list_changed");
}

// ---------- Tool registry ----------

static const struct mcp_tool *
find_tool(const char *name)
{
	pthread_mutex_lock(&g_server.tools_mutex);
	const struct mcp_tool *found = NULL;
	for (size_t i = 0; i < g_server.tool_count; i++) {
		if (strcmp(g_server.tools[i]->name, name) == 0) {
			found = g_server.tools[i];
			break;
		}
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	return found;
}

void
mcp_server_register_tool(const struct mcp_tool *tool)
{
	if (tool == NULL || tool->name == NULL || tool->fn == NULL) {
		return;
	}
	bool added = false;
	pthread_mutex_lock(&g_server.tools_mutex);
	if (g_server.tool_count < MAX_TOOLS) {
		g_server.tools[g_server.tool_count++] = tool;
		added = true;
	} else {
		MCP_LOG_W(LOG_PFX "tool registry full; dropping '%s'", tool->name);
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	if (added) {
		notify_tools_changed();
	}
}

void
mcp_server_unregister_tool(const char *name)
{
	if (name == NULL) {
		return;
	}
	bool removed = false;
	pthread_mutex_lock(&g_server.tools_mutex);
	for (size_t i = 0; i < g_server.tool_count; i++) {
		if (strcmp(g_server.tools[i]->name, name) != 0) {
			continue;
		}
		// Order is presentation-only; shift to keep tools/list stable-ish.
		memmove(&g_server.tools[i], &g_server.tools[i + 1],
		        (g_server.tool_count - i - 1) * sizeof(g_server.tools[0]));
		g_server.tool_count--;
		removed = true;
		break;
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	if (removed) {
		notify_tools_changed();
	}
}

/*!
 * Validate an app id slug: ^[a-z0-9][a-z0-9-]{0,31}$ — mirrors the
 * manifest spec. Underscores are excluded by design ('__' is the
 * aggregator's namespace separator).
 */
static bool
app_id_valid(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return false;
	}
	size_t len = strlen(id);
	if (len > 32) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = id[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (i == 0 ? !alnum : !(alnum || c == '-')) {
			return false;
		}
	}
	return true;
}

void
mcp_server_set_app_id(const char *app_id)
{
	if (!app_id_valid(app_id)) {
		MCP_LOG_W(LOG_PFX "ignoring invalid app id '%s'", app_id ? app_id : "(null)");
		return;
	}
	bool changed = false;
	pthread_mutex_lock(&g_server.tools_mutex);
	if (strcmp(g_server.app_id, app_id) != 0) {
		snprintf(g_server.app_id, sizeof(g_server.app_id), "%s", app_id);
		changed = true;
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	if (changed) {
		// The id rides tools/list _meta; a list_changed makes consumers
		// (the workspace aggregator) re-read it and re-prefix.
		notify_tools_changed();
	}
}

// ---------- Built-in echo tool (slice 1 handshake check) ----------

// ---------- Built-in tail_log tool ----------

static cJSON *
tool_tail_log(const cJSON *params, void *userdata)
{
	(void)userdata;
	uint64_t since = 0;
	size_t max_entries = 128;
	if (params != NULL) {
		const cJSON *s = cJSON_GetObjectItemCaseSensitive(params, "since");
		if (cJSON_IsNumber(s)) {
			since = (uint64_t)s->valuedouble;
		}
		const cJSON *m = cJSON_GetObjectItemCaseSensitive(params, "max");
		if (cJSON_IsNumber(m)) {
			double mv = m->valuedouble;
			if (mv > 0 && mv <= 1024) {
				max_entries = (size_t)mv;
			}
		}
	}

	struct mcp_log_entry *buf = calloc(max_entries, sizeof(*buf));
	if (buf == NULL) {
		return NULL;
	}
	size_t count = 0;
	uint64_t next_cursor = since, dropped = 0;
	mcp_log_ring_read(since, buf, max_entries, &count, &next_cursor, &dropped);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "cursor", (double)next_cursor);
	cJSON_AddNumberToObject(r, "dropped", (double)dropped);
	cJSON *arr = cJSON_CreateArray();
	static const char *level_names[] = {"trace", "debug", "info", "warn", "error", "raw"};
	for (size_t i = 0; i < count; i++) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "seq", (double)buf[i].seq);
		cJSON_AddNumberToObject(e, "ts_ns", (double)buf[i].timestamp_ns);
		int lv = (int)buf[i].level;
		cJSON_AddStringToObject(e, "level",
		                        (lv >= 0 && lv < (int)(sizeof(level_names) / sizeof(level_names[0])))
		                            ? level_names[lv]
		                            : "unknown");
		cJSON_AddStringToObject(e, "text", buf[i].text);
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(r, "entries", arr);
	free(buf);
	return r;
}

static const struct mcp_tool TAIL_LOG_TOOL = {
    .name = "tail_log",
    .description =
        "Return buffered U_LOG lines with seq > `since`. Pass the returned `cursor` as `since` "
        "on the next call to stream. `dropped` indicates how many entries were evicted before "
        "the caller read them (ring size is fixed).",
    .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"integer\",\"default\":0},"
        "\"max\":{\"type\":\"integer\",\"default\":128,\"maximum\":1024}}}",
    .fn = tool_tail_log,
    .userdata = NULL,
};

static cJSON *
tool_echo(const cJSON *params, void *userdata)
{
	(void)userdata;
	cJSON *result = cJSON_CreateObject();
	cJSON_AddItemToObject(result, "echo", params != NULL ? cJSON_Duplicate(params, 1) : cJSON_CreateNull());
	return result;
}

static const struct mcp_tool ECHO_TOOL = {
    .name = "echo",
    .description = "Echoes the params back in {\"echo\": <params>}. Used for handshake testing.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = tool_echo,
    .userdata = NULL,
};

// ---------- MCP protocol methods ----------

static const char *
group_name(enum mcp_tool_group group)
{
	switch (group) {
	case MCP_TOOL_GROUP_APP: return "app";
	case MCP_TOOL_GROUP_WORKSPACE: return "workspace";
	case MCP_TOOL_GROUP_CAPTURE: return "capture";
	case MCP_TOOL_GROUP_DIAGNOSTIC:
	default: return "diagnostic";
	}
}

static cJSON *
build_tools_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	pthread_mutex_lock(&g_server.tools_mutex);
	for (size_t i = 0; i < g_server.tool_count; i++) {
		const struct mcp_tool *t = g_server.tools[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", t->name);
		if (t->description != NULL) {
			cJSON_AddStringToObject(e, "description", t->description);
		}
		if (t->input_schema_json != NULL) {
			cJSON *schema = cJSON_Parse(t->input_schema_json);
			if (schema != NULL) {
				cJSON_AddItemToObject(e, "inputSchema", schema);
			}
		}
		cJSON *meta = cJSON_CreateObject();
		cJSON_AddStringToObject(meta, "displayxr/group", group_name(t->group));
		cJSON_AddItemToObject(e, "_meta", meta);
		cJSON_AddItemToArray(arr, e);
	}
	pthread_mutex_unlock(&g_server.tools_mutex);
	return arr;
}

static char *
handle_request(const cJSON *req, struct mcp_client *client)
{
	const cJSON *method_node = cJSON_GetObjectItemCaseSensitive(req, "method");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
	const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
	bool is_notification = (id == NULL);
	const char *method = cJSON_IsString(method_node) ? method_node->valuestring : NULL;

	if (method == NULL) {
		return is_notification ? NULL : jsonrpc_error(id, -32600, "invalid request: missing method");
	}

	// Notifications are fire-and-forget; MCP sends notifications/initialized.
	if (is_notification) {
		return NULL;
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
		cJSON_AddStringToObject(info, "name", "displayxr-mcp");
		cJSON_AddStringToObject(info, "version", DISPLAYXR_MCP_VERSION);
		pthread_mutex_lock(&g_server.tools_mutex);
		if (g_server.app_id[0] != '\0') {
			cJSON_AddStringToObject(info, "appId", g_server.app_id);
		}
		pthread_mutex_unlock(&g_server.tools_mutex);
		cJSON_AddItemToObject(result, "serverInfo", info);
		// Mark the client notification-eligible only once it has done
		// the MCP handshake — pre-initialize notifications confuse
		// strict clients.
		if (client != NULL) {
			client->initialized = true;
		}
		return jsonrpc_result(id, result);
	}

	if (strcmp(method, "ping") == 0) {
		return jsonrpc_result(id, cJSON_CreateObject());
	}

	if (strcmp(method, "tools/list") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "tools", build_tools_list());
		// Result-level _meta carries the app id so consumers that joined
		// before xrSetMCPAppInfoEXT ran can re-read it on list_changed.
		pthread_mutex_lock(&g_server.tools_mutex);
		if (g_server.app_id[0] != '\0') {
			cJSON *meta = cJSON_CreateObject();
			cJSON_AddStringToObject(meta, "displayxr/appId", g_server.app_id);
			cJSON_AddItemToObject(result, "_meta", meta);
		}
		pthread_mutex_unlock(&g_server.tools_mutex);
		return jsonrpc_result(id, result);
	}

	if (strcmp(method, "tools/call") == 0) {
		const cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(params, "name");
		const cJSON *tool_args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
		if (!cJSON_IsString(tool_name)) {
			return jsonrpc_error(id, -32602, "tools/call: missing string 'name'");
		}
		const struct mcp_tool *tool = find_tool(tool_name->valuestring);
		if (tool == NULL) {
			return jsonrpc_error(id, -32601, "tool not found");
		}
		cJSON *inner = tool->fn(tool_args, tool->userdata);
		if (inner == NULL) {
			return jsonrpc_error(id, -32000, "tool handler failed");
		}
		// MCP wraps tool results in { content: [{type:'text', text: <json>}] }
		// so clients without per-tool schema rendering still see the payload.
		cJSON *result = cJSON_CreateObject();
		cJSON *content = cJSON_CreateArray();
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "type", "text");
		char *inner_str = cJSON_PrintUnformatted(inner);
		cJSON_AddStringToObject(item, "text", inner_str != NULL ? inner_str : "");
		free(inner_str);
		cJSON_AddItemToArray(content, item);
		cJSON_AddItemToObject(result, "content", content);
		// Also expose the structured result for programmatic consumers.
		cJSON_AddItemToObject(result, "structured", inner);
		return jsonrpc_result(id, result);
	}

	return jsonrpc_error(id, -32601, "method not found");
}

// ---------- Per-connection serve loop ----------

static void
serve(struct mcp_client *client)
{
	for (;;) {
		size_t len = 0;
		char *body = read_frame(client->conn, &len);
		if (body == NULL) {
			return; // EOF, abort, or framing error.
		}
		cJSON *req = cJSON_ParseWithLength(body, len);
		free(body);

		char *reply = NULL;
		if (req == NULL) {
			reply = jsonrpc_error(NULL, -32700, "parse error");
		} else {
			reply = handle_request(req, client);
			cJSON_Delete(req);
		}
		if (reply != NULL) {
			// Serialize against notification broadcasts from other threads.
			pthread_mutex_lock(&client->write_mutex);
			bool ok = write_frame(client->conn, reply);
			pthread_mutex_unlock(&client->write_mutex);
			free(reply);
			if (!ok) {
				return;
			}
		}
	}
}

static void *
client_thread(void *arg)
{
	struct mcp_client *client = arg;
	serve(client);

	// Unwind the slot. Broadcasters hold conns_mutex while writing, so
	// once the slot is cleared nobody else touches the conn and the
	// close/free here is safe.
	pthread_mutex_lock(&g_server.conns_mutex);
	struct mcp_conn *conn = client->conn;
	client->conn = NULL;
	client->initialized = false;
	pthread_detach(client->thread);
	g_server.active_clients--;
	if (g_server.active_clients == 0) {
		pthread_cond_broadcast(&g_server.conns_drained);
	}
	pthread_mutex_unlock(&g_server.conns_mutex);
	mcp_conn_close(conn);
	return NULL;
}

// ---------- Thread entry ----------

static void *
server_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct mcp_conn *c = mcp_listener_accept(g_server.listener);
		if (c == NULL) {
			return NULL; // listener closed.
		}

		pthread_mutex_lock(&g_server.conns_mutex);
		struct mcp_client *slot = NULL;
		for (size_t i = 0; i < MAX_CONNS; i++) {
			if (g_server.clients[i].conn == NULL) {
				slot = &g_server.clients[i];
				break;
			}
		}
		if (slot == NULL) {
			pthread_mutex_unlock(&g_server.conns_mutex);
			MCP_LOG_W(LOG_PFX "client limit (%d) reached; rejecting connection", MAX_CONNS);
			mcp_conn_close(c);
			continue;
		}
		slot->conn = c;
		slot->initialized = false;
		int rc = pthread_create(&slot->thread, NULL, client_thread, slot);
		if (rc != 0) {
			MCP_LOG_W(LOG_PFX "client pthread_create failed: %s", strerror(rc));
			slot->conn = NULL;
			pthread_mutex_unlock(&g_server.conns_mutex);
			mcp_conn_close(c);
			continue;
		}
		g_server.active_clients++;
		pthread_mutex_unlock(&g_server.conns_mutex);
	}
}

// ---------- Public start/stop ----------

static void
start_with_listener(struct mcp_listener *listener, const char *endpoint_label)
{
	// One-time init of the per-slot write mutexes (slots are static
	// storage; PTHREAD_MUTEX_INITIALIZER can't reach array members).
	static bool slots_init = false;
	if (!slots_init) {
		for (size_t i = 0; i < MAX_CONNS; i++) {
			pthread_mutex_init(&g_server.clients[i].write_mutex, NULL);
		}
		slots_init = true;
	}

	g_server.listener = listener;
	int rc = pthread_create(&g_server.thread, NULL, server_thread, NULL);
	if (rc != 0) {
		MCP_LOG_W(LOG_PFX "pthread_create failed: %s", strerror(rc));
		mcp_listener_close(g_server.listener);
		g_server.listener = NULL;
		return;
	}
	g_server.thread_started = true;
	MCP_LOG_I(LOG_PFX "server started (%s)", endpoint_label);
}

bool
mcp_check_env_or(bool fallback)
{
	const char *flag = getenv("DISPLAYXR_MCP");
	if (flag == NULL) {
		return fallback;
	}
	// Explicit override: empty or leading '0' force-disables; anything
	// else force-enables. Symmetric with the historical mcp_enabled()
	// semantics so consumers that pre-date this API don't change.
	if (flag[0] == '\0' || flag[0] == '0') {
		return false;
	}
	return true;
}

static void
register_builtins(void)
{
	// Start the log ring first so the embedder's logging hook can push
	// bring-up messages into it, then register built-in tools.
	// Initialize the safety-model allowlist — tool handlers query it
	// per-call.
	mcp_log_ring_start();
	mcp_allowlist_init();
	mcp_server_register_tool(&ECHO_TOOL);
	mcp_server_register_tool(&TAIL_LOG_TOOL);
}

void
mcp_server_start(void)
{
	if (g_server.thread_started) {
		return;
	}
	register_builtins();

	struct mcp_listener *listener = mcp_listener_open(mcp_self_pid());
	if (listener == NULL) {
		MCP_LOG_W(LOG_PFX "failed to open listener; MCP disabled");
		return;
	}
	char label[64];
	snprintf(label, sizeof(label), "pid=%ld", (long)mcp_self_pid());
	start_with_listener(listener, label);
}

void
mcp_server_start_named(const char *role)
{
	if (g_server.thread_started) {
		return;
	}
	if (role == NULL || role[0] == '\0') {
		MCP_LOG_W(LOG_PFX "named start requires a non-empty role");
		return;
	}
	register_builtins();

	struct mcp_listener *listener = mcp_listener_open_named(role);
	if (listener == NULL) {
		MCP_LOG_W(LOG_PFX "failed to open named listener '%s'; MCP disabled", role);
		return;
	}
	char label[96];
	snprintf(label, sizeof(label), "role=%s pid=%ld", role, (long)mcp_self_pid());
	start_with_listener(listener, label);
}

void
mcp_server_maybe_start(void)
{
	if (mcp_check_env_or(false)) {
		mcp_server_start();
	}
}

void
mcp_server_maybe_start_named(const char *role)
{
	if (mcp_check_env_or(false)) {
		mcp_server_start_named(role);
	}
}

void
mcp_server_stop(void)
{
	if (!g_server.thread_started) {
		return;
	}
	// 1. Stop accepting (wakes the accept thread).
	mcp_listener_close(g_server.listener);
	g_server.listener = NULL;
	pthread_join(g_server.thread, NULL);

	// 2. Abort live connections — their serve threads wake, clear their
	//    slots, and signal conns_drained. Abort (not close): ownership
	//    of the conn stays with its serve thread.
	pthread_mutex_lock(&g_server.conns_mutex);
	for (size_t i = 0; i < MAX_CONNS; i++) {
		if (g_server.clients[i].conn != NULL) {
			mcp_conn_abort(g_server.clients[i].conn);
		}
	}
	while (g_server.active_clients > 0) {
		pthread_cond_wait(&g_server.conns_drained, &g_server.conns_mutex);
	}
	pthread_mutex_unlock(&g_server.conns_mutex);

	g_server.thread_started = false;
	mcp_log_ring_stop();
	MCP_LOG_I(LOG_PFX "server stopped");
}
