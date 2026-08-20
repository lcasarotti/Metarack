---
name: project-vst3-daw-crashes
description: "VST3 hand-written: crash rimozione/chiusura RISOLTO (use-after-free tearoff, fix identità-COM commit 6a2278dd, verificato live 2026-07-19). Anche il sintomo ASIO al caricamento NON si riproduce più dopo il fix (~10 cicli load/remove puliti in Reaper+Live): probabile radice comune (heap corruption dal UAF), non provato ma il filone è di fatto chiuso."
metadata: 
  node_type: memory
  type: project
  originSessionId: a8dbd285-bed3-4fd7-891f-d9d72fad561d
  modified: 2026-07-19T11:38:44.690Z
---

## AGGIORNAMENTO 2026-07-19 — ROOT CAUSE del crash di rimozione + FIX applicato

Il log diagnostico è arrivato (`vst3-log.txt`, ~310 righe) e la firma è netta: la sequenza di
teardown **arriva pulita fino in fondo** — `RackComponent::terminate` → `destroyInstance` (context
distrutto) → `~RackComponent: fine` — e **NON c'è nessun blocco `=== CRASH ===`**. Il crash avviene
DOPO che il nostro codice è tornato all'host. Inoltre `~RackComponent` parte **subito dopo** che
`terminate()` ritorna: segno che il rilascio di `IComponent` porta il refcount a 0 mentre l'host
tiene ancora altre interfacce.

**Causa: use-after-free dei tearoff per violazione dell'identità COM.** Ogni tearoff
(`RackAudioProcessor`, `RackEditController`, `RackMidiMapping`) aveva un `refcounter` PROPRIO e
separato. `RackComponent::queryInterface(IAudioProcessor)` faceva `++processor->refcounter`, non
`++component->refcounter`. Così la vita del component dipendeva SOLO dalla ref di `IComponent`:
l'host rilascia `IComponent` → `~RackComponent` → `delete processor; delete controller` mentre
l'host tiene ancora `IAudioProcessor`/`IEditController` → al rilascio successivo l'host dereferenzia
memoria liberata → crash fuori dal nostro logging. Vale identico per rimozione e chiusura sessione
(stesso teardown). Conferma pratica: nel log `~RackPlugView` è a 29.413, PRIMA della distruzione del
component (29.445) → il view è già rilasciato, per questo NON serve fixare il view.

**Fix (commit da fare): refcount unificato / identità COM.** Regola COM: tutte le interfacce
ottenute via `queryInterface` sullo stesso oggetto condividono UN refcount e muoiono insieme.
Modifiche in `adapters/vst3.cpp`:
- `RackComponent` ha ora un membro `selfPtr` (il `RackComponent**` di `create_instance`), settato in
  `createInstance`. `RackComponent::unrefFn` libera SEMPRE `selfPtr` (non il `cptr` derivato da
  `self`), perché l'ultimo rilascio può arrivare da un handle tearoff (`&component->processor`).
- `refFn`/`unrefFn` di processor, controller e midi-mapping ora **inoltrano** a
  `RackComponent::refFn/unrefFn(&x->component)` invece di toccare un contatore proprio.
- Tutte le QI che restituiscono un tearoff (`RackComponent::queryInterface` rami audio/controller;
  `RackEditController::queryInterface` self + midi_mapping; le QI dei tearoff stessi) fanno
  `++component->refcounter`. Il `refcounter` proprio dei tearoff è ora vestigiale (commentato).
- `RackMidiMapping` ha un back-pointer `component` (costruttore `RackMidiMapping(RackComponent*)`).

Build `make vst3` **pulita** (nessun warning) il 2026-07-19, bundle `C:\Rack\Rack.vst3` aggiornato.
**VERIFICATO DAL VIVO (2026-07-19): il crash NON compare più**, né alla rimozione né alla chiusura
sessione. L'utente per ora carica il .vst3 direttamente dalla cartella `C:\Rack` (bundle di build);
copierà poi il bundle nella cartella VST3 di sistema.

**Stato strumentazione (pulita 2026-07-19, commit `e34019c8`):**
- Breadcrumb INFO di teardown/caricamento RIMOSSI (vst3.cpp + destroyInstance in rackhost.cpp).
- Crash handler `SetUnhandledExceptionFilter` **TENUTO in pianta stabile** (costo zero,
  EXCEPTION_CONTINUE_SEARCH): rete di sicurezza per bug futuri.
- Rimozione di `window::destroy()`/glfwTerminate da destroyInstance TENUTA (fix corretto per
  adapter mono-istanza).

**Commit:** fix `6a2278dd` + pulizia `e34019c8`, entrambi su `Screen-Reader-Accessibility` (2026-07-19).
- Sintomo **#1 ASIO "can't open device" NON si riproduce più dopo il fix** (utente: ~10 cicli
  load/remove in Reaper e Live, device audio mai più espulso). Ipotesi: stessa radice del crash —
  il UAF corrompeva heap/stato globale e il danno si manifestava anche sull'ASIO; oppure i test
  ASIO precedenti erano su sessioni già inquinate da crash pregressi. NON dimostrato, ma il filone
  è di fatto chiuso. Se dovesse ricomparire, le ipotesi originali (globali GLFW / blocco main
  thread ~1,7 s / 8 bus input) restano sotto in questa nota.

---


Filone aperto il **2026-07-18** (Reaper **e** Ableton Live, entrambi con device **ASIO**). Riguarda
il VST3 hand-written travesty (`adapters/vst3.cpp` + core `adapters/rackhost.cpp`, `make vst3`), non
il CLAP. Contesto architetturale: vedi [[project-plugin-adapter]].

## I due sintomi (riferiti dall'utente)

1. **Caricamento → il device ASIO della DAW viene espulso** con l'errore generico "**can't open
   device**" (sia Reaper sia Live). ASIO è single-client: "can't open device" = la DAW non riesce a
   (ri)aprire l'ASIO. Il nostro `audio::init()` NON tocca RtAudio/ASIO (registra solo il driver finto
   "DAW"), quindi non è un conflitto di risorsa dal nostro codice — causa ancora da determinare.
2. **Rimozione del plugin dalla traccia → crash della DAW** (sia Reaper sia Live).

## Cosa è stato ESCLUSO / stabilito (2026-07-18)

- **Il crash di rimozione NON è `glfwTerminate`.** Prima ipotesi: `destroyInstance` chiamava
  `window::destroy()`(=`glfwTerminate`)+`ui::destroy()` dopo `~Context`, e lo standalone su Windows
  evita apposta quel teardown (`standalone.cpp:324-347` fa `TerminateProcess`). **Ho rimosso quelle
  due chiamate** da `destroyInstance` (rackhost.cpp, ~riga 621; `ui::destroy` è comunque vuota,
  `ui/common.cpp:14`) → **il crash NON si è spostato**: muore ancora subito dopo l'ultima riga di
  `~Context` ("Deleting MIDI loopback", `context.cpp:42`). La rimozione di glfwTerminate resta (è
  comunque corretta: mono-istanza, GLFW resta init per la vita del processo), ma non era la causa.
- Il logger fa `fflush` per riga (`logger.cpp:122`), quindi l'ultima riga del log è affidabile:
  `~Context` completa TUTTI i suoi delete (window→patch→scene→event→history→**engine**→midiloopback),
  **`~Engine` compreso** (la riga midiloopback viene dopo "Deleting engine"). Quindi NON è la race
  ~Engine-vs-thread-audio che descrive standalone.cpp (a quel punto l'audio è quiescente).
- `delete midiLoopbackContext` (ultima riga di `~Context`) è **banale**: `~Context` midiloopback
  cancella 16 `Device` con distruttori vuoti (`midiloopback.cpp:105`, `midi.hpp:157` `~Device(){}`).
- Quindi il crash di rimozione è **dopo che `~Context` ritorna**: candidati rimasti = `delete inst`
  (banale) oppure la **teardown ABI VST3 che l'host esegue DOPO `terminate()`** (rilascio di
  view/controller/component → `~RackComponent` che fa `delete processor; delete controller`), oppure
  una `process()` che il thread audio esegue con l'istanza già liberata (`useContext` su context
  freed). Non ancora distinto → per questo la build diagnostica.

## Build DIAGNOSTICA attualmente deployata (NON un fix — da rimuovere dopo)

Ricompilata `make vst3` il 2026-07-18 (bundle `C:\Rack\Rack.vst3\...` aggiornato). Aggiunge:

- **Handler crash** in `rackhost.cpp` (funz. `crashHandler`/`logAddressModule`, installato con
  `SetUnhandledExceptionFilter` in `processInit` subito dopo `logger::init`): a QUALUNQUE eccezione
  non gestita, su QUALUNQUE thread, scrive nel log `=== CRASH: eccezione 0x… sul thread N ===` +
  `fault modulo+0xoffset` + stack (`RtlCaptureStackBackTrace`). Dice il **modulo che fa fault**
  (RackVst3Adapter.dll / libRack.dll / driver GL Intel / driver ASIO). Ritorna
  `EXCEPTION_CONTINUE_SEARCH` (non altera il comportamento del crash).
- **Breadcrumb teardown** (INFO): `RackComponent::terminate` (entra/esce), `destroyInstance`
  (inizio → accessibleWindow distrutta → context distrutto → fine), `~RackComponent` (inizio/fine),
  `RackPlugView::removed`, `~RackPlugView`.
- **Breadcrumb caricamento** (INFO): `setBusArrangements` (quanti bus chiede l'host vs i nostri 8/8),
  `setupProcessing` (sampleRate/blockSize), `setActive(0/1)` con thread-id.

## PROSSIMO PASSO (da fare all'apertura della prossima sessione)

Chiedere all'utente il **tail (~50 righe) di `C:\Rack\vst3-log.txt`** dopo aver riprodotto:
carica il plugin (fa scattare l'espulsione ASIO) → rimuovilo (fa scattare il crash). La sezione
`=== CRASH ===` localizza il fault; i breadcrumb dicono l'ultimo passo prima. Da lì si decide il
fix vero. Se al caricamento NON compare `=== CRASH ===`, l'ASIO cade senza crash nel nostro processo
→ effetto collaterale (candidati: `SetProcessDpiAwarenessContext` process-wide o `DirectInput8Create`
in `glfwInit`, vedi `dep/glfw/src/win32_init.c:95,630`; oppure la dichiarazione **8 bus input** che
fa riconfigurare l'audio della DAW; oppure il blocco ~1,4 s del main thread in `createInstance`).

## Ipotesi ancora in piedi per #1 (ASIO "can't open device")

- Non apriamo ASIO noi. Possibili cause indirette: (a) `glfwInit` cambia DPI-awareness di TUTTO il
  processo + carica DirectInput → la DAW reagisce riconfigurando l'audio; (b) dichiarare **8 bus di
  INGRESSO** su uno strumento fa sì che la DAW tenti di dare 16 canali in input → riconfigura/riapre
  l'ASIO e fallisce; (c) `createInstance` blocca il main thread ~1,4 s (crea finestra GL + font +
  SVG) → l'ASIO va in timeout. Da discriminare col log.

Vedi [[project-plugin-adapter]], [[feedback-build-command]] (comando `make vst3` da WSL).
