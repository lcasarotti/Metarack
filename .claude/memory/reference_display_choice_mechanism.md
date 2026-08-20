---
name: reference-display-choice-mechanism
description: Come funzionano i display cliccabili dei moduli Rack (Audio/MIDI/CC) a livello di codice — due famiglie di LedDisplayChoice
metadata: 
  node_type: memory
  type: reference
  originSessionId: bad7d18a-14f6-4b84-9f6f-844f910ed80c
---

I "display" cliccabili sui pannelli dei moduli (Audio device/driver, MIDI channel, MIDI-CC learn) **non** sono `ParamWidget` né `PortWidget`: sono widget grafici custom figli del `ModuleWidget`. La classe base comune è **`app::LedDisplayChoice`** (`: OpaqueWidget`, in `include/app/LedDisplay.hpp:22`). `LedDisplayChoice::onButton` (`src/app/LedDisplay.cpp:132`) traduce un click sinistro/destro in `onAction()`. Ogni cella tiene un puntatore al modello headless (`audio::Port*` / `midi::Port*`).

**Ci sono DUE famiglie di celle, entrambe derivate da `LedDisplayChoice`, attivate per vie diverse:**

1. **Celle a menù** (Audio `AudioDriverChoice`/`AudioDeviceChoice`/`AudioSampleRateChoice`/`AudioBlockSizeChoice` in `src/app/AudioDisplay.cpp`; MIDI `MidiDriverChoice`/`MidiDeviceChoice`/`MidiChannelChoice` in `src/app/MidiDisplay.cpp`). Fanno override di `onAction` → `createMenu()` (`include/helpers.hpp:182`, aggiunge un `ui::MenuOverlay`+`ui::Menu` come ULTIMO figlio di `APP->scene`) e popolano voci `ui::MenuItem` (`text`, `rightText`=CHECKMARK, `onAction` che fa `port->setDeviceId()` ecc.). `MenuItem::doAction()` (`src/ui/MenuItem.cpp:86`) esegue l'azione e chiude l'overlay. Alcune voci sono sotto-menù: `MidiChannelItem` usa `createChildMenu()` con RIGHT_ARROW (in `appendMidiMenu`) → il walker generico DEVE gestire l'annidamento.

2. **Celle a fuoco/learn** (`CcChoice`, `NoteChoice` in `src/core/plugin.hpp:61`, usate da MIDICC_CV ecc.). **NON** fanno override di `onAction`. Fanno override di `onSelect`/`onDeselect`/`onSelectText`/`onSelectKey`. Il click **seleziona** la cella (focus tastiera); `onSelect` mette `module->learningId = id` → learn mode; poi l'engine, nel suo `process()`, cattura il prossimo CC. In alternativa si digitano cifre (`onSelectText` accumula in `focusCc`) + Invio (`onSelectKey` → `onDeselect` → `setLearnedCc`).

**Perché si possono unificare:** `EventState::setSelectedWidget()` (`src/widget/event.cpp:174`) dispatcha davvero `onDeselect`/`onSelect`; `onSelectText`/`onSelectKey` vanno al widget selezionato. Quindi dalla finestra accessibile si pilotano ENTRAMBE le famiglie con la sola interfaccia base `Widget` (`onAction` per i menù; `setSelectedWidget`+`onSelectText`+`onSelectKey` per le celle a fuoco), **senza mai castare a tipi audio/MIDI né chiamare `setLearnedCc`**. Helper "tutto in uno" già pronti: `appendAudioMenu(menu, port)` / `appendMidiMenu(menu, port)`.

Implementazione accessibile pianificata in [[project-accessible-display-plan]]. Vedi anche [[project-accessible-window]].
