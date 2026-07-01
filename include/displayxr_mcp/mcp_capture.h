// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Cross-thread hand-off for MCP capture_frame requests.
 *
 * The MCP server thread fills a request with a target PNG path and
 * blocks on a condvar. The compositor thread polls at end-of-frame,
 * does the GPU→CPU readback, encodes PNG, and signals done. Keeps
 * GPU calls on the thread that owns the resource.
 *
 * Each capture produces a single PNG of the content region that the
 * compositor hands to the display processor (tile_columns × view_width
 * by tile_rows × view_height) — i.e. what the app actually wrote into
 * its swapchain, not the worst-case atlas allocation.
 *
 * @ingroup aux_util
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_CAPTURE_PATH_MAX 256

/*!
 * Selects what state of the atlas is captured.
 *
 * - POST_COMPOSE (default): the atlas the compositor hands to the
 *   display processor — projection layers + window-space (HUD) + quads,
 *   composed across every tile. Useful for "what does the DP see?".
 * - PROJECTION_ONLY: the atlas with only projection-class layers
 *   (XR_TYPE_COMPOSITION_LAYER_PROJECTION{,_DEPTH} and quads). Useful
 *   for verifying tile-aware app rendering independent of chrome.
 *
 * Wire-format value is stable; compositors switch on it to pick the
 * capture call site.
 */
enum mcp_capture_mode
{
	MCP_CAPTURE_MODE_POST_COMPOSE = 0,
	MCP_CAPTURE_MODE_PROJECTION_ONLY = 1,
};

struct mcp_capture_request
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	char path[MCP_CAPTURE_PATH_MAX];
	uint32_t mode; // enum mcp_capture_mode
	bool pending;
	bool done;
	bool success;
};

void
mcp_capture_init(struct mcp_capture_request *req);

void
mcp_capture_fini(struct mcp_capture_request *req);

/*!
 * Register @p req as the MCP @c capture_frame handler. Safe to call
 * even when the MCP server is not running.
 */
void
mcp_capture_install(struct mcp_capture_request *req);

/*!
 * Unregister. Call from compositor destroy before freeing @p req.
 */
void
mcp_capture_uninstall(void);

/*!
 * Check for a pending request. Returns true if one is in flight and
 * fills @p out_path (must be at least @c MCP_CAPTURE_PATH_MAX bytes)
 * and @p out_mode (one of @ref mcp_capture_mode). @p out_mode may be
 * NULL if the caller only handles a single mode. Caller must follow
 * up with @ref mcp_capture_complete after the PNG is written (or
 * failed).
 */
bool
mcp_capture_poll(struct mcp_capture_request *req,
                  char *out_path,
                  uint32_t *out_mode);

/*!
 * Signal the waiting MCP thread.
 */
void
mcp_capture_complete(struct mcp_capture_request *req, bool success);

/*!
 * Return the currently installed capture request, or NULL if none.
 * Used by the service-side capture_frame tool to submit requests
 * directly to the compositor's capture handler.
 */
struct mcp_capture_request *
mcp_capture_get_installed(void);

/*!
 * Blocking handler for the Phase A per-PID capture_frame tool. Submits
 * a capture request with the requested @p mode and waits (up to 3 s)
 * for the compositor thread to complete it. Called from the oxr
 * tool_capture_frame handler.
 */
bool
mcp_capture_blocking_handler(const char *path,
                              uint32_t mode,
                              void *userdata);

typedef void (*mcp_capture_notify_fn)(void *req);

/*!
 * Register optional callbacks for Phase A (per-PID) integration.
 * Called by the oxr state tracker at startup to wire capture_frame
 * into the per-PID MCP server. Service processes don't call this.
 */
void
mcp_capture_set_notify(mcp_capture_notify_fn on_install,
                         mcp_capture_notify_fn on_uninstall);

#ifdef __cplusplus
}
#endif
