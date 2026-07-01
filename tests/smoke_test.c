// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*
 * Standalone smoke test for the displayxr_mcp framework.
 *
 * Spins up an MCP server on /tmp/displayxr-mcp-smoke.sock with a single
 * mock tool (mock_list_windows) that returns fixed window data, then
 * forks the displayxr-mcp adapter and exercises:
 *   - initialize
 *   - tools/list
 *   - tools/call mock_list_windows
 *
 * Verifies the JSON-RPC envelope, tool dispatch, and stdio↔socket
 * pump. This is the same wire path Phase B (in shell-pvt) and Phase A
 * (in the runtime) use — only the tool implementations differ.
 *
 * Build: enabled by -DDISPLAYXR_MCP_BUILD_TESTS=ON.
 * Run:   ctest -V (or ./build/tests/displayxr_mcp_smoke)
 */

#include <displayxr_mcp/mcp_server.h>

#include <cjson/cJSON.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static cJSON *
mock_list_windows(const cJSON *params, void *userdata)
{
	(void)params;
	(void)userdata;
	cJSON *arr = cJSON_CreateArray();
	cJSON *w = cJSON_CreateObject();
	cJSON_AddNumberToObject(w, "id", 42);
	cJSON_AddStringToObject(w, "name", "mock_app");
	cJSON_AddBoolToObject(w, "focused", true);
	cJSON_AddItemToArray(arr, w);
	return arr;
}

static const struct mcp_tool MOCK_TOOL = {
    .name = "mock_list_windows",
    .description = "Smoke-test stand-in. Returns one fake window.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = mock_list_windows,
};

static int
expect_substring(const char *haystack, const char *needle, const char *label)
{
	if (strstr(haystack, needle) == NULL) {
		fprintf(stderr, "FAIL: %s — expected to find '%s' in:\n%s\n", label, needle, haystack);
		return 1;
	}
	printf("  ok: %s\n", label);
	return 0;
}

static int
send_frame(FILE *f, const char *body)
{
	size_t len = strlen(body);
	if (fprintf(f, "Content-Length: %zu\r\n\r\n", len) < 0) {
		return -1;
	}
	if (fwrite(body, 1, len, f) != len) {
		return -1;
	}
	fflush(f);
	return 0;
}

// Read one Content-Length-framed body. Caller must free *out.
static int
recv_frame(FILE *f, char **out)
{
	char line[256];
	size_t content_length = 0;
	bool have_length = false;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '\r' || line[0] == '\n') {
			break;
		}
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_length = (size_t)strtoul(line + 15, NULL, 10);
			have_length = true;
		}
	}
	if (!have_length || content_length == 0) {
		return -1;
	}
	*out = malloc(content_length + 1);
	if (*out == NULL) {
		return -1;
	}
	if (fread(*out, 1, content_length, f) != content_length) {
		free(*out);
		return -1;
	}
	(*out)[content_length] = '\0';
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <path-to-displayxr-mcp-adapter>\n", argv[0]);
		return 2;
	}
	const char *adapter = argv[1];

	// Start the server.
	setenv("DISPLAYXR_MCP", "1", 1);
	mcp_server_register_tool(&MOCK_TOOL);
	mcp_server_maybe_start_named("smoke");

	// Give the server thread a beat to bind.
	usleep(100 * 1000);

	// Fork the adapter as a subprocess: parent writes JSON-RPC to its
	// stdin, reads from its stdout. Two pipes wired to popen-style.
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0 || pipe(from_child) != 0) {
		perror("pipe");
		return 1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);
		close(to_child[1]);
		close(from_child[0]);
		execl(adapter, adapter, "--target", "smoke", (char *)NULL);
		perror("exec");
		_exit(127);
	}
	close(to_child[0]);
	close(from_child[1]);
	FILE *out = fdopen(to_child[1], "w");
	FILE *in = fdopen(from_child[0], "r");

	int failures = 0;

	// 1. initialize
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
	char *body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no initialize response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"protocolVersion\"", "initialize: protocolVersion");
		failures += expect_substring(body, "\"displayxr-mcp\"", "initialize: server name");
		free(body);
	}

	// 2. tools/list
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
	body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no tools/list response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"mock_list_windows\"", "tools/list: mock tool present");
		failures += expect_substring(body, "\"echo\"", "tools/list: built-in echo present");
		free(body);
	}

	// 3. tools/call mock_list_windows
	send_frame(out, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
	                "\"params\":{\"name\":\"mock_list_windows\",\"arguments\":{}}}");
	body = NULL;
	if (recv_frame(in, &body) != 0) {
		fprintf(stderr, "FAIL: no tools/call response\n");
		failures++;
	} else {
		failures += expect_substring(body, "\"mock_app\"", "tools/call: returned mock window name");
		failures += expect_substring(body, "\"id\":42", "tools/call: returned mock window id");
		free(body);
	}

	// Tear down: close adapter stdin so it exits cleanly.
	fclose(out);
	fclose(in);
	int status = 0;
	waitpid(pid, &status, 0);

	mcp_server_stop();

	if (failures == 0) {
		printf("=== smoke test PASSED ===\n");
		return 0;
	}
	printf("=== smoke test FAILED (%d) ===\n", failures);
	return 1;
}
