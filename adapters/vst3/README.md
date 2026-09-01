# MetaRack — adapter VST3

`MetaRack.vst3` è un adapter **scritto a mano**: `adapters/vst3.cpp` implementa l'ABI VST3 e
linka direttamente `libRack`, con la stessa build del resto del progetto.

Non c'è nessun SDK Steinberg e nessun wrapper: le interfacce VST3 sono dichiarate da
**travesty** (`travesty/`, licenza ISC), che le esprime in **C puro** — ogni vtable è una
struct esplicita di puntatori a funzione. Per questo non dipendiamo dal layout vtable del
compilatore e possiamo usare la nostra toolchain (MinGW su Windows, clang su macOS) invece di
MSVC o dell'SDK Steinberg.

`adapters/vst3.cpp` è solo il guscio ABI: la logica di Rack — init dei singleton di processo,
driver audio e MIDI "DAW", Context per istanza, finestra accessibile — sta in
`adapters/rackhost.cpp`, che è scritto per essere indipendente dal formato di plugin.

## Build

```
make vst3       # bundle in-tree, per lo sviluppo
make vst3dist   # bundle rilocabile in dist/, quello da installare
make vst3test   # mini-host da console, per provarlo senza DAW
```

Il log è `vst3-log.txt` dentro la cartella utente di Rack, separato da quello dello standalone.

## Il bundle

| Piattaforma | Layout |
|---|---|
| Windows | `Contents/x86_64-win/MetaRack.vst3` (lo **stub**, vedi sotto) + `RackVst3Adapter.dll` + `libRack.dll` + runtime MinGW + `nvdaControllerClient.dll` |
| macOS | `Contents/MacOS/MetaRack` (il modulo) + `libRack.dylib`, più `Info.plist` e `PkgInfo` |

`make vst3dist` aggiunge `Contents/Resources` con `res/`, `translations/`, `Core.json`,
`template.vcv` e `cacert.pem`. È ciò che rende il bundle **rilocabile**: a runtime
`findPackagedResources()` li trova lì dentro e punta `asset::systemDir` nel bundle, mentre
`userDir` resta quella per-utente condivisa con lo standalone — così i plugin scaricati e le
patch sono gli stessi. Senza `Contents/Resources` (build in-tree) si risale invece dal modulo
in cerca di `res/`, che è il comportamento comodo durante lo sviluppo.

Su macOS il bundle va in `~/Library/Audio/Plug-Ins/VST3/`.

## Perché su Windows c'è uno stub

Windows non cerca le dipendenze di una DLL nella cartella della DLL stessa: il search path
parte dall'**EXE host**, che qui è il DAW, non noi. Peggio, un host può caricare i plugin con
la ricerca **ristretta** (`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`: cartella dell'app + System32 +
user dirs), che non include nemmeno la cartella del modulo caricato. Lì `libRack.dll` accanto
è invisibile: il load fallisce con `ERROR_MOD_NOT_FOUND` e **il plugin sparisce senza un
errore comprensibile**. È esattamente ciò che facevano Reaper e Ableton.

Lo stub non ha dipendenze oltre `kernel32` e `msvcrt` (entrambe in System32, sempre
risolvibili), quindi si carica con qualunque politica. Poi carica `RackVst3Adapter.dll` per
**percorso assoluto** con `LOAD_WITH_ALTERED_SEARCH_PATH`, che per quella catena di load mette
la nostra cartella in testa. Non possiamo semplicemente linkare tutto in una DLL sola: i
`.vcvplugin` dell'utente importano `libRack.dll` per nome, quindi il motore deve restare una
DLL condivisa.

## Le tre trappole di macOS

Niente stub qui — dyld risolve per `@loader_path`, cioè relativamente al binario — ma tre
cose vanno fatte apposta, e sono tutte nella regola `vst3` del Makefile:

1. **L'install name di libRack.dylib.** I plugin di terze parti sono compilati dalla farm VCV
   dentro `/tmp/Rack2` e dichiarano quella dipendenza **assoluta**. Diamo alla copia nel
   bundle proprio quell'install name: dyld trova un'immagine già caricata con quel nome e la
   riusa, senza toccare il filesystem. Senza questo rimedio **nessun** plugin dell'utente si
   carica e il rack resta al solo Core.
2. **La firma.** Su Apple Silicon un binario non firmato non viene caricato, e
   `install_name_tool` invalida la firma che il linker aveva applicato: si rifirma ad-hoc.
3. **Il `.dSYM`.** `dsymutil` lo scrive dentro il bundle; va spostato fuori, o finisce
   sigillato dalla firma e copiato in distribuzione.

In più, dentro una DAW `glfwInit()` ha due effetti **process-wide** inaccettabili: si prende
il delegate di `NSApp` (che è quello dell'host) e cambia la working directory del processo.
`adapters/rackhost_mac.mm` salva e ripristina entrambi attorno all'init.

## Test senza DAW

```
make vst3test && ./RackVst3Test            # bundle in-tree
./RackVst3Test dist/MetaRack.vst3          # oppure un bundle installato
```

Mini-host da console (`adapters/vst3test.cpp`): carica il bundle, percorre il ciclo di vita
VST3, fa passare un seno a 440 Hz e verifica l'RMS in uscita (~0.35), spinge note e pitch-bend
per provare il ponte MIDI, poi esercita edit controller e plug view. Con un ABI scritto a mano
è l'unico modo di distinguere "il DAW non lo vede" da "l'ABI è sbagliato".

L'harness **non si linka a libRack** ed accetta il percorso di un bundle, così può girare da
una cartella qualunque e verificare *onestamente* se è il bundle a essere autosufficiente:
eseguito accanto alla libreria, il test passerebbe anche con un bundle incompleto. Per lo
stesso motivo su Windows carica il modulo con `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, la politica
più restrittiva che un host reale possa usare: la versione permissiva passava allegramente
mentre né Reaper né Ableton caricavano il plugin.

Il test imposta `METARACK_TEST_PASSTHROUGH=1`: il rack parte vuoto (il modulo Audio c'è ma non
è cablato — è l'utente a instradare i moduli dalla finestra accessibile), quindi senza quella
variabile l'uscita sarebbe silenziosa per costruzione. Nessun DAW la imposta mai.

## GUI

VST3 prevede una sola GUI: `IPlugView`, che l'host **incorpora** nella propria finestra
editor. MetaRack però non ha nulla da incorporare — la sua UI è la finestra accessibile
top-level, l'unica che NVDA o VoiceOver possono leggere.

Il ponte: la finestra accessibile compare all'istanziazione (auto-show in
`IComponent::initialize`); quando il DAW apre l'editor, `attached()` mette lì un pannellino
segnaposto che dice dov'è la UI vera e rimbalza il focus sulla finestra accessibile, e
`removed()` la nasconde. Il segnaposto è una finestra figlia Win32 su Windows e una `NSView`
su macOS (`adapters/vst3_mac.mm`); il resto dell'ABI è condiviso.
