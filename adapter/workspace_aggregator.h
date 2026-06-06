// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Entry point for the `--target workspace` aggregator mode.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Run the workspace aggregator on stdio until stdin EOF. Returns the
 * process exit code. @p expose_diagnostics surfaces DIAGNOSTIC-group
 * backend tools in the merged tools/list (hidden by default).
 */
int
workspace_aggregator_run(bool expose_diagnostics);

#ifdef __cplusplus
}
#endif
