---
name: reference-github-repo
description: GitHub fork del progetto VCV Rack con le modifiche di accessibilità
metadata: 
  node_type: memory
  type: reference
  originSessionId: ccb1932a-ffe8-4322-9940-aea7f29fcb11
---

Il repo GitHub del progetto è **https://github.com/lcasarotti/Metarack**.

Il branch principale per il lavoro di accessibilità è `Screen-Reader-Accessibility`.

Il remote locale è configurato come `metarack` (origin punta all'upstream VCVRack/Rack). ⚠️ Il branch locale `v2` traccia `origin/v2` (= VCV upstream): per pushare sul fork serve SEMPRE l'esplicito `git push metarack v2:Screen-Reader-Accessibility`, e per le release `gh ... --repo lcasarotti/Metarack` (altrimenti gh prende origin = VCV).

## Prima release: v1.0-accessible (2026-06-25)

Pubblicata la **prima release di MetaRack**: tag `v1.0-accessible` (annotato, ora su commit `0f04e99b` dopo aver uniformato la versione installer a 1.0), release "MetaRack v1.0 — accessible (Rack 2.6.6)" con allegato l'installer `MetaRack-1.0-win.exe` (~20 MB). URL: https://github.com/lcasarotti/Metarack/releases/tag/v1.0-accessible. La versione mostrata (1.0) è la versione PRODOTTO MetaRack; 2.6.6 è solo la base Rack (vedi [[reference-installer]]).

Il commit della release include SOLO i 9 file MetaRack+build (accessibili/standalone, installer.nsi, Makefile con -lcomctl32, compile.mk, dep.mk, dep/Makefile). Restano fuori (volutamente, ancora non committati): puntatori submodule, plugins/.gitignore, e i sorgenti adapter CLAP (adapters/clap.cpp, claptest.cpp, spike.cpp = [[project-plugin-adapter]], lavoro separato in corso).
