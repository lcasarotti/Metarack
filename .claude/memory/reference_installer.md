---
name: reference-installer
description: "Come costruire l'installer .exe per la versione accessibile di VCV Rack"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 8d8d13f7-74e2-4417-be29-96137a3aa852
  modified: 2026-07-24T14:14:07.112Z
---

## Formato e tool

L'installer è un `.exe` NSIS (non .msi). NSIS installato in `C:\Program Files (x86)\NSIS\makensis.exe`.

## File chiave

- `installer.nsi` — script NSIS, brandizzato **MetaRack** (2026-06-25); installa in `MetaRack2` (separato dall'ufficiale `Rack2Free`)
- `make_dist.sh` — script temporaneo da ricreare per assemblare `dist/Rack2Free/`. ⚠️ Se lo scrivi con un tool Windows arriva con CRLF e `bash` fallisce (`mkdir: cannot create directory ''`): prima di eseguirlo fai `wsl -d Ubuntu -- bash -c "sed -i 's/\r$//' /mnt/c/Rack/make_dist.sh && bash /mnt/c/Rack/make_dist.sh"`. Rimuoverlo dopo (è untracked).

## Stato attuale installer.nsi — brand MetaRack (2026-06-25)

- **Versione prodotto disaccoppiata:** `!define METARACK_VERSION "2.0"` (era "1.0", portata a **2.0 il 2026-07-24** su richiesta utente) = versione di MetaRack (mostrata nel titolo della finestra di setup e in `DisplayVersion`), INDIPENDENTE dalla versione base Rack (`RACK_VERSION` = 2.6.6, ora citata solo nelle note di rilascio). Nome file output allineato: `MetaRack-2.0-win.exe`.
- **⚠️ L'installer ora impacchetta ANCHE il VST3 (dal 2026-07-24).** Vedi sezione "Installer combinato standalone + VST3" in fondo.
- `NAME` = "MetaRack 2" (nome prodotto nei collegamenti/DisplayName, usa `RACK_VERSION_MAJOR`), `NAME_FULL` = "MetaRack ${METARACK_VERSION}" = "MetaRack 1.0" (titolo finestra setup)
- `RACK_DIR` = "MetaRack2" → cartella di install
- `DIST_DIR` = "Rack2Free" → cartella sorgente in `dist/` (prodotta da make_dist.sh) — invariata
- `INSTALL_REG` = `Software\MetaRack\MetaRack2`, `UNINSTALL_REG` = `…\Uninstall\MetaRack2`, Publisher "MetaRack", cleanup `Software\MetaRack` → coesiste con Rack ufficiale
- `InstallDir` = `$PROGRAMFILES\MetaRack\${RACK_DIR}`
- Restano VCV (compat): associazione `.vcv`→`VCVRack.Patch` ("VCV Rack patch"), binario `Rack.exe`

## Processo completo per produrre l'installer

### 1. Build binari da WSL
```powershell
wsl -d Ubuntu bash -c "cd /mnt/c/Rack && make -j4 CROSS_COMPILE=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ STRIP=x86_64-w64-mingw32-strip AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres"
```

### 2. Assembla dist/Rack2Free/ da WSL

Percorsi DLL MinGW in WSL Ubuntu:
- `libwinpthread-1.dll` → `/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll`
- `libstdc++-6.dll` → `/usr/lib/gcc/x86_64-w64-mingw32/13-posix/libstdc++-6.dll`
- `libgcc_s_seh-1.dll` → `/usr/lib/gcc/x86_64-w64-mingw32/13-posix/libgcc_s_seh-1.dll`

Script bash (salvare come `make_dist.sh` nella root di Rack):
```bash
#!/usr/bin/env bash
set -e
RACK=/mnt/c/Rack
DIST=/mnt/c/Rack/dist/Rack2Free
STRIP=x86_64-w64-mingw32-strip
rm -rf "$DIST" && mkdir -p "$DIST"
cp "$RACK/libRack.dll" "$RACK/Rack.exe" "$DIST/"
$STRIP -s "$DIST/libRack.dll" "$DIST/Rack.exe"
cp -R "$RACK/res" "$RACK/translations" "$DIST/"
cp "$RACK/cacert.pem" "$RACK/Core.json" "$RACK/template.vcv" "$RACK/LICENSE-GPLv3.txt" "$DIST/"
cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$DIST/"
cp /usr/lib/gcc/x86_64-w64-mingw32/13-posix/libstdc++-6.dll "$DIST/"
cp /usr/lib/gcc/x86_64-w64-mingw32/13-posix/libgcc_s_seh-1.dll "$DIST/"
```

⚠️ **`nvdaControllerClient.dll` (dal 2026-07-14) — la installa `installer.nsi`, NON make_dist.sh.** Serve accanto a `Rack.exe` perché NVDA legga i messaggi della status bar (vedi meccanismo in [[project-accessible-window]]). NON è nel repo di NVDA né nel suo installer: scaricata dal "Controller Client" di NV Access (o `extras/controllerClient/x64/nvdaControllerClient.dll` dal repo NVDA) e tenuta in **`C:\Rack\nvdaControllerClient.dll`** (nome v2, senza suffisso). `installer.nsi` la preleva dalla root del repo con una `File "nvdaControllerClient.dll"` esplicita (fonte unica) protetta da `!if /FileExists` → se manca, makensis dà `!error` con istruzioni. **NON** copiarla anche in make_dist.sh: finirebbe inclusa due volte nell'installer. Il codice runtime prova comunque i nomi `nvdaControllerClient.dll` → `...64.dll` → `...32.dll`. Se manca a runtime: nessun crash, solo NVDA muto sulla status bar (UIA regge Jaws/Narrator).
Eseguire: `wsl -d Ubuntu bash /mnt/c/Rack/make_dist.sh`

### 3. Crea installer con makensis (da PowerShell)
```powershell
$ver = (git describe --tags --abbrev=0 --match "v2.*") -replace '^v',''
& "C:\Program Files (x86)\NSIS\makensis.exe" "/DRACK_VERSION_MAJOR=2" "/DRACK_VERSION=$ver" "/XOutFile dist\MetaRack-$ver-win.exe" installer.nsi
```
NB: con `makensis.exe` da PowerShell usa il prefisso `/` per le opzioni (`/D`, `/X`), non `-`.

⚠️ **Versione pulita per l'utente:** usa `--abbrev=0` (dà la sola tag, es. `2.6.6`), NON `git describe` completo (che dà `2.6.6-45-gae314793`, suffisso commit+hash superfluo). `RACK_VERSION` passato a makensis compare nel **titolo della finestra dell'installer** (`Name "MetaRack ${RACK_VERSION}"`) e in `DisplayVersion`. La versione COMPLETA resta solo dentro il binario (Makefile `-D_RACK_VERSION`, visibile nel log) — utile per debug, non si tocca.

Output: nomina il file con la versione PRODOTTO MetaRack, es. `/XOutFile dist\MetaRack-2.0-win.exe`. Ultimo build OK: `MetaRack-2.0-win.exe` (2026-07-24, titolo installer "MetaRack 2.0", ~30 MB — più grande di prima perché include anche il bundle VST3). NB: `RACK_VERSION` non è più referenziato in installer.nsi (NAME_FULL/DisplayVersion usano METARACK_VERSION) → a makensis basta `/DRACK_VERSION_MAJOR=2`.

## Risultato installazione

- Cartella: `C:\Program Files\MetaRack\MetaRack2\`
- Shortcut desktop/Start: "MetaRack 2" (Finish page "Launch MetaRack 2")
- Registro: `HKLM\Software\MetaRack\MetaRack2`
- Uninstaller incluso

## Installer combinato standalone + VST3 (dal 2026-07-24)

Committato `e4ad7b78` (2026-07-24: installer.nsi VST3+2.0 e vst3.cpp i18n pannellino insieme).

Un UNICO installer installa DUE componenti (richiesta utente):
1. **Standalone** → `C:\Program Files\MetaRack\MetaRack2\` (come prima, da `dist\Rack2Free`).
2. **VST3** → `$COMMONFILES64\VST3\Luca Casarotti\MetaRack.vst3` = `C:\Program Files\Common
   Files\VST3\Luca Casarotti\MetaRack.vst3` (la cartella VST3 di sistema che ogni DAW scandisce).
   Sorgente: `dist\MetaRack.vst3`, bundle autosufficiente prodotto da `make vst3dist` (vedi
   [[project-plugin-adapter]] sezione packaging). Condivide libreria/token con lo standalone via
   `%LOCALAPPDATA%\Rack2`.

Modifiche a `installer.nsi`: in coda alla INSTALL_SECTION (dopo gli shortcut, per non alterare la
working dir degli shortcut standalone) → `RMDir /r` del vecchio bundle + `SetOutPath
"$COMMONFILES64\VST3\Luca Casarotti"` + `File /r "dist\MetaRack.vst3"`. Nella sezione Uninstall →
`RMDir /r` del bundle + `RMDir` (senza /r) della cartella vendor se vuota. Uso `$COMMONFILES64`
(non `$COMMONFILES`) così punta a Program Files\Common Files a prescindere dal bit dell'installer.

⚠️ **Ricostruire ENTRAMBE le dist da HEAD prima di makensis**: `make` (→ libRack+Rack.exe) E `make
vst3dist` (→ bundle), poi `make_dist.sh` per `dist\Rack2Free`. Il bundle VST3 va **strippato a mano**
dopo `make vst3dist` — `make vst3dist` NON strippa, lascia `libRack.dll` a ~104 MB con simboli:
`wsl … x86_64-w64-mingw32-strip -s libRack.dll RackVst3Adapter.dll MetaRack.vst3` dentro
`dist/MetaRack.vst3/Contents/x86_64-win/` → 104 MB → 14 MB, installer 52 MB → 30 MB. (Lo standalone
lo strippa già make_dist.sh.)

## Note

- **Solo moduli Core** nel pacchetto (Audio/MIDI): make_dist.sh NON include Fundamental né altri plugin. Per un pacchetto "completo" servirebbe scaricare `Fundamental-<ver>-win-x64.vcvplugin` (URL api.vcvrack.com) e copiarlo nella dist. Per il test di installazione pulita non è necessario.
- Path DLL runtime MinGW in WSL confermati `13-posix` (2026-06-25): `libstdc++-6.dll`, `libgcc_s_seh-1.dll` in `/usr/lib/gcc/x86_64-w64-mingw32/13-posix/`; `libwinpthread-1.dll` in `/usr/x86_64-w64-mingw32/lib/`.
