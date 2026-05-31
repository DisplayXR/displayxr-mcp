#!/usr/bin/env bash
# Build the macOS .pkg for DisplayXR MCP Tools.
#
# Usage:
#   DISPLAYXR_VERSION=0.3.3 ./build_installer.sh <adapter-binary> [output.pkg]
#
# <adapter-binary> is the full path to the cmake-built displayxr-mcp
# executable (e.g. build/displayxr-mcp). Output defaults to
# ./DisplayXRMCP-<version>.pkg in the current dir.
#
# Two-stage build (mirroring displayxr-runtime / Gauss demo):
#   1. pkgbuild --root <staging> --scripts scripts/   → mcp.pkg
#   2. productbuild --distribution Distribution.xml … → DisplayXRMCP-<v>.pkg
#
# Payload layout:
#   /usr/local/bin/displayxr-mcp                                       — adapter
#   /Library/Application Support/DisplayXR/Capabilities/MCP/AdapterPath  — text
#   /Library/Application Support/DisplayXR/Capabilities/MCP/Version      — text
# `Enabled` is written by the postinstall script (not staged) so the
# capability bit is owned root:wheel regardless of who staged the payload.
#
# The .pkg is ad-hoc signed only. Full Developer ID signing is a parallel
# effort tracked in DisplayXR/displayxr-runtime#280.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ADAPTER_BIN="${1:?Usage: $0 <adapter-binary-path> [output.pkg]}"
VERSION="${DISPLAYXR_VERSION:-0.0.0}"
OUTPUT_PKG="${2:-$(pwd)/DisplayXRMCP-${VERSION}.pkg}"

if [ ! -f "$ADAPTER_BIN" ]; then
    echo "Error: adapter binary not found at '$ADAPTER_BIN'" >&2
    exit 1
fi
if [ ! -x "$ADAPTER_BIN" ]; then
    echo "Error: adapter binary at '$ADAPTER_BIN' is not executable" >&2
    exit 1
fi

# Resolve to absolute paths so cd around doesn't break them.
ADAPTER_BIN="$(cd "$(dirname "$ADAPTER_BIN")" && pwd)/$(basename "$ADAPTER_BIN")"
OUTPUT_PKG_DIR="$(cd "$(dirname "$OUTPUT_PKG")" && pwd)"
OUTPUT_PKG="$OUTPUT_PKG_DIR/$(basename "$OUTPUT_PKG")"
mkdir -p "$OUTPUT_PKG_DIR"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Building DisplayXRMCP installer"
echo "    version:     $VERSION"
echo "    adapter:     $ADAPTER_BIN"
echo "    output:      $OUTPUT_PKG"
echo "    workdir:     $WORK_DIR"

# --- Stage payload --------------------------------------------------------
PAYLOAD_ROOT="$WORK_DIR/payload"
BIN_DIR="$PAYLOAD_ROOT/usr/local/bin"
CAP_DIR="$PAYLOAD_ROOT/Library/Application Support/DisplayXR/Capabilities/MCP"
mkdir -p "$BIN_DIR" "$CAP_DIR"

cp "$ADAPTER_BIN" "$BIN_DIR/displayxr-mcp"
chmod 0755 "$BIN_DIR/displayxr-mcp"

# Ad-hoc re-sign in case the cmake-built binary lost its signature in
# transit (e.g. across a tar archive in CI). `codesign --force --sign -`
# is a no-op-equivalent for already-correctly-signed binaries.
codesign --force --sign - "$BIN_DIR/displayxr-mcp" 2>/dev/null || true

# Companion metadata. Enabled is intentionally NOT staged — postinstall
# writes it as root so the gate file is root:wheel 0644 regardless of who
# ran this script. See installer/macos/scripts/postinstall.
printf '/usr/local/bin/displayxr-mcp' > "$CAP_DIR/AdapterPath"
printf '%s' "$VERSION"               > "$CAP_DIR/Version"
chmod 0644 "$CAP_DIR/AdapterPath" "$CAP_DIR/Version"

# Strip stray AppleDouble metadata if any crept in.
find "$PAYLOAD_ROOT" \( -name '._*' -o -name '.DS_Store' \) -delete

# --- pkgbuild: component package ------------------------------------------
COMPONENT_PKG="$WORK_DIR/mcp.pkg"
pkgbuild \
    --root "$PAYLOAD_ROOT" \
    --scripts "$SCRIPT_DIR/scripts" \
    --identifier com.displayxr.mcp \
    --version "$VERSION" \
    --install-location / \
    --ownership recommended \
    "$COMPONENT_PKG"

# --- productbuild: distribution wrapper -----------------------------------
DIST_XML="$SCRIPT_DIR/Distribution.xml"
RESOURCES_DIR="$SCRIPT_DIR/resources"

if [ ! -f "$DIST_XML" ]; then
    echo "Error: $DIST_XML not found" >&2
    exit 1
fi
if [ ! -d "$RESOURCES_DIR" ]; then
    echo "Error: $RESOURCES_DIR not found" >&2
    exit 1
fi

productbuild \
    --distribution "$DIST_XML" \
    --resources "$RESOURCES_DIR" \
    --package-path "$WORK_DIR" \
    "$OUTPUT_PKG"

echo "==> Built: $OUTPUT_PKG"
ls -lh "$OUTPUT_PKG"
