// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*
 * Portable in-process test of the server + transport: runs on Windows
 * (named pipe) as well as POSIX (unix socket). The smoke test forks the
 * adapter and is POSIX-only; this one talks to the server directly
 * through the transport API, in one process:
 *
 *   1. start/stop cycles: start the server, connect a client to our own
 *      pid's endpoint, initialize + tools/list, leave a second client
 *      connected and idle (its serve thread parked in a read), stop the
 *      server. Stop must return (a watchdog fails the test otherwise)
 *      and both clients must see the connection end.
 *   2. stop while a client is connecting: a thread connects and drops
 *      connections in a loop while the main thread stops the server.
 *
 * Exercises the stop path an embedder hits on every xrDestroyInstance:
 * waking and freeing the listener, and aborting live connections.
 *
 * Build: enabled by -DDISPLAYXR_MCP_BUILD_TESTS=ON.
 */

#include <displayxr_mcp/mcp_server.h>
#include <displayxr_mcp/mcp_transport.h>

#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <time.h>
#endif

#define CYCLES 3
#define WATCHDOG_S 30

static void
sleep_ms(unsigned ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
#endif
}

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
	fflush(stdout);
}

// ---------------------------------------------------------------- watchdog

static volatile int g_phase = 0;

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code after exit()
#endif

static void *
watchdog_thread(void *arg)
{
	(void)arg;
	sleep_ms(WATCHDOG_S * 1000);
	fprintf(stderr, "FAIL: watchdog: still running after %d s (phase %d) — a stop or read hung\n",
	        WATCHDOG_S, g_phase);
	fflush(stderr);
	exit(3);
	return NULL;
}

// ---------------------------------------------------------------- mock tool

static cJSON *
mock_ping(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "pong", "transport_test");
	return o;
}

static const struct mcp_tool MOCK_TOOL = {
    .name = "transport_test_ping",
    .description = "Transport-test stand-in.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_ping,
    .group = MCP_TOOL_GROUP_APP,
};

// ---------------------------------------------------------------- framing

static bool
send_frame(struct mcp_conn *c, const char *body)
{
	char header[64];
	int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", strlen(body));
	return hlen > 0 && mcp_conn_write(c, header, (size_t)hlen) && mcp_conn_write(c, body, strlen(body));
}

// One Content-Length frame; caller frees. NULL on EOF / error.
static char *
recv_frame(struct mcp_conn *c)
{
	size_t content_length = 0;
	bool have_length = false;
	for (;;) {
		char line[256];
		size_t n = 0;
		for (;;) {
			if (n + 1 >= sizeof(line) || !mcp_conn_read(c, &line[n], 1)) {
				return NULL;
			}
			if (line[n++] == '\n') {
				break;
			}
		}
		line[n] = '\0';
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_length = (size_t)strtoul(line + 15, NULL, 10);
			have_length = true;
		}
	}
	if (!have_length || content_length == 0 || content_length > (1u << 22)) {
		return NULL;
	}
	char *body = malloc(content_length + 1);
	if (body == NULL || !mcp_conn_read(c, body, content_length)) {
		free(body);
		return NULL;
	}
	body[content_length] = '\0';
	return body;
}

// Send a request, return the reply body (caller frees) or NULL.
static char *
roundtrip(struct mcp_conn *c, const char *req)
{
	return send_frame(c, req) ? recv_frame(c) : NULL;
}

static struct mcp_conn *
connect_retry(long pid)
{
	for (int i = 0; i < 100; i++) {
		struct mcp_conn *c = mcp_conn_connect(pid);
		if (c != NULL) {
			return c;
		}
		sleep_ms(20);
	}
	return NULL;
}

// ---------------------------------------------------------------- 1. cycles

static void
one_cycle(int cycle)
{
	printf("cycle %d\n", cycle);
	mcp_server_start();
	long pid = mcp_self_pid();

	struct mcp_conn *c = connect_retry(pid);
	check(c != NULL, "client connected to own pid endpoint");
	if (c == NULL) {
		mcp_server_stop();
		return;
	}

	char *r = roundtrip(c,
	                    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
	                    "{\"protocolVersion\":\"2024-11-05\",\"clientInfo\":{\"name\":\"transport_test\"}}}");
	check(r != NULL && strstr(r, "\"protocolVersion\"") != NULL && strstr(r, "\"id\":1") != NULL,
	      "initialize answered");
	free(r);
	check(send_frame(c, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"),
	      "notifications/initialized sent");

	r = roundtrip(c, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
	check(r != NULL && strstr(r, "\"transport_test_ping\"") != NULL && strstr(r, "\"id\":2") != NULL,
	      "tools/list lists the registered tool");
	free(r);

	r = roundtrip(c,
	              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
	              "{\"name\":\"transport_test_ping\",\"arguments\":{}}}");
	check(r != NULL && strstr(r, "transport_test") != NULL && strstr(r, "\"id\":3") != NULL,
	      "tools/call dispatched");
	free(r);

	// A second, idle client: its serve thread stays parked in a read
	// until stop aborts it.
	struct mcp_conn *idle = connect_retry(pid);
	check(idle != NULL, "second client connected");
	sleep_ms(50); // let its serve thread reach the read

	g_phase = 10 + cycle;
	mcp_server_stop();
	check(true, "mcp_server_stop returned with clients connected");

	// Both connections are gone server-side; reads must end, not block.
	char byte;
	check(!mcp_conn_read(c, &byte, 1), "active client sees the connection closed");
	if (idle != NULL) {
		check(!mcp_conn_read(idle, &byte, 1), "idle client sees the connection closed");
		mcp_conn_close(idle);
	}
	mcp_conn_close(c);
}

// ---------------------------------------------------------------- 2. stop mid-connect

static volatile int g_stop_connector = 0;

static void *
connector_thread(void *arg)
{
	long pid = *(long *)arg;
	while (!g_stop_connector) {
		struct mcp_conn *c = mcp_conn_connect(pid);
		if (c != NULL) {
			mcp_conn_close(c);
		} else {
			sleep_ms(1);
		}
	}
	return NULL;
}

static void
stop_mid_connect(void)
{
	printf("stop while a client is connecting\n");
	for (int round = 0; round < 3; round++) {
		mcp_server_start();
		long pid = mcp_self_pid();
		struct mcp_conn *probe = connect_retry(pid);
		check(probe != NULL, "server up");
		mcp_conn_close(probe);

		g_stop_connector = 0;
		pthread_t t;
		pthread_create(&t, NULL, connector_thread, &pid);
		sleep_ms(100);
		g_phase = 20 + round;
		mcp_server_stop();
		g_stop_connector = 1;
		pthread_join(t, NULL);
		check(true, "mcp_server_stop returned while a client was connecting");
	}
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	pthread_t wd;
	pthread_create(&wd, NULL, watchdog_thread, NULL);
	pthread_detach(wd);

	mcp_server_register_tool(&MOCK_TOOL);
	for (int i = 0; i < CYCLES; i++) {
		one_cycle(i);
	}
	stop_mid_connect();

	if (g_failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("transport_test: all checks passed\n");
	return 0;
}
