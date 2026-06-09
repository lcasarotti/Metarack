---
name: project-macos-accessibility
description: Fase successiva del progetto — implementazione accessibilità VoiceOver su macOS
metadata:
  type: project
---

## Obiettivo

Portare su macOS un'interfaccia accessibile analoga a quella Windows (`src/accessible/AccessibleWindow.cpp`), usando le API native macOS (NSAccessibility / AppKit) al posto di Win32/MSAA. L'utente usa VoiceOver.

**Why:** L'implementazione Windows è sostanzialmente completa (2026-06). macOS è la piattaforma di sviluppo principale dell'utente.

**How to apply:** Il nuovo codice andrà in `src/accessible/` con guard `#if defined ARCH_MAC`, probabilmente in file `.mm` (Objective-C++). La struttura UX di riferimento è l'AccessibleWindow Win32 già documentata in [[project-accessible-window]]. Le decisioni UX sono dell'utente (è non vedente, usa VoiceOver).

## Ambiente di sviluppo configurato (2026-06-09)

- `CLAUDE.md` — istruzioni progetto a portata di Claude
- `CLAUDE.local.md` — preferenze personali (lingua IT, ruolo, contesto C++)
- `.claude/hooks/format-cpp.sh` — astyle auto-format su `.cpp`/`.hpp` post-edit
- `.claude/hooks/lint-accessible.sh` — clang-tidy su `src/accessible/` post-edit
- `.claude/settings.json` — entrambi i hook cablati come PostToolUse
- `.claude/skills/verify/` — skill `/verify` per build + report warning
- `.clang-tidy` — check bugprone-*, clang-analyzer-*, objc-*; filtro su src/accessible/
- `compile_commands.json` — generato con `bear -- make` (gitignored; rigenerare dopo `make` con nuovi file)
- Plugin `skill-creator` installato

## Note tecniche macOS

- clang-tidy è in `/opt/homebrew/opt/llvm/bin/clang-tidy` (non nel PATH di default)
- `compile_commands.json` va rigenerato con `bear -- make` ogni volta che si aggiungono nuovi file `.mm`
- La build nativa macOS usa `make` senza variabili MinGW
