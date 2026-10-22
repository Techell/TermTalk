<p align="center">
  <img src="assets/termtalk.svg" alt="TermTalk — Messages. Files. Terminal." width="680">
</p>

# TermTalk

[![Windows CI](https://github.com/Techell/TermTalk/actions/workflows/windows.yml/badge.svg)](https://github.com/Techell/TermTalk/actions/workflows/windows.yml)

A terminal messenger for Windows, written in C. Private conversations, file transfers
and a keyboard-driven interface — with a small, modular client/server core.

**Windows 10/11 · C11 · TCP · English / Русский**

> Early development. Traffic and local history are currently unencrypted.
> Use with trusted peers on trusted networks. See [security](SECURITY.md).

## Features

- Private messaging through a central server; Unicode nicknames and messages.
- Console interface with contacts, command suggestions, Tab completion and 20-command history.
- Saved servers, connection status, round-trip latency and reconnect without restarting.
- Nickname editing before connecting and after a rejected nickname: **F7** in the server menu.
- File offers with explicit accept/reject, progress, SHA-256 checking and a single-pane file browser.
- Optional local conversation history; English and Russian interface switching at any time.
- Configurable server capacity: 1–32 participants; default file limit: 32 GiB per transfer.

Group chats, offline delivery and encrypted connections are planned, not implemented.

## Build

Requires Windows, CMake 3.16+ and a C11 compiler with the Windows SDK.
The locally verified toolchain is MinGW GCC 6.3.0; MSVC is not yet verified.
No third-party runtime libraries are required.

With MinGW available in PATH, run from PowerShell:

```powershell
.\comp.bat
```

This configures and builds into `build/`. The script uses the Windows short path
for compatibility with older MinGW Make versions. On an ASCII path, the equivalent is:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick start

Start a server:

```powershell
.\build\server.exe 5050
```

Start the client in a second terminal:

```powershell
.\build\client.exe
```

Choose `127.0.0.1:5050` or enter a server address. Set your nickname using **F7**
or the nickname prompt. A rejected nickname can be changed in the same menu.
Run another client with a different nickname, select a contact with **Tab → arrows → Enter**,
and type a message. `/help` lists commands; **F2** switches EN/RU.

You can also provide the address and nickname directly:

```powershell
.\build\client.exe 127.0.0.1 5050 Alice --lang en
.\build\server.exe 5050 --max-clients 16 --max-file-gib 32
```

Use `/sendfile` or **F6** to select a file, `/accept` to choose a destination,
and `/reject` to decline. **F4** leaves the server; **F3** opens the server menu.
Full commands, storage paths and limits: [user guide](docs/UI.md) (Russian).

## Tests

Python 3 is optional for building the applications, but required for the four
integration tests. Install it before configuring CMake to run the full suite:

```powershell
ctest --test-dir build --output-on-failure
```

The suite includes native Windows console checks, a protocol parser, partial I/O,
queue pressure, reconnect and file-transfer scenarios.
[Verification and limitations](docs/BUILD-VERIFICATION.md).

## Project layout

```text
apps/       Client and server entry points
libs/       Transport, protocol, UI, storage and file-transfer modules
assets/     Project branding
tests/     Contract tests and independent protocol clients
tools/     Roadmap tooling
docs/      User guide, architecture and protocol specifications
.github/    CI and contribution templates
```

## Documentation and contributing

Start with the [documentation index](docs/README.md),
[contributing guide](CONTRIBUTING.md) and [roadmap](docs/ROADMAP.md).
The on-wire protocol remains **WSCK v2**; the rename does not change it.
Legacy `%LOCALAPPDATA%/WinSock` storage paths are preserved to retain existing profiles.

Current application version: **0.9.0**.
See [changelog](CHANGELOG.md).
Licensed under the [MIT License](LICENSE), copyright © 2026 Techell.
