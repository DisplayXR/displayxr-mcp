# displayxr-mcp

Tiny embeddable [MCP (Model Context Protocol)](https://modelcontextprotocol.io)
server framework. Hand-rolled JSON-RPC 2.0 with `Content-Length` framing,
unix-socket / Windows-named-pipe transport, and a stdio bridge so any MCP
client (Claude Code, voice CLI, custom agent) can connect.

The library knows nothing about OpenXR, the DisplayXR runtime, or the shell.
It's a server-side framework: the embedder registers tools (name, JSON
Schema, handler) and the framework dispatches them.

Used by:

- [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime) — Phase A handle-app introspection (per-PID server in each app process).
- [DisplayXR Shell](https://github.com/DisplayXR/displayxr-shell-releases) — Phase B workspace control (per-shell-process server).
- Any third-party DisplayXR workspace controller that wants to expose an agent surface.

## Consuming via CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(displayxr_mcp
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-mcp
    GIT_TAG v0.2.0)
FetchContent_MakeAvailable(displayxr_mcp)

target_link_libraries(your_target PRIVATE displayxr_mcp::mcp)
```

The framework is a static library. The `displayxr-mcp` adapter binary
(stdio↔socket bridge) is also built by default; use
`-DDISPLAYXR_MCP_BUILD_ADAPTER=OFF` to skip it.

Dependencies are kept minimal: cJSON, pthreads, and the C runtime. No
DisplayXR runtime headers are required — you can use this in any C
project.

## Registering tools

```c
#include <displayxr_mcp/mcp_server.h>

static cJSON *
my_tool(const cJSON *params, void *userdata)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "hello", "world");
    return r; // ownership transferred to framework
}

static const struct mcp_tool MY_TOOL = {
    .name = "hello",
    .description = "Returns a greeting.",
    .input_schema_json = "{\"type\":\"object\"}",
    .fn = my_tool,
};

int main(void)
{
    mcp_server_register_tool(&MY_TOOL);

    // Two API styles for starting the server:
    //
    //   mcp_server_start_named(role)        — unconditional. Caller owns
    //                                          the policy gate.
    //   mcp_server_maybe_start_named(role)  — gated on DISPLAYXR_MCP
    //                                          env var. Convenience.
    //
    // For a registry / config / env-var hybrid, use mcp_check_env_or:
    //
    //   bool fallback = my_capability_enabled();   // your gate
    //   if (mcp_check_env_or(fallback)) {          // env var still wins
    //       mcp_server_start_named("my-app");      // \\.\pipe\displayxr-mcp-my-app
    //   }

    setenv("DISPLAYXR_MCP", "1", 1);   // or set externally
    mcp_server_maybe_start_named("my-app");
    pause();   // server thread is detached
}
```

Then connect:

```bash
displayxr-mcp --target my-app  # uses the adapter shipped with this lib
```

## Wiring into your logging system

The built-in `tail_log` tool reads from a ring buffer that the framework
exposes but does not populate on its own. To expose your runtime logs:

```c
#include <displayxr_mcp/mcp_log_ring.h>

// In your existing log callback / sink:
void my_log_sink(int level, const char *fmt, va_list args)
{
    mcp_log_ring_append(map_to_mcp_level(level), fmt, args);
    // ... your own destinations (stderr, file, etc.) ...
}
```

## Installers

CI builds a Windows installer (`DisplayXRMCPSetup-X.Y.Z.exe`) and a
macOS package (`DisplayXRMCP-X.Y.Z.pkg`) on tag pushes and attaches
both to the GitHub Release.

### Windows — `DisplayXRMCPSetup-*.exe` (NSIS)

- Drops `displayxr-mcp.exe` at `C:\Program Files\DisplayXR\MCP\bin\`.
- Writes `HKLM\Software\DisplayXR\Capabilities\MCP\{Enabled, AdapterPath, Version}`.
- Cleanly uninstalls via Add/Remove Programs.

### macOS — `DisplayXRMCP-*.pkg` (productbuild)

- Drops `displayxr-mcp` at `/usr/local/bin/`.
- Stages `AdapterPath` + `Version` text files at
  `/Library/Application Support/DisplayXR/Capabilities/MCP/`.
- Postinstall writes a single byte `1` to `…/Capabilities/MCP/Enabled`
  (root:wheel 0644). The runtime + shell read the first byte of this
  file at startup — the POSIX mirror of the Windows REG_DWORD gate.
- Uninstall via the bundled `installer/macos/uninstall.sh`.

### How the gate composes

The DisplayXR runtime + shell read the capability marker (registry on
Windows, file on macOS) at their startup to decide whether to spawn
their MCP server thread (`Capabilities\<name>` is the sibling extension
point to `WorkspaceControllers\<name>`). Two results: agent control
becomes a separate installer that's discoverable and opt-in, *and* the
`DISPLAYXR_MCP` env var still works as an override for dev / CI
workflows (see `mcp_check_env_or` above).

### Building installers locally

Windows:

```bat
cmake -S . -B build -DDISPLAYXR_MCP_BUILD_INSTALLER=ON
cmake --build build --config Release
:: produces build/installer/DisplayXRMCPSetup-X.Y.Z.0.exe
```

NSIS must be on `PATH`; the GitHub Actions windows-latest runner ships
it preinstalled at `C:\Program Files (x86)\NSIS\`.

macOS:

```bash
cmake -S . -B build -G Ninja -DDISPLAYXR_MCP_BUILD_MACOS_INSTALLER=ON
cmake --build build
# produces build/installer/DisplayXRMCP-X.Y.Z.pkg
```

`pkgbuild` + `productbuild` ship with the Xcode Command Line Tools (and
are preinstalled on the GitHub Actions macos-latest runner).

## Spec

See [`docs/mcp-spec.md`](docs/mcp-spec.md) for the protocol semantics,
tool conventions, and safety model (audit log + per-client allowlist).
The on-the-wire JSON-RPC framing matches MCP `2024-11-05`.

## License

[BSL-1.0](LICENSE) — Boost Software License.
