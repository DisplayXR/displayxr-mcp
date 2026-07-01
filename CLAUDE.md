# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repo.

## What this repo is

A small **MCP (Model Context Protocol) server framework + stdio↔socket
adapter** used by the DisplayXR runtime + shell to expose live state
and control surfaces to AI agents (Claude Code, voice CLI, custom
clients). Hand-rolled JSON-RPC 2.0 with `Content-Length` framing,
unix-socket / Windows-named-pipe transport.

The framework knows **nothing** about OpenXR, the DisplayXR runtime,
or the shell. It's a generic embedder library: a process registers
tools (name + JSON Schema + handler) and the framework dispatches
them over a transport. The OpenXR-specific tools live in the
*consuming* repos (`oxr_mcp_tools.c` in runtime; `shell_mcp_tools.c`
in shell-pvt).

Public artifacts:
- **Static library** (`displayxr_mcp::mcp`) — consumed by runtime + shell via CMake FetchContent.
- **`displayxr-mcp` adapter binary** — stdio↔socket bridge so MCP clients (which speak stdio) can connect to in-process pipe servers (runtime + shell hosts).
- **`DisplayXRMCPSetup-<version>.exe`** — Windows NSIS installer that drops the adapter at `C:\Program Files\DisplayXR\MCP\bin\` and registers `HKLM\Software\DisplayXR\Capabilities\MCP\{Enabled, AdapterPath, Version}`.

## Where this fits in DisplayXR

```
                          this repo
                  ┌──────────────────────┐
                  │ displayxr-mcp        │
                  │  ├─ libdisplayxr_mcp │  ← framework lib
                  │  └─ displayxr-mcp    │  ← stdio↔socket adapter
                  └──────────────────────┘
                          ▲     ▲
            FetchContent  │     │ FetchContent
                          │     │
      ┌───────────────────┘     └─────────────────┐
      │                                            │
displayxr-runtime                            displayxr-shell-pvt
  Phase A — per-app                            Phase B — workspace
  introspection                                control
  (oxr_mcp_tools.c)                            (shell_mcp_tools.c)
  pipe: \\.\pipe\displayxr-mcp-<pid>           pipe: \\.\pipe\displayxr-mcp-shell
                          ▲
                          │ MCP client connects via stdio
                          │
                  ┌───────────────────┐
                  │ Claude Code,      │
                  │ voice CLI, agent  │
                  │ (uses adapter to  │
                  │  bridge stdio to  │
                  │  the named pipe)  │
                  └───────────────────┘
```

Spec: [`docs/mcp-spec.md`](docs/mcp-spec.md) — the authoritative
design doc. It explains the "stereo correctness + 6DOF arrangement"
thesis behind the agent surface, the gate (Capabilities registry +
env override), the tool conventions, and the named-pipe topology.

## Code structure

| Path | What it is |
|---|---|
| `include/displayxr_mcp/mcp_server.h` | Top-level public API. `mcp_server_start()`, `mcp_server_start_named(role)`, `mcp_check_env_or(fallback)`, tool registration. |
| `include/displayxr_mcp/mcp_transport.h` | Transport abstraction (Unix socket / Windows named pipe). |
| `include/displayxr_mcp/mcp_capture.h` | Frame-capture-as-tool helpers. |
| `include/displayxr_mcp/mcp_audit.h` | Tool-call audit trail (security/observability). |
| `include/displayxr_mcp/mcp_allowlist.h` | Per-tool allowlist (caller-side gate). |
| `include/displayxr_mcp/mcp_log_ring.h` | Bounded log buffer the server exposes via the `tail_log` tool. |
| `src/` | C implementation of the headers above + `os_compat.c` for pthread / Windows differences. |
| `adapter/displayxr_mcp.c` | The stdio↔socket adapter binary. `--target <pid:N\|shell>` selects the named pipe to bridge. |
| `external/cjson/` | Vendored cJSON. The framework reuses parent project's `cJSON::cJSON` target if one exists; falls back to this. |
| `installer/DisplayXRMCPInstaller.nsi` | NSIS installer for the adapter binary + Capabilities registry registration. |
| `tests/smoke_test.c` | ctest binary that exercises the wire protocol end-to-end. Runs on every CI build. |
| `docs/mcp-spec.md` | The product/protocol spec. **Update this if you change tool conventions or transport.** |

## Build commands

### Linux / macOS
```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

### Windows
```bat
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build -C Debug
```

Windows pulls `pthreads` from vcpkg (the framework uses pthread
mutexes; on MSVC we resolve via `PThreads4W::PThreads4W`).

### Consuming via FetchContent

The runtime + shell already do this. To embed in a new consumer:
```cmake
include(FetchContent)
FetchContent_Declare(displayxr_mcp
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-mcp
    GIT_TAG vX.Y.Z)
FetchContent_MakeAvailable(displayxr_mcp)
target_link_libraries(your_target PRIVATE displayxr_mcp::mcp)
```

The static lib pulls in vendored cJSON (or your parent project's
`cJSON::cJSON` if present) and pthreads (or its Windows equivalent).
You register tools, the framework dispatches them. **The framework
does not gate itself** — gating is the consumer's responsibility
(see `mcp_check_env_or` for the canonical pattern).

## Releasing

Preferred path: the user-level `/dxr-release` skill. It tags HEAD,
watches CI, reports the auto-bump + installer mirror outcome.

Manual fallback:
```bash
git tag -a vX.Y.Z -m "release notes ..."
git push origin vX.Y.Z
```

### What happens on tag push

1. `.github/workflows/build.yml` matrix-builds (ubuntu, macos, windows) + runs the smoke test.
2. `softprops/action-gh-release@v2` creates the GitHub Release; the Windows job attaches `DisplayXRMCPSetup-*.exe`.
3. **`dispatch-versions-bump` job** fires a `repository_dispatch` at
   `displayxr-runtime/versions-bump.yml` with `field: "mcp_tools"`.
4. The runtime side bumps `versions.json[mcp_tools]` on runtime/main + mirrors to installer/main. **No ABI gate** — mcp talks the MCP wire protocol, not a runtime C ABI; mismatches surface at runtime via `mcp_check_env_or` / spec version negotiation rather than at load time.

Full spec:
[`displayxr-runtime/docs/specs/runtime/versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md).

## Things to be careful about

- **Don't add platform-specific symbols to public headers.** This
  library is consumed by code that builds for Windows + Linux +
  macOS. Anything platform-specific goes in `src/os_compat.c`
  behind the abstractions in `os_compat.h`. Common gotcha: `pid_t`
  is POSIX-only — public APIs use `long`.
- **cJSON is vendored** (`external/cjson/`) so the framework is
  self-contained for embedders without their own cJSON. The
  consumer-vendoring deduplication is intentional — don't `find_package(cjson)` unconditionally.
- **Don't change tool conventions without updating `docs/mcp-spec.md`.**
  The spec is what runtime + shell + third-party workspace
  controllers code against. Wire-protocol changes that don't bump
  the spec version break consumers silently.
- **Don't host MCP yourself; embed.** This library does not run an
  MCP server process on its own — it's a static lib that runs
  inside a consumer (`libopenxr_displayxr` per-app, or
  `displayxr-shell.exe`). There's intentionally NO main() except
  for the stdio↔socket adapter binary.
- **Windows ships pthreads** via vcpkg. Do not assume `<unistd.h>`
  or `_Atomic` — the runtime CLAUDE.md's MinGW caveats apply here
  too (no `clock_gettime(CLOCK_MONOTONIC, …)`, no C11 atomics).
- **Per-PID named pipes** mean each app process gets its own
  `\\.\pipe\displayxr-mcp-<pid>` server. The shell uses the
  well-known `\\.\pipe\displayxr-mcp-shell`. There is intentionally
  NO `displayxr-mcp-service` endpoint — the runtime's IPC service
  does not host MCP (Phase A moved to per-process).

## License

[Apache-2.0](LICENSE) — same as the other wholly-owned DisplayXR repos (the runtime stays BSL-1.0 as a Monado-aligned fork).
