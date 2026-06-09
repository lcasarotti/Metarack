---
name: project-accessible-display-plan
description: Piano a fasi per esporre i display cliccabili dei moduli nella finestra accessibile (tasto D) — meccanismo generico Tier A menù / Tier B learn
metadata: 
  node_type: memory
  type: project
  originSessionId: bad7d18a-14f6-4b84-9f6f-844f910ed80c
---

## Obiettivo
Tasto **`D`** (da RACK e da PARAM) apre, per il modulo a fuoco, l'elenco delle celle di display gestibili. Meccanismo GENERICO astratto da audio/MIDI, basato sulla classe `app::LedDisplayChoice` e sull'interfaccia eventi base `Widget`. Vedi il meccanismo in [[reference-display-choice-mechanism]]. Si innesta su [[project-accessible-window]] (riusa vista CONTEXT_MENU, `showContextMenu`, `pushCommand`/`drainCommands`).

## Modello a due tier (fork all'apertura di una cella)
- **Tier A (cella a menù):** fai scattare `cell.choice->onAction()`; se compare un nuovo `ui::MenuOverlay` come ultimo figlio di `APP->scene` → cattura, tienilo vivo ma `hide()`, cammina i `ui::MenuItem` del `Menu` figlio (salta MenuLabel/MenuSeparator), converti in `ContextMenuItem`. Se `mi->createChildMenu()` è non-null → sotto-livello (annidamento via stack). Alla scelta foglia: `mi->doAction()` (chiude l'overlay) + `cleanupCapturedMenu()`.
- **Tier B (cella a fuoco/learn):** nessun overlay comparso → `APP->event->setSelectedWidget(cell.choice)` (= learn on), `learningCell=choice`, annuncio "in apprendimento". **Spazio** = toggle. Valore diretto: dialog numerico → inoltro per-carattere `onSelectText` + `onSelectKey(ENTER)`.

## STATO: ✅ VERIFICATO DAL VIVO 2026-06-08. Tier A funzionante su Audio-16. UX focus corretta.

### Fix UX focus display (2026-06-08, ✅ VERIFICATO)
1. **Focus resta nella display view dopo selezione Tier A:** quando `!displayCells.empty() && menuStack.size() >= 2`, invece di `switchView(previousView)` si riesegue `collectDisplayCells` + si ripopola `listContextMenu` + `switchView(CONTEXT_MENU)`.
2. **Focus torna sulla cella che ha aperto il sottomenù:** nuovo membro `int lastDisplayCellRow = 0`. Quando si apre una cella (livello 0→1), si salva la riga in `lastDisplayCellRow`. Al ritorno, `lvFocusRow` usa quel valore (con clamp).

## Note implementative
- `ContextMenuItem` ha un costruttore esplicito (C++11 non ammette brace-init su struct con std::function + default member initializer).
- `buildItemsFromMenu` usa lazy creation dei submenu: la probe `createChildMenu()` viene chiamata solo per detect (poi `delete probe`), e una seconda volta nel lambda quando l'utente naviga nel sottomenù.
- `cleanupCapturedMenu()` aggiunta anche ai path di rimozione modulo e in `reloadRackAfterMutation`.
