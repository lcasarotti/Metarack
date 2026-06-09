---
name: build-plugin
description: Build and install a VCV Rack plugin (not Rack core). Cross-compiles a plugin under C:\Rack\<Plugin> from WSL with MinGW and installs the .vcvplugin into the Rack user dir. Pass the plugin folder name as the argument.
---

Build and install a VCV Rack **plugin** (not Rack core — for the core use the
`build` skill). The plugin folder lives under `C:\Rack\<Plugin>` (e.g.
`Fundamental`, `Befaco`, `AudibleInstruments`, `ESeries`, `Object`,
`VCV-Prototype`, `VCV-Recorder`).

The plugin to build is given as the skill argument. If no argument was provided,
ask the user which plugin folder to build before running anything.

## Why a plugin needs special handling

- Plugins use `RACK_DIR ?= ../..`, which from `C:\Rack\<Plugin>\` would resolve
  to `C:\` — so `RACK_DIR=/mnt/c/Rack` MUST be passed explicitly.
- Like the core, plugins cross-compile from WSL Ubuntu with MinGW, so the full
  MinGW variable set (`CC`/`CXX`/`STRIP`/`AR`/`WINDRES`) is required.
- `make install` copies the `.vcvplugin` into `$(RACK_USER_DIR)/plugins-win-x64/`.
  From WSL `$LOCALAPPDATA` is a Windows path, so `RACK_USER_DIR` must be set
  explicitly to a WSL path.

## Before building: submodules

Some plugins need submodules initialised first (run inside the plugin folder):

- **AudibleInstruments**: `git submodule update --init` (eurorack)
- **Befaco**: `git submodule update --init --recursive` (Iroi + OwlProgram + nested)
- **ESeries**, **Fundamental**: no submodules

If the build fails with missing source files (e.g. a missing `eurorack/` or
`OwlProgram/` dir), init the submodules and retry.

## Command (use the Bash tool)

Replace `<Plugin>` with the plugin folder name. This builds, then installs:

```bash
wsl -d Ubuntu -- bash -c "cd /mnt/c/Rack/<Plugin> && \
  make -j4 RACK_DIR=/mnt/c/Rack \
    CROSS_COMPILE=x86_64-w64-mingw32- \
    CC=x86_64-w64-mingw32-gcc \
    CXX=x86_64-w64-mingw32-g++ \
    STRIP=x86_64-w64-mingw32-strip \
    AR=x86_64-w64-mingw32-ar \
    WINDRES=x86_64-w64-mingw32-windres && \
  make RACK_DIR=/mnt/c/Rack \
    CROSS_COMPILE=x86_64-w64-mingw32- \
    CC=x86_64-w64-mingw32-gcc \
    CXX=x86_64-w64-mingw32-g++ \
    STRIP=x86_64-w64-mingw32-strip \
    AR=x86_64-w64-mingw32-ar \
    WINDRES=x86_64-w64-mingw32-windres \
    'RACK_USER_DIR=/mnt/c/Users/Luca Casarotti/AppData/Local/Rack2' install"
```

Notes:
- The Ubuntu WSL distro starts automatically when invoked with `wsl -d Ubuntu`.
- `nproc` doesn't exist in PowerShell — use a fixed `-j4`.
- Pipe through `tail` (`2>&1 | tail -40`) to keep output manageable.
- If the build fails, surface the first compiler error with file and line, and
  suggest a likely fix if the cause is obvious.
- To build WITHOUT installing, drop the second `make ... install` half.
