---
name: reference-installer
description: "Come costruire l'installer .exe per la versione accessibile di VCV Rack (Windows)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 8d8d13f7-74e2-4417-be29-96137a3aa852
---

## Formato e tool

L'installer è un `.exe` NSIS (non .msi). NSIS installato in `C:\Program Files (x86)\NSIS\makensis.exe`.

## File chiave

- `installer.nsi` — script NSIS, modificato per installare in `Rack2Accessible` (separato dall'ufficiale `Rack2Free`)
- `make_dist.sh` — script temporaneo da ricreare per assemblare `dist/Rack2Free/`

## Modifiche a installer.nsi (rispetto all'originale)

- `NAME` = "VCV Rack 2 Free Accessible" (con suffisso Accessible)
- `RACK_DIR` = "Rack2Accessible" → cartella di install in Program Files
- `DIST_DIR` = "Rack2Free" → cartella sorgente in `dist/` (prodotta dal build)
- `INSTALL_REG` / `UNINSTALL_REG` → chiavi di registro separate → coesiste con Rack ufficiale

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

### 3. Crea installer con makensis (da PowerShell)
```powershell
$ver = (git describe --tags --match "v2.*") -replace '^v',''
& "C:\Program Files (x86)\NSIS\makensis.exe" "-DRACK_VERSION_MAJOR=2" "-DRACK_VERSION=$ver" "-XOutFile dist\RackFree-$ver-win.exe" installer.nsi
```

## Risultato installazione

- Cartella: `C:\Program Files\VCV\Rack2Accessible\`
- Shortcut desktop/Start: "VCV Rack 2 Free Accessible"
- Registro: `HKLM\Software\VCV\Rack2Accessible`
