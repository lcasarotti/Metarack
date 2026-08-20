---
name: reference-memory-in-repo
description: "La memoria del progetto è nel repo git — come sincronizzarla tra macchine"
metadata:
  node_type: memory
  type: reference
  originSessionId: session-2026-06-09
---

Tutti i file di memoria sono stati copiati nel repo a `.claude/memory/` (commit dd02a609, branch v2, push su metarack 2026-06-09). Questo permette di portare il contesto su nuove macchine (es. Mac).

**Come caricare su una nuova macchina:** clona il repo, apri Claude Code nella cartella del progetto, poi chiedi: *"Leggi la memoria da `.claude/memory/MEMORY.md` e carica tutti i file lì"*. Claude Code li copierà nel suo sistema di memoria locale.

**Aggiornamento:** quando si aggiunge memoria significativa, aggiornare anche i file in `.claude/memory/` e committarli su metarack, così le macchine restano sincronizzate.

**Risincronizzata il 2026-08-20** sul branch `Screen-Reader-Accessibility` (non più v2): la copia nel repo era ferma a dd02a609, con 10 file su 15. La sincronizzazione è una copia secca dalla cartella viva (`~/.claude/projects/c--Rack/memory/`) a `.claude/memory/`: la copia viva è sempre il superset e quella nel repo non ha mai file esclusivi, quindi sovrascrivere è sicuro. Va rifatta a ogni cambio di memoria, altrimenti diverge in silenzio.
