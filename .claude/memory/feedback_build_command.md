---
name: feedback-build-command
description: Comando esatto per cross-compilare VCV Rack per Windows da WSL Ubuntu
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 5589ffab-b35e-4f15-815e-3fea3e32b6e1
---

Il build per Windows si fa da WSL Ubuntu con MinGW cross-compiler. Il comando corretto è:

```bash
wsl -d Ubuntu /bin/bash -c "cd /mnt/c/Rack && make \
  CROSS_COMPILE=x86_64-w64-mingw32- \
  CC=x86_64-w64-mingw32-gcc \
  CXX=x86_64-w64-mingw32-g++ \
  STRIP=x86_64-w64-mingw32-strip \
  AR=x86_64-w64-mingw32-ar \
  WINDRES=x86_64-w64-mingw32-windres"
```

**Why:** `CROSS_COMPILE` da solo non basta — `arch.mk` lo usa solo per rilevare la piattaforma (ARCH_WIN), non per impostare CC/CXX. `WINDRES` va passato esplicitamente perché il Makefile fa `$(CROSS_COMPILE)-windres` aggiungendo un trattino extra.

**How to apply:** Se si fa un primo build dopo aver compilato per Linux, aggiungere `rm -rf build/` prima di `make` per eliminare i `.o` compilati senza `ARCH_WIN`.

La distro Ubuntu WSL parte automaticamente quando invocata con `wsl -d Ubuntu`.

**Nota PowerShell:** usare `wsl -d Ubuntu -- bash -c "..."` (non `/bin/bash -c`). `nproc` non esiste in PowerShell — usare un numero fisso (es. `-j4`).

## Build plugin (non Rack core)

I plugin in `C:\Rack\PLUGIN\` usano `RACK_DIR ?= ../..` che da `C:\Rack\PLUGIN\` risolverebbe a `C:\` → **bisogna passare `RACK_DIR` esplicitamente**.

Comando per build + install di un plugin:
```powershell
wsl -d Ubuntu -- bash -c "cd /mnt/c/Rack/PLUGIN && make -j4 RACK_DIR=/mnt/c/Rack CROSS_COMPILE=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ STRIP=x86_64-w64-mingw32-strip AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres && make RACK_DIR=/mnt/c/Rack CROSS_COMPILE=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ STRIP=x86_64-w64-mingw32-strip AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres 'RACK_USER_DIR=/mnt/c/Users/Luca Casarotti/AppData/Local/Rack2' install"
```

`make install` usa `$(RACK_USER_DIR)/plugins-win-x64/` e ci copia il `.vcvplugin`. Da WSL `$LOCALAPPDATA` è un path Windows → impostare `RACK_USER_DIR` esplicitamente come path WSL.

## Abilitare ASIO (driver audio)

La SDK ASIO open è **già vendorizzata** in `rtaudio/include/` (asio.cpp, asiodrivers.cpp, asiolist.cpp, iasiothiscallresolver.cpp). `rtaudio/CMakeLists.txt` la aggancia da solo quando `RTAUDIO_API_ASIO=ON`. `dep/Makefile` accende quel flag se la variabile `RTAUDIO_ASIO` è definita (`ifdef`, qualunque valore). Default OFF, quindi va riattivata a ogni rebuild pulito di rtaudio.

**Attenzione alle DUE convenzioni opposte di `CROSS_COMPILE`:**
- Parte `dep`: `dep.mk` costruisce il compilatore come `$(CROSS_COMPILE)-gcc` → passare `CROSS_COMPILE=x86_64-w64-mingw32` **SENZA** trattino finale e NON passare CC/CXX espliciti (li deriva lui). Con trattino → doppio trattino `mingw32--gcc` → errore CMake.
- Build principale: come sopra, `CROSS_COMPILE=x86_64-w64-mingw32-` CON trattino + override espliciti dei tool.

Rebuild solo rtaudio + rilink (il relink NON parte da solo cambiando una .a: il target dipende dai .o, va rimosso `libRack.dll`/`Rack.exe`):
```bash
wsl -d Ubuntu -- bash -c "cd /mnt/c/Rack && \
  rm -rf dep/rtaudio/build dep/lib/librtaudio.a && \
  make dep -j4 RTAUDIO_ASIO=1 CROSS_COMPILE=x86_64-w64-mingw32 && \
  rm -f libRack.dll libRack.dll.a Rack.exe && \
  make -j4 CROSS_COMPILE=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ STRIP=x86_64-w64-mingw32-strip AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres"
```
Conferma CMake: `Compiling with support for: asio ds wasapi`. Limiti ASIO: un solo device alla volta (`rtaudio.cpp` subscribe); solo device con driver ASIO installato.

Submodule da inizializzare prima del build:
- AudibleInstruments: `git submodule update --init` (eurorack)
- Befaco: `git submodule update --init --recursive` (Iroi + OwlProgram + nested)
- ESeries, Fundamental: nessun submodule
