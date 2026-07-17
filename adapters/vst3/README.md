# MetaRack — adapter VST3

`Rack.vst3` è un adapter **scritto a mano**: `adapters/vst3.cpp` implementa l'ABI VST3 e
linka `libRack.dll` direttamente, con la stessa build MinGW/WSL di tutto il resto.

Non c'è nessun SDK Steinberg e nessun wrapper: le interfacce VST3 sono dichiarate da
**travesty** (`travesty/`, licenza ISC), che le esprime in **C puro** — ogni vtable è una
struct esplicita di puntatori a funzione. Per questo non dipendiamo dal layout vtable del
compilatore e possiamo usare MinGW invece di MSVC, che è il vincolo che rendeva costoso il
percorso precedente.

Conseguenze pratiche rispetto al vecchio wrapper:

- Il VST3 è **autosufficiente**: non carica `Rack.clap` a runtime, quindi niente
  `CLAP_PATH` da impostare per avviare il DAW.
- Una sola toolchain: `make vst3` da WSL, niente MSVC/CMake/PowerShell e niente download
  del VST3 SDK al primo configure.
- Niente patch fuori repo da riapplicare a mano.

`adapters/vst3.cpp` è solo il guscio ABI: la logica di Rack (init dei singleton di processo,
driver audio "DAW", Context per istanza, finestra accessibile) sta in `adapters/rackhost.cpp`,
condiviso con l'adapter CLAP.

## Build

```
make vst3
```

Output: il bundle `Rack.vst3/`, che contiene:

| File | Cos'è |
|---|---|
| `Contents/x86_64-win/Rack.vst3` | **lo stub** (`adapters/vst3stub.c`): è il modulo che l'host carica |
| `RackVst3Adapter.dll` | l'adapter vero (`vst3.cpp` + `rackhost.cpp`), importa libRack |
| `libRack.dll` | il motore |
| `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` | su Windows libRack **non** è linkata staticamente al runtime MinGW (il `-static-libstdc++` del Makefile è solo per `ARCH_LIN`) |
| `nvdaControllerClient.dll` | caricata a runtime dalla finestra accessibile |

## Perché c'è uno stub

Windows non cerca le dipendenze di una DLL nella cartella della DLL stessa: il search path
parte dall'**EXE host**, che qui è il DAW, non noi. Peggio, un host può caricare i plugin con
la ricerca **ristretta** (`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`: cartella dell'app + System32 +
user dirs), che non include nemmeno la cartella del modulo caricato. Lì `libRack.dll` accanto
è invisibile: il load fallisce con `ERROR_MOD_NOT_FOUND` e **il plugin sparisce senza un
errore comprensibile** — né tra i disponibili né tra i falliti. È esattamente ciò che
facevano Reaper e Ableton.

Lo stub non ha dipendenze oltre `kernel32` e `msvcrt` (entrambe in System32, sempre
risolvibili), quindi si carica con qualunque politica. Poi carica `RackVst3Adapter.dll` per
**percorso assoluto** con `LOAD_WITH_ALTERED_SEARCH_PATH`, che per quella catena di load mette
la nostra cartella in testa: da lì libRack e il runtime si risolvono sempre. Inoltra i tre
simboli del modulo e basta: la factory è ABI C, quindi ogni chiamata successiva entra dritta
nell'adapter.

Questo è anche il motivo per cui non possiamo semplicemente linkare tutto in una DLL sola
(come fa Cardinal): i `.vcvplugin` dell'utente importano `libRack.dll` per nome, quindi il
motore deve restare una DLL condivisa. Lo stub la carica in memoria, e da lì i moduli la
risolvono dalla lista dei moduli già caricati.

Il log è `vst3-log.txt`, separato da quelli dello standalone e del CLAP.

## Dove va il bundle (importante)

`asset::systemDir` viene trovato **risalendo dalla DLL in cerca di `res/`** (fino a 4 livelli).
Dal bundle in `C:\Rack\Rack.vst3\Contents\x86_64-win` si risale a `C:\Rack`, dove stanno
`res/`, `Core.json` e `plugins-win-x64`.

Ne segue che **il bundle va lasciato in `C:\Rack`** e il DAW va puntato lì (Reaper:
Preferences → Plug-ins → VST → aggiungi `C:\Rack` ai percorsi VST3; Ableton: cartella VST3
personalizzata). È lo stesso schema con cui funziona `Rack.clap`.

Copiando invece il bundle in `%COMMONPROGRAMFILES%\VST3` il plugin **si carica ma resta un
guscio vuoto**: senza `res/` accanto non trova nemmeno il plugin Core, quindi niente modulo
Audio e nessun suono (nel log: `Modello Core/AudioInterface2 non trovato`). Un bundle
davvero installabile ovunque dovrà portarsi dentro `res/` + `Core.json` + i plugin di
sistema e puntare `userDir` ad `AppData/Local/Rack2`: è lavoro di packaging ancora da fare.

## Test senza DAW

```
make vst3test && ./RackVst3Test.exe
```

Mini-host da console (`adapters/vst3test.cpp`, gemello di `claptest`): carica il bundle,
percorre il ciclo di vita VST3, fa passare un seno a 440 Hz e verifica l'RMS in uscita
(~0.35), poi esercita edit controller e plug view. Con un ABI scritto a mano è l'unico modo
di distinguere "il DAW non lo vede" da "l'ABI è sbagliato".

L'harness è **linkato staticamente di proposito** e accetta il percorso di un bundle:

```
RackVst3Test.exe "C:\percorso\Rack.vst3"
```

Così può girare da una cartella qualunque e verificare **onestamente** se è il *bundle* a
essere autosufficiente. Eseguendolo da `C:\Rack` (dove stanno `libRack.dll` e il runtime) il
test passerebbe anche con un bundle incompleto, perché le DLL verrebbero risolte dalla
cartella dell'eseguibile.

Per lo stesso motivo carica il modulo con **`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`**, la politica
più restrittiva che un host reale possa usare — non col permissivo
`LOAD_WITH_ALTERED_SEARCH_PATH`. Il test deve emulare l'host *peggiore*: la versione
permissiva passava allegramente mentre né Reaper né Ableton caricavano il plugin.

Il test imposta `METARACK_TEST_PASSTHROUGH=1`: su Windows il rack parte vuoto (il modulo
Audio c'è ma non è cablato — è l'utente a instradare i moduli dalla finestra accessibile),
quindi senza quella variabile l'uscita sarebbe silenziosa per costruzione. La variabile fa
aggiungere a `rackhost` i cavi di loopback DAW in → DAW out. Nessun DAW la imposta mai.

## GUI

VST3 non ha il concetto di finestra "floating" che il CLAP ci dà (`is_floating=true`):
l'unica GUI prevista è `IPlugView`, che l'host **incorpora** nella propria finestra editor.
MetaRack però non ha nulla da incorporare — la sua UI è la finestra accessibile Win32
top-level, l'unica che NVDA può leggere.

Il ponte: la finestra accessibile compare all'istanziazione (auto-show in
`IComponent::initialize`); quando il DAW apre l'editor, `attached()` mette lì un pannellino
segnaposto 320×80 e riporta la finestra MetaRack in primo piano, `removed()` la nasconde.

## Deploy nel DAW

Lascia il bundle in `C:\Rack` e aggiungi `C:\Rack` ai percorsi VST3 del DAW (vedi sopra
*Dove va il bundle*). Non serve nessuna variabile d'ambiente.

## Percorso legacy (in rimozione)

`CMakeLists.txt`, `build.ps1`, `empty.cpp` e `build/` appartengono al vecchio VST3 costruito
con [free-audio/clap-wrapper](https://github.com/free-audio/clap-wrapper), che caricava
`Rack.clap` a runtime. Sono ancora qui solo finché l'adapter nuovo non è validato in un DAW
reale; dopo vanno rimossi insieme a `dep/clap-wrapper`. Il target `make vst3` **non** li usa
più.
