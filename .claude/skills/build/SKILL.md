---
name: build
description: Build VCV Rack. Runs make dep first if dependencies haven't been compiled yet, otherwise just make. Surfaces compiler errors with file/line context.
---

Build VCV Rack. The project root is `C:\Rack`.

## Important: this is a Windows cross-compile from WSL

Rack is built for Windows by cross-compiling from WSL Ubuntu with the MinGW
toolchain. Do NOT run `make` directly from PowerShell — it will use the wrong
compiler. Every `make` invocation must go through WSL and pass the full MinGW
variable set.

`CROSS_COMPILE` alone is not enough: `arch.mk` uses it only to detect the
platform (`ARCH_WIN`), not to set `CC`/`CXX`. `WINDRES` must be passed
explicitly because the Makefile appends an extra dash (`$(CROSS_COMPILE)-windres`).
`nproc` doesn't exist in PowerShell, so use a fixed job count (e.g. `-j4`).

## Build logic

1. Check whether `dep/lib/` exists and has files. If it's missing or empty, run
   `make dep` first (compiles all submodule dependencies — takes several
   minutes; warn the user), then run `make`. Otherwise just run `make`.
2. If this is the first build right after a Linux build, the `build/` dir may
   hold `.o` files compiled without `ARCH_WIN` — prepend `rm -rf build/` before
   `make` to avoid stale objects.
3. If the build fails, show the first compiler error with file and line clearly
   identified, and suggest a likely fix if the cause is obvious (missing
   include, type mismatch, etc.).

## Command (use the Bash tool)

Build (incremental):

```bash
wsl -d Ubuntu -- bash -c "cd /mnt/c/Rack && make -j4 \
  CROSS_COMPILE=x86_64-w64-mingw32- \
  CC=x86_64-w64-mingw32-gcc \
  CXX=x86_64-w64-mingw32-g++ \
  STRIP=x86_64-w64-mingw32-strip \
  AR=x86_64-w64-mingw32-ar \
  WINDRES=x86_64-w64-mingw32-windres"
```

First build (dependencies not yet compiled) — same variables, target `dep` first:

```bash
wsl -d Ubuntu -- bash -c "cd /mnt/c/Rack && make dep \
  CROSS_COMPILE=x86_64-w64-mingw32- \
  CC=x86_64-w64-mingw32-gcc \
  CXX=x86_64-w64-mingw32-g++ \
  STRIP=x86_64-w64-mingw32-strip \
  AR=x86_64-w64-mingw32-ar \
  WINDRES=x86_64-w64-mingw32-windres"
```

The Ubuntu WSL distro starts automatically when invoked with `wsl -d Ubuntu`.
Pipe through `tail` to keep output manageable, e.g. append `2>&1 | tail -40`.
A successful build produces `Rack.exe` and `libRack.dll` in `C:\Rack`.
