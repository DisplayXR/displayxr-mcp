// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Opt-in in-process MCP server for DisplayXR (Phase A).
 * @ingroup aux_util
 *
 * Enabled when the @c DISPLAYXR_MCP environment variable is set at
 * xrInstanceCreate time. Spawns a detached thread that binds a per-PID
 * unix socket and speaks MCP-style JSON-RPC 2.0 framed by Content-Length
 * headers.
 *
 * @see docs/roadmap/mcp-spec-v0.2.md
 * @see docs/roadmap/mcp-phase-a-plan.md
 */

#pragma once

#include <cjson/cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Tool handler callback. @p params is the JSON-RPC `params` object (may be
 * NULL). The handler returns a cJSON node that becomes the `result` field,
 * or NULL to signal an error — in which case the server replies with a
 * JSON-RPC internal-error response.
 */
typedef cJSON *(*mcp_tool_fn)(const cJSON *params, void *userdata);

/*!
 * Tool descriptor shown by `tools/list` and dispatched by `tools/call`.
 * All fields except @p fn are optional; the MCP protocol requires a
 * name + description and accepts a JSON Schema for parameters.
 */
struct mcp_tool
{
	const char *name;
	const char *description;
	const char *input_schema_json; //!< Optional static JSON Schema string.
	mcp_tool_fn fn;
	void *userdata;
};

/*!
 * Unconditionally start the server bound to a per-PID socket. The caller
 * is responsible for the policy gate (registry / env var / config).
 * Idempotent no-op if already running. Best-effort: failure logs and
 * returns without aborting.
 *
 * Used by handle apps that link the framework in-process.
 */
void
mcp_server_start(void);

/*!
 * Unconditionally start the server bound to a well-known named socket
 * (`/tmp/displayxr-mcp-<role>.sock` on POSIX,
 * `\\.\pipe\displayxr-mcp-<role>` on Windows). Idempotent.
 *
 * Used by singleton processes (e.g. @c displayxr-shell.exe) where the
 * adapter discovers the endpoint by role rather than PID. Pair with
 * @ref mcp_check_env_or for the standard env-var-overrides-default
 * gating policy.
 */
void
mcp_server_start_named(const char *role);

/*!
 * Convenience wrapper: start the per-PID server iff @c DISPLAYXR_MCP env
 * var is set to a non-zero value. Equivalent to
 * `if (mcp_check_env_or(false)) mcp_server_start();`. Kept for back-
 * compat with consumers from before the registry-based gate landed.
 */
void
mcp_server_maybe_start(void);

/*!
 * Convenience wrapper: start the named server iff @c DISPLAYXR_MCP env
 * var is set to a non-zero value. Equivalent to
 * `if (mcp_check_env_or(false)) mcp_server_start_named(role);`. Kept
 * for back-compat with consumers from before the registry-based gate
 * landed.
 */
void
mcp_server_maybe_start_named(const char *role);

/*!
 * Read the @c DISPLAYXR_MCP env var as an explicit override.
 *
 * - Var set to a non-zero, non-empty value (`"1"`, `"true"`, …) → returns @c true.
 * - Var set to `"0"` or empty string → returns @c false (force-disable).
 * - Var unset → returns @p fallback.
 *
 * Consumers that want their own primary gate (e.g. a Windows registry
 * lookup under @c HKLM\Software\DisplayXR\Capabilities\MCP) call this
 * with the registry verdict as @p fallback so the env var still wins
 * when explicitly set:
 *
 *   bool registry = win32_capability_mcp_enabled();
 *   if (mcp_check_env_or(registry)) mcp_server_start_named("shell");
 */
bool
mcp_check_env_or(bool fallback);

/*!
 * Stop the server (joins the thread, unlinks the socket). Safe to call
 * when the server was never started.
 */
void
mcp_server_stop(void);

/*!
 * Register a tool. Safe to call before or after the server starts.
 * The descriptor pointer must outlive the server (use static storage).
 */
void
mcp_server_register_tool(const struct mcp_tool *tool);

#ifdef __cplusplus
}
#endif
