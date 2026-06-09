---
name: verify
description: Build Rack and report compiler warnings or errors. Use after changes to confirm nothing is broken.
disable-model-invocation: true
---

Build Rack using the platform-appropriate command, then report the outcome.

## macOS (native build)
```bash
make -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -60
```

## Windows (WSL cross-compile)
Use the full MinGW command from the /build skill.

## Reporting
- Clean build: say so in one line.
- Warnings: list each one with file:line context.
- Errors: show the first error clearly with file:line, suggest a likely fix if the cause is obvious (missing include, type mismatch, etc.).

Do not make any code changes — this is a read-only check.
