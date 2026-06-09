# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: VCV Rack — Virtual Eurorack Modular Synthesizer

This is **Metarack**, a personal fork of VCV Rack with screen reader accessibility features (not for upstream contribution). The user is blind and uses NVDA/VoiceOver; they are the authority on accessibility UX decisions.

**Repo**: github.com/lcasarotti/Metarack (branch `v2`)  
**Platforms**: macOS (native `make`) and Windows (cross-compile from WSL Ubuntu with MinGW)

## Build & Dependencies

**Makefile-based build** (no npm/pip/cargo):
- `make dep` — compile all dependencies (first build only; takes several minutes)
- `make` — build libRack.so/dylib/dll + Rack executable
- `make run` — build + launch with debug flag (`./Rack -d`)
- `make clean` — remove build artifacts
- `make dist` — create distribution bundle (platform-specific)

**Dependencies are git submodules in `/dep/`** (GLFW, OpenSSL, libcurl, RtAudio, RtMidi, speexdsp, etc.). Precompiled `.a` files go to `dep/lib/`. If switching platforms, delete `build/` before rebuilding to avoid stale `.o` files.

**Platform detection** (`arch.mk`):
- Automatically detects CPU (x64/arm64) and OS (Mac/Win/Linux)
- Windows: must cross-compile from WSL with MinGW (`CC`, `CXX`, `STRIP`, `AR`, `WINDRES` vars set)
- Skills handle this: `/build`, `/build-plugin`, `/run-rack`

## Code Style

**Astyle 3.1+ formatter** (`.astylerc`):
- Style: Java-style braces, 2-space tabs (`indent=tab=2`)
- Pointer alignment: `int* p` not `int *p` (`align-pointer=type`)
- Operators padded; return types attached
- Post-edit hook (`.claude/hooks/format-cpp.sh`) auto-formats `.cpp`/`.hpp` files

**Languages**: C++11, C, Objective-C/C++ (macOS), Python (build), Bash

## Non-Obvious Gotchas

1. **Plugin build requires explicit `RACK_DIR`** — plugins use `RACK_DIR ?= ../..` which would resolve to `/` without it.

2. **First build takes several minutes** — `make dep` compiles all submodules. Subsequent builds are fast.

3. **Windows builds from WSL only** — never run `make` from PowerShell. Must pass full MinGW variable set; `nproc` doesn't exist in PowerShell so use `-j4`.

4. **Accessibility is authority-driven** — user is blind; accessibility UX decisions are theirs, not best-practice defaults. Defer to `.claude/memory/` docs when making changes to `src/accessible/`.

5. **No traditional test framework** — compiler warnings (`-Wall -Wextra`) are the main quality gate.

6. **macOS code signing** — `make package` requires hardcoded Developer ID identity. `make notarize` submits to Apple.

7. **Version from git tags** — `RACK_VERSION` derived from `git describe --tags --match "v2.*"` and baked into binary.

## Skills

- `/build` — Build Rack core (explains WSL cross-compile on Windows)
- `/build-plugin <Plugin>` — Build & install a plugin
- `/run-rack` — Build + launch with debug flag

## Memory & Context

See `.claude/memory/` for:
- User profile & accessibility authority
- Current accessible window architecture & TODOs
- Build command reference
- GitHub repo & installer build notes

