---
name: project-plugin-adapter
description: "Adapter plugin CLAP + VST3 per libRack — core condiviso rackhost, VST3 scritto a mano con travesty; stato e limiti"
metadata: 
  node_type: memory
  type: project
  originSessionId: a355851a-354b-4087-b053-3da67053290d
  modified: 2026-07-24T14:14:02.210Z
---

Obiettivo: versioni **CLAP e VST3** di VCV Rack (incluso il layer accessibile MetaRack) dai
sorgenti open-source, che di base costruiscono solo `libRack` + adapter standalone. Gli adapter
plugin sono closed-source in Rack Pro → scritti da zero attorno all'API pubblica di `libRack`.

## Architettura (dal 2026-07-17)

`adapters/rackhost.{hpp,cpp}` è il **core condiviso**: init/deinit dei singleton di processo
(refcounted, `retainProcess(logName)`), driver audio finto `DawDriver`/`DawDevice`
(`DAW_DRIVER_ID` 0x44415721), `createInstance/destroyInstance`, `activate`, `processPlanar`,
e i comandi `guiShow/guiHide/guiSetTransient/guiGetSize` sulla finestra accessibile.
`adapters/clap.cpp` e `adapters/vst3.cpp` sono **solo gusci ABI** sopra di esso.

Ponte audio: `processPlanar` → `DawDevice::processBuffer` → il Port del modulo Core Audio (master)
→ `engine->stepBlock()`. L'adapter non chiama mai `stepBlock` direttamente. `APP` è thread-local:
`rackhost::useContext()` all'inizio di ogni callback.

## Multicanale — 8 bus stereo (dal 2026-07-18)

L'audio DAW↔plugin è **16 canali** (non più stereo). Modulo ponte = **`Core/AudioInterface16`**
(`Audio<16,16>`; confermato in `src/core/Audio.cpp:54-56` che il modulo riversa `min(getNumInputs(),
NUM_AUDIO_OUTPUTS)` canali → con device 16/16 passano tutti e 16). Numero centralizzato in
`rackhost::kNumChannels = 16` (rackhost.hpp): lo leggono `DawDevice` (num in/out), i buffer
interleaved di `activate` (`maxFrames * kNumChannels`), il loop di `processPlanar` (stride
kNumChannels, generalizzato da 2 a N con lookup-puntatore fuori dal loop dei frame) e i cavi di
passthrough del test. **I tre punti non possono più andare fuori sincrono.**

Verso il DAW i 16 canali sono esposti come **8 BUS STEREO separati** (non un unico bus largo — scelta
dell'utente: strumento multi-uscita, ogni coppia a una traccia diversa).

⚠️ **I bus vanno dichiarati DUE VOLTE, in due file indipendenti** — è l'errore che ho fatto: il VST3
funzionante è quello **hand-written con travesty** (`vst3.cpp`, `make vst3`), NON il wrapper clap.
`clap.cpp` e `vst3.cpp` hanno OGNUNO la propria dichiarazione dei bus/porte; toccarne uno solo non
basta, e `make clap` non ricompila il VST3 (che ha la sua `RackVst3Adapter.dll`). La costante
condivisa `rackhost::kNumChannels` tiene almeno allineato il numero totale di canali.

- **`clap.cpp`**: `audioPortsCount` → `kStereoBuses = kNumChannels/2 = 8`; `audioPortsGet` → id=index,
  nome 1-based `"Out 1/2".."15/16"` (input `"In …"`), bus 0 con `CLAP_AUDIO_PORT_IS_MAIN`, gli altri
  senza flag, tutti `CLAP_PORT_STEREO`. `pluginProcess` srotola gli 8 bus planari in un array piatto
  di 16 puntatori-per-canale prima di `processPlanar`.
- **`vst3.cpp`** (travesty): `RackComponent::getBusCount` → 8 per `V3_AUDIO`; `getBusInfo` → per busIdx
  0..7 channel_count=2, nome `"Out N/N+1"`, `bus_type` = `V3_MAIN` (bus 0) / `V3_AUX` (altri), tutti
  `V3_DEFAULT_ACTIVE`. `RackAudioProcessor::setBusArrangements` accetta ESATTAMENTE 8/8 bus stereo;
  `getBusArrangement` torna stereo per idx 0..7; `process` srotola `data->inputs[b]`/`outputs[b]`
  (8 bus × 2 canali) nell'array piatto di 16 come nel CLAP. Serve `#include <cstdio>` per snprintf.

Entrambi gli array piatti nascono azzerati con `{}` → un bus non presentato dall'host = silenzio.
`processPlanar` (rackhost.cpp) è generalizzato a `kNumChannels` con lookup-puntatore fuori dal loop
dei frame. Test aggiornati (`claptest`/`vst3test` ora enumerano e presentano 8 bus): entrambi
enumerano 8 IN + 8 OUT, pass-through RMS ~0.35, exit 0. **Ricompilare con `make clap` E `make vst3`**
(DAW chiuso: locka le DLL del bundle). **Instradamento multicanale in Reaper CONFERMATO dal vivo
dall'utente il 2026-07-18** ("Audio multicanale in reaper ok").

## VST3: scritto a mano, NIENTE clap-wrapper (dal 2026-07-17)

Il wrapper `free-audio/clap-wrapper` è stato **abbandonato**: costava tre artefatti accoppiati a
runtime (`Rack.vst3` → `Rack.clap` → `libRack.dll`), `CLAP_PATH` obbligatorio all'avvio del DAW,
MSVC + CMake + download del VST3 SDK, e una patch out-of-tree a `fsutil.cpp` che si perdeva
(`dep/` è gitignored).

Ora `adapters/vst3.cpp` implementa l'ABI VST3 con **travesty** (`adapters/vst3/travesty/`, ISC,
vendorizzato da DPF/Cardinal `~/cardinal/cardinal-26.01/dpf/distrho/src/travesty/`): interfacce in
C puro, vtable = struct esplicite di puntatori a funzione → nessuna dipendenza dal layout vtable
del compilatore → **si costruisce con lo stesso MinGW di libRack**. Riferimento d'implementazione:
`dpf/distrho/src/DistrhoPluginVST3.cpp`.

**L'idioma ABI (la parte meno ovvia):** l'oggetto eredita per valore la struct-vtable
(`struct RackComponent : v3_component_cpp`), quindi i suoi primi byte SONO i puntatori a funzione.
All'host si passa un puntatore a un PUNTATORE all'oggetto (`T**`) → ogni metodo fa
`*static_cast<T**>(self)`. I sotto-oggetti stanno come MEMBRI PUNTATORE: l'indirizzo del membro è
il `T**` da consegnare. Attenzione: metodi statici chiamati `ref`/`unref` **nascondono** i membri
dato omonimi di `v3_funknown` (errore "assignment of read-only location") → chiamarli `refFn`/`unrefFn`.

Scelta: **single component effect** — una sola classe nella factory, lo stesso oggetto espone
IComponent + IAudioProcessor + IEditController; `get_controller_class_id` → `V3_NOT_IMPLEMENTED`.
Niente IConnectionPoint, e il controller raggiunge direttamente la finestra accessibile della SUA
istanza. TUID classe: `V3_ID(0x4D657461, 0x5261636B, 0x436F6D70, 0x6F6E656E)` — **non cambiarlo**.

**GUI**: VST3 non ha finestre floating (a differenza del CLAP, `is_floating=true`). `IPlugView`-ponte:
auto-show della finestra accessibile in `IComponent::initialize`; `attached()` crea un segnaposto
`WS_CHILD` 320×80 nell'editor del DAW + `guiSetTransient(GetAncestor(parent, GA_ROOT))` + riporta in
primo piano; `removed()` nasconde.

**Navigazione da tastiera DAW↔MetaRack (risolta 2026-07-17, confermata dall'utente).** La UI vera
è una finestra top-level SEPARATA, non parte della gerarchia dell'host → serviva agganciarla a mano
in entrambi i sensi:
- **Entrare in MetaRack → Alt+Tab.** `guiSetTransient` la rende *posseduta* dall'host (per z-order),
  e una finestra posseduta **sparisce da Alt+Tab/taskbar**. Fix: crearla con **`WS_EX_APPWINDOW`**
  (in `AccessibleWindow::create`), che forza una voce Alt+Tab propria pur restando posseduta.
- **Tornare alla DAW → F6.** L'host non vede mai un F6 premuto dentro la nostra finestra. In
  `ChildSubclassProc` intercetto `VK_F6` e porto in primo piano l'`hostWindow` memorizzato (nuovo
  campo su AccessibleWindow, salvato da `guiSetTransient`) — imita la convenzione "F6 → arrange" di
  Reaper.
- ⚠️ **`SetForegroundWindow` da solo NON basta**: Windows lo ignora se il thread chiamante non
  possiede il foreground corrente → la finestra veniva avanti solo *visivamente* e il focus (quindi
  lo screen reader) restava sull'host. Serve il "prestito" della coda input:
  `AttachThreadInput(myThread, fgThread, TRUE)` intorno a `SetForegroundWindow`+`SetFocus`. Helper
  `forceForeground()` in AccessibleWindow.cpp + stessa logica in `rackhost::guiShow`.
- Il pannellino segnaposto in Reaper **non riceve MAI il focus Win32 reale** (il focus resta nella
  chrome dell'host; lo screen reader lo raggiunge solo con la navigazione a oggetti) → un `Invio`/
  `WM_SETFOCUS`/`WM_KEYDOWN` sul pannellino non arriva a noi e non si può intercettare. Per questo
  la via d'ingresso affidabile è Alt+Tab, non il pannellino.

**Bundle + STUB (fondamentale)**: `make vst3` → `Rack.vst3/Contents/x86_64-win/` contiene:
`Rack.vst3` = **stub** (`adapters/vst3stub.c`), `RackVst3Adapter.dll` = adapter vero,
`libRack.dll`, il runtime MinGW (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`
— su Windows libRack NON è linkata staticamente ad essi: il `-static-libstdc++` è solo ARCH_LIN;
il Makefile li chiede al compilatore con `$(CXX) -print-file-name=...`), `nvdaControllerClient.dll`.

⚠️ **Perché lo stub**: un host può caricare i plugin con ricerca DLL RISTRETTA
(`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` = app dir + System32 + user dirs), che **non include la
cartella del modulo caricato** → `libRack.dll` accanto è invisibile → `ERROR_MOD_NOT_FOUND` → **il
plugin sparisce senza errore comprensibile, né tra i disponibili né tra i falliti**. È ciò che
facevano *sia Reaper sia Ableton*. Lo stub non ha dipendenze oltre kernel32/msvcrt (System32,
sempre risolvibili), quindi carica sempre; poi carica l'adapter per **percorso assoluto** con
`LOAD_WITH_ALTERED_SEARCH_PATH` e inoltra i 3 export (la factory è ABI C: il resto va dritto).
Non si può invece linkare tutto in una DLL sola come fa Cardinal: i `.vcvplugin` importano
`libRack.dll` per nome, quindi il motore deve restare DLL condivisa (lo stub la porta in memoria
e i moduli la risolvono dalla lista dei moduli caricati). Log: `vst3-log.txt`.

⚠️ **Il bundle porta una PROPRIA copia di `libRack.dll`** (la copia `make vst3` riga ~210 del
Makefile). Quindi una modifica a `libRack` **non arriva al plugin con `make` da solo**: il DAW
carica la `libRack.dll` DENTRO il bundle (sta accanto a `RackVst3Adapter.dll` → vince nel search
order), non quella a `C:\Rack`. Bisogna rifare **`make vst3`**. E il DAW va **chiuso prima**: tiene
mappate (lockate) `RackVst3Adapter.dll` e `libRack.dll` → il link fallisce con *Permission denied*,
e comunque un DAW già avviato riusa la DLL vecchia già in memoria. Verifica veloce: `Get-FileHash`
della `libRack.dll` nel bundle == quella a `C:\Rack`. Lezione pagata 2026-07-18 (persistenza token).

**Diagnosi di "il DAW non lo vede"** — tecniche che hanno funzionato: la cache
`%APPDATA%\REAPER\reaper-vstplugins64.ini` mostra `Nome.vst3=<FILETIME>` **senza** class-id/nome
quando Reaper ha provato e non ha ottenuto classi (voce buona: `...=<ts>,<id>{<cid>,Nome (Vendor)`).
Decodificando quel FILETIME (byte order invertito) e confrontandolo col mtime del modulo si prova
che l'host ha trovato il file giusto. Se poi `vst3-log.txt` NON è stato riscritto all'ora della
scansione, `InitDll` non è mai partito → il fallimento è nel LOAD, non nel nostro codice.

⚠️ **Il bundle va lasciato in `C:\Rack`**, puntandoci i percorsi VST3 del DAW (come per il CLAP).
`asset::systemDir` risale dal modulo cercando `res/` (max 4 livelli) → dal bundle in
`C:\Rack\Rack.vst3\Contents\x86_64-win` trova `C:\Rack`. Copiato in `%COMMONPROGRAMFILES%\VST3` il
plugin **si carica ma è un guscio vuoto e muto**: niente `res/` → niente Core → niente modulo Audio
(`Modello Core/AudioInterface16 non trovato` nel log). Packaging vero (res/ + Core.json dentro il
bundle, userDir → AppData/Local/Rack2) ancora da fare.

⚠️ `nvdaControllerClient.dll` è caricata a runtime con nome nudo → cercherebbe nella cartella del
**DAW**. `AccessibleWindow.cpp::loadDllBesideLibRack()` aggiunge il fallback al percorso esplicito
accanto al proprio modulo, altrimenti NVDA resta muta nel plugin pur caricandosi tutto.

## MIDI DAW → plugin (dal 2026-07-18, VST3)

Gemello MIDI del ponte audio. **Solo INPUT** (DAW → Rack); direzione opposta non fatta.

- **Core** (`rackhost.cpp`): driver MIDI finto `DawMidiDriver` + `DawMidiDevice : midi::InputDevice`,
  un solo device (id 0, nome **"DAW"**), `DAW_MIDI_DRIVER_ID = 0x444D4921`. Registrato in
  `processInit()` con `midi::addDriver` (che ne prende possesso → `midi::destroy` lo libera; azzero
  il puntatore lì). `pushMidiMessage(inst, bytes, len, sampleOffset)` costruisce un `midi::Message`,
  lo timestampa con `engine->getFrame() + sampleOffset` e chiama `device.onMessage()`.
- **⚠️ Timing**: `pushMidiMessage` va chiamato **PRIMA** di `processPlanar`. Lì `getFrame()` è il
  frame d'inizio blocco (stepBlock non ha ancora gito); il modulo Core MIDI fa `tryPop(&msg,
  args.frame)` durante lo stepBlock, con args.frame in `[start, start+n)` → il messaggio esce al
  campione esatto `start+sampleOffset` (sample-accurate, stesso blocco). Il push (mutex della
  InputQueue) è sul thread audio ma NON conteso: stessa thread fa push e tryPop in sequenza.
- **VST3** (`vst3.cpp`): `getBusCount` → 1 bus `V3_EVENT`/`V3_INPUT`; `getBusInfo` per quel bus
  channel_count=16, nome "MIDI In", `V3_MAIN`+`V3_DEFAULT_ACTIVE`. In `process()`, PRIMA di
  processPlanar, itera `data->input_events` (idioma travesty `v3_cpp_obj(elist)->get_event_count/
  get_event(elist)`) e `pushVst3Event` traduce ogni `v3_event` → byte MIDI grezzi.
- **Scelta utente (2026-07-18): NESSUN modulo auto-inserito.** Registro solo il driver "DAW";
  l'utente aggiunge il modulo Core MIDI che vuole (MIDI-CV, MIDI-Gate, MIDI-CC…) e ne seleziona il
  device "DAW" dal chooser accessibile. Simmetria col fatto che l'audio SÌ auto-inserisce Audio-16
  (obbligatorio per non essere muti), il MIDI no (opzionale).
### Note vs controller: DUE canali diversi in VST3

- **Note** (on/off, poly-pressure, SysEx): arrivano come **eventi** in `data->input_events`.
  `pushVst3Event` traduce ogni `v3_event` → byte MIDI.
- **⚠️ CC / pitch-bend / channel-pressure (aftertouch di canale): NON sono eventi.** VST3 li
  instrada come **parameter change** in `data->input_params`, dopo che il plugin ha mappato
  `(canale, controller) → id parametro` via **`IMidiMapping`**. È il motivo per cui "il pitch-bend
  non funzionava": senza IMidiMapping + parametri l'host non ha dove mandarli. **Fatto il
  2026-07-18** replicando l'idioma DPF (`DistrhoPluginVST3.cpp`, su disco in
  `~/cardinal/cardinal-26.01/dpf/distrho/src/`):
  - **Parametri**: 130 controller × 16 canali = **2080 param nascosti** (`kMidiCCParamCount`).
    `id = canale*130 + cc`; cc 0..127 = CC, **128 = channel pressure, 129 = pitch-bend**. Flag
    `V3_PARAM_CAN_AUTOMATE | V3_PARAM_IS_HIDDEN` (fuori dalla lista utente), step_count 127,
    default 0.5 per il pitch-bend (centro) altrimenti 0. Sono gli UNICI parametri esposti
    (`getParameterCount` passa da 0 → 2080; un rack non ha manopole automatizzabili).
  - **`IMidiMapping`** = oggetto tearoff `RackMidiMapping : v3_midi_mapping_cpp` posseduto dal
    controller, esposto dalla `queryInterface` del controller (l'host lo interroga LÌ, non sul
    component). `get_midi_controller_assignment(bus, ch, cc, id*)` → `id = ch*130+cc`.
  - **`process()`**: dopo gli eventi, itera `data->input_params`; per ogni coda con id < 2080 e
    per ogni punto (offset, valore normalizzato) chiama `pushVst3ParamChange` → byte MIDI grezzi
    (CC = `Bn cc val7`; pressure = `Dn val7` a 2 byte; pitch-bend = `En LSB MSB`, `val14 =
    clamp(norm*16384, 0, 16383)`). La InputQueue del modulo riordina per frame → non serve fondere
    con le note. NB clamp a **16383** non 16384: `16384>>7 = 128` darebbe un byte MIDI con bit alto.
  - I valori NON si cachano nel controller: l'audio li legge da input_params, non da
    get/setParameterNormalised (che rispondono col valore a riposo / V3_OK).
  - ⚠️ Tutto questo è nel blocco `#if ARCH_WIN` del controller (esiste solo lì per la GUI) → **CC/PB
    su non-Windows non funzionano** finché il controller non è reso cross-platform. Irrilevante ora
    (target Windows/Reaper).
- **Test**: `vst3test` — note via `TestEventList : v3_event_list_cpp`; pitch-bend via
  `TestParamChanges`/`TestParamQueue` (finti `IParameterChanges`/`IParamValueQueue`) sull'id 129,
  dopo aver verificato `get_midi_controller_assignment(0,0,129)==129` e `get_parameter_count()==2080`.
  Verifica via il simbolo di debug **esportato** `MetarackDebugMidiCount` (contatore atomico in
  rackhost, ri-esportato dall'adapter; vive nell'ADAPTER non nello stub → handle con
  `GetModuleHandleW("RackVst3Adapter.dll")` dopo InitDll). Gate verde 2026-07-18, exit 0.
- **NON ancora confermato dal vivo**: l'ultimo hop (device "DAW" → modulo Core MIDI sottoscritto →
  CV/PW output) è codice Rack esistente, ma va provato in un DAW con un MIDI-CV cablato al device
  "DAW". Il pitch-bend esce dal `PW_OUTPUT` di MIDI-CV, il mod-wheel (CC1) dal `MOD_OUTPUT`.

## i18n del pannellino segnaposto VST3 (2026-07-24)

⚠️ **Il pannellino segnaposto ha DUE fonti di testo, entrambe da localizzare** — insidia pagata:
1. Il **nome-finestra MSAA** passato a `CreateWindowExW` in `RackPlugView::attached` (`vst3.cpp`
   ~riga 447): è ciò che NVDA **annuncia** quando focus/navigazione oggetti tocca il pannellino.
2. Il **testo dipinto** con `DrawTextW` nel `WM_PAINT` del `placeholderWndProc`: sono solo pixel,
   ma **NVDA li legge lo stesso** tramite il display model (hook GDI). Non basta ignorarlo perché
   "tanto è grafica".

Localizzarne una sola → l'utente non vedente sente **le due lingue insieme** (es. nome-finestra IT
+ testo dipinto EN).

**Scelta finale (2026-07-24): il messaggio vive SOLO nel nome-finestra MSAA**; il `WM_PAINT`
dipinge solo il riquadro grigio (niente `DrawTextW`), altrimenti NVDA lo rileggerebbe via display
model = duplicato anche a lingua uguale. Il nome-finestra è localizzato con l'helper `T(en, it)`
locale in `vst3.cpp` (gemello del `T()` di AccessibleWindow, che è `static` lì → non riusabile),
che legge `rack::settings::language` (serve `#include <settings.hpp>`). Il nome-finestra porta anche
l'hint "F6 riporta alla DAW". Committato `e4ad7b78` (2026-07-24, insieme all'installer 2.0 + VST3).
Vedi [[project-i18n-accessible]] e [[reference-installer]].

## Test automatici

`make claptest && ./RackClapTest.exe` e `make vst3test && ./RackVst3Test.exe` — mini-host da console.
Entrambi: ciclo di vita completo + seno 440Hz → RMS uscita ~0.35 → `PASS-THROUGH OK`, exit 0.

⚠️ `RackVst3Test.exe` è linkato **staticamente**, accetta il percorso di un bundle
(`RackVst3Test.exe "C:\...\Rack.vst3"`) e carica con **`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`** (la
politica più restrittiva di un host reale), non col permissivo `LOAD_WITH_ALTERED_SEARCH_PATH`.
Serve a testare ONESTAMENTE l'autosufficienza del bundle. Lezione pagata due volte: eseguito da
`C:\Rack` risolveva le DLL dalla cartella dell'eseguibile, e col flag permissivo caricava
comunque → il gate era verde mentre NESSUN DAW vedeva il plugin. **Un test di caricamento deve
emulare l'host peggiore, da una cartella neutra.**

**Hook `METARACK_TEST_PASSTHROUGH=1`** (letta con `GetEnvironmentVariableA`, NON `getenv`: la CRT ha
una copia privata dell'ambiente che `SetEnvironmentVariable` non aggiorna): su Windows il rack parte
vuoto (il modulo Audio non è cablato — è l'utente a instradare dalla finestra accessibile), quindi
senza questa variabile l'uscita è silenziosa **per costruzione** e il test non direbbe nulla. Nessun
DAW la imposta.

⚠️ Su Windows i cavi vanno creati via `CableWidget` + `updateCable()` + `rack->addCable()`, mai
fabbricando a mano `engine::Cable`: quello salta la registrazione dei plug → assert in
`Engine::removeModule` e **crash allo shutdown**. Stessa lezione in `AccessibleWindow.cpp:3080`.
Il ramo headless (non-Windows) non ha widget, lì i cavi bare-engine vanno bene.

## Stato

- CLAP: **validato dal vivo in DAW** dall'utente (2026-07-14).
- VST3 hand-written (travesty + stub): **Reaper LO ENUMERA**, confermato dall'utente 2026-07-17,
  col bundle in `C:\Rack` e `C:\Rack` nei path VST3. Prima non lo vedeva né Reaper né Ableton:
  è stato lo stub a sbloccarlo. Gate automatici verdi (carica sotto ricerca ristretta da cartella
  neutra, enumera, audio RMS ~0.35, view ok, exit 0); CLAP non regredito.
- **Finestra MetaRack letta da NVDA dentro Reaper + navigazione DAW↔MetaRack: confermate dal vivo
  dall'utente il 2026-07-17** ("funziona tutto benissimo"). Barra dei menu nativa dello standalone
  conservata anche nel plugin.
- **Committato il 2026-07-17** su branch `Screen-Reader-Accessibility` (prima volta che `adapters/`
  entra nel repo): commit `9bd299be` "adapters: add hand-written VST3 adapter..." (adapter + core
  rackhost + travesty + Makefile + .gitignore) e `199e5f83` "accessible: F6 returns focus to the
  DAW...". CLAP (`clap.cpp`/`claptest.cpp`) e `spike.cpp` restano **untracked** — commit futuro.
- **Login account persistente (2026-07-18, confermato dal vivo).** Il login fatto dentro il plugin
  non sopravviveva: `library::logIn` metteva `settings::token` solo in memoria e `processDeinit`
  NON salva le settings (per non sovrascrivere quelle dello standalone) → re-login ogni istanza, e
  i **moduli Plus restavano muti** (leggono il token alla creazione del modulo → senza entitlement
  niente audio). Fix: `settings::saveToken()` fa il MERGE del solo campo `token` nel `settings.json`
  esistente (non riscrive il resto), chiamato da `logIn`/`logOut`. Commit `d8a28a36`. Verifica nel
  log: riga `saveToken] Saving token to settings` tra `POST /token` e `GET /user`.
- ✅ **Confermato dal vivo il 2026-08-20** (Reaper + Ableton Live): plugin usato davvero dentro
  la DAW, comprese le scorciatoie nuove (vedi [[project-accessible-window]]). Restava aperto
  da luglio.
- ⚠️ **DUE CRASH LIVE APERTI (dal 2026-07-18): rimozione del plugin → DAW down; caricamento → device
  ASIO espulso ("can't open device"), Reaper e Live.** Debug in corso con build diagnostica deployata
  (handler crash + breadcrumb). Dettagli, cosa è escluso e prossimo passo in [[project-vst3-daw-crashes]].
- Poi: rimuovere il percorso legacy `adapters/vst3/{CMakeLists.txt,build.ps1,empty.cpp,build/}` e
  `dep/clap-wrapper` (il target `make vst3` non li usa già più; `build/` e `Rack.vst3` sono gitignored).

## Packaging VST3 — FATTO e verificato live-ish 2026-07-19

Bundle **rilocabile e autosufficiente**, installabile in una qualunque cartella VST3 (non più
vincolato a `C:\Rack`). Modello Cardinal (`%COMMONPROGRAMFILES%\VST3\Postmodular.vst3` sul disco
dell'utente = riferimento).

- **Vendoring**: `kVendor = "Luca Casarotti"`, `kPluginName = "MetaRack"` (vst3.cpp righe ~52-53),
  coerente con lo standalone (prodotto MetaRack, autore Luca Casarotti). Prima erano entrambi
  "Metarack". Il DAW mostra "MetaRack (Luca Casarotti)". La TUID **non è cambiata** (progetti esistenti
  reggono).
- **Bundle rinominato** `Rack.vst3` → **`MetaRack.vst3`** (Makefile `VST3_BUNDLE`/`VST3_TARGET`,
  stub, vst3test, .gitignore). Il modulo dentro `Contents/x86_64-win/` DEVE chiamarsi come il bundle.
- **Risoluzione asset a due modalità** (`rackhost.cpp`, nuova `findPackagedResources()`):
  se esiste `Contents/Resources/res` un livello sopra il modulo → **installato**: `systemDir` =
  quella `Resources`, `userDir` lasciato **vuoto** → `asset::init()` lo deriva da sé
  (`%LOCALAPPDATA%\Rack2`, **libreria/token/patch condivisi con lo standalone installato**; il
  `saveToken` merge-only evita di sovrascrivere le settings). Altrimenti (build in-tree) →
  comportamento storico (risale a `C:\Rack`, userDir = C:\Rack). Il CLAP non ha una `Resources`
  accanto → scartato dal ramo packaged, nessuna regressione sul core condiviso. **Il log packaged
  va in `%LOCALAPPDATA%\Rack2\vst3-log.txt`**, non più in C:\Rack.
- **Target `make vst3dist`** (Makefile): dipende da `vst3`, assembla in `dist/MetaRack.vst3` il
  bundle self-contained copiando in `Contents/Resources`: `res/`, `translations/`, `Core.json`,
  `template.vcv`, `cacert.pem`. `make vst3` resta il bundle LEAN in-tree per lo sviluppo (non tocca
  Resources → resta in modalità dev). Comando: `wsl … make -j4 vst3dist CROSS_COMPILE=… CXX=…`.
- **Installato in** `%COMMONPROGRAMFILES%\VST3\Luca Casarotti\MetaRack.vst3` (sottocartella vendor,
  copiata via PowerShell — serve elevazione; nella sessione avevo i permessi).
- **Verifica**: `RackVst3Test.exe` eseguito da cartella NEUTRA (scratchpad) contro il bundle
  **installato in Program Files**, ricerca DLL ristretta: `vendor: Luca Casarotti`, pass-through
  RMS ~0.35, MIDI + pitch-bend ok, ciclo di vita completo, exit 0. Log conferma
  `systemDir=…\Contents/Resources`, `userDir=…AppData\Local\Rack2`, Core caricato dal `res/` del
  bundle. ✅ **PROVATO DAL VIVO in Reaper e Ableton Live il 2026-08-20** ("funziona tutto"),
  col bundle installato in `%COMMONPROGRAMFILES%\VST3\Luca Casarotti\MetaRack.vst3`: il
  packaging rilocabile regge in una DAW vera, non solo sotto `RackVst3Test`.

## Limiti / nodi aperti

- **Mono-istanza**: `g_dawDriver`/`DawDevice` globali + singleton di processo → due istanze nello
  stesso DAW instradano male l'audio. Serve un Device per istanza instradato per deviceId.
- **Niente stato**: `get_state`/`set_state` → `V3_NOT_IMPLEMENTED`, la patch non si salva nel
  progetto del DAW. Prima estensione naturale (`v3_bstream` + `APP->patch`).
- **MIDI-in VST3 completo** (note via eventi + CC/pitch-bend/aftertouch via IMidiMapping, vedi
  sezione MIDI), ma **solo su Windows** (i parametri/mapping stanno nel controller ARCH_WIN) e **il
  CLAP non ha ancora il MIDI-in** (solo il VST3 hand-written).
- `APP->scene->step()` non gira in modalità plugin → display dei moduli e luci fermi (irrilevante
  per l'interfaccia accessibile).
- X sulla finestra accessibile in plugin chiama `APP->window->close()` sul GL window nascosto che
  nessuno poll-a → inerte. Andrebbe reso pluginMode-aware (hide + notifica all'host).
- Dialog modali `osdialog` da sopprimere in-DAW.

Piano: `~/.claude/plans/ora-torniamo-alla-realizzazione-peppy-dream.md`.
Vedi [[project-accessible-window]], [[feedback-build-command]].
