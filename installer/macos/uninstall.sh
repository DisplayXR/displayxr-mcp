#!/bin/bash
# Uninstall DisplayXR MCP Tools from macOS.
#
# Mirror of the NSIS uninstaller section in DisplayXRMCPInstaller.nsi:
#   - removes the adapter binary
#   - drops the Capabilities/MCP/ subtree (Enabled + AdapterPath + Version)
#   - forgets the pkg receipt so `pkgutil --pkgs` doesn't leave orphans
#
# Does NOT remove the parent `/Library/Application Support/DisplayXR/`
# tree — that's owned by the runtime installer, and clobbering it here
# would break a runtime that's still installed. Pruning is left to the
# runtime uninstall script when MCP is the last DisplayXR component on
# the box.

set -e

echo "=== DisplayXR MCP Tools Uninstaller ==="

echo "Removing /usr/local/bin/displayxr-mcp..."
sudo rm -f /usr/local/bin/displayxr-mcp

echo "Removing capability marker..."
# rm -rf the whole MCP subdirectory so the runtime + shell see the
# capability go away on next launch. Capabilities/ itself is left in
# place even if empty — it's a DisplayXR-owned namespace root.
sudo rm -rf "/Library/Application Support/DisplayXR/Capabilities/MCP"

echo "Forgetting installer receipt..."
# Identifier must match the --identifier value in build_installer.sh.
sudo pkgutil --forget com.displayxr.mcp 2>/dev/null || true

echo "=== DisplayXR MCP Tools uninstalled ==="
