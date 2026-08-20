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

## Stato nuovo (AccessibleWindow.hpp)
- `struct DisplayCell { app::LedDisplayChoice* choice; std::wstring label; }; std::vector<DisplayCell> displayCells;`
- `std::vector<std::vector<ContextMenuItem>> menuStack;` (per menu→sottomenu, Esc torna indietro un livello)
- `ui::MenuOverlay* capturedOverlay = nullptr;`
- `app::LedDisplayChoice* learningCell = nullptr; std::wstring learningLastText;`
- Estendere `ContextMenuItem` con `std::function<std::vector<ContextMenuItem>()> submenu;` (opzionale; voci esistenti lo lasciano nullo).

## Funzioni nuove
`collectDisplayCells(ModuleWidget*)` (ricorsiva su `children`, dynamic_cast a LedDisplayChoice, `step()` poi `text`), `handleDisplayKey()`, `openDisplayCell(DisplayCell)`, `buildItemsFromMenu(ui::Menu*)` (ricorsiva), `pushMenuLevel(items)`, `cleanupCapturedMenu()`.

## Punti d'innesto (ChildSubclassProc, src/accessible/AccessibleWindow.cpp)
- Nuovo `case 'D'` accanto a 'P'/'O'/'I' (~riga 2119). NB: il `case 'D'` a riga ~1684 è dentro `handleRackCtrlKey` = **Ctrl+D duplica**, diverso dalla D nuda.
- `VK_ESCAPE` in CONTEXT_MENU (~riga 2051): se `menuStack.size()>1` pop+ripopola, else `switchView(previousView)` + `cleanupCapturedMenu()`.
- `VK_RETURN` in CONTEXT_MENU (~riga 2070): se voce ha `submenu` → push livello; else `action()`.
- `VK_SPACE`: se `learningCell` attiva → toggle.
- `onTimer`: polling `learningCell->text`; se cambiato → annuncia "CC appreso"; se `getSelectedWidget()!=learningCell` → azzera.
- Cleanup in `reloadRackAfterMutation`/`removeAction`/distruttore: `cleanupCapturedMenu()` + azzera displayCells/menuStack/learningCell. All'apertura ricavare ModuleWidget fresco via `rack->getModule(currentModule->id)`.

## Rollout incrementale (build MinGW + prova NVDA ad ogni step)
1. **Step 1** — scoperta + Tier A flat. Test su **Audio-16**. ← PARTENZA
2. **Step 2** — `buildItemsFromMenu` ricorsiva + stack (sotto-menù). Test su **MIDI** (canale).
3. **Step 3** — Tier B learn: select + Spazio toggle + polling. Test su **MIDICC_CV**.
4. **Step 4** — valore diretto via dialog → onSelectText/onSelectKey.

## Dettagli UX/edge
- Voci `disabled` incluse con no-op + " (non disponibile)". `rightText`/CHECKMARK → suffisso " ✓"/" (selezionato)". Modulo senza display → status "Nessun display cliccabile".
- Etichetta cella ambigua (solo valore es. "48 kHz") accettata per v1; miglioria futura: leggere la MenuLabel header del menù.

## Rischi
- Discovery limitata a `LedDisplayChoice` (display custom non derivati mancano) — accettabile v1.
- Tier A dipende dall'overlay come ultimo figlio di APP->scene (confermato `helpers.hpp:189`); verificare che sia davvero `ui::MenuOverlay` per non cadere per errore nel Tier B.

## STATO: ✅ VERIFICATO DAL VIVO 2026-06-08. Tier A funzionante su Audio-16. UX focus corretta (vedi fix sotto).

### Note implementative (delta rispetto al piano)
- `ContextMenuItem` ha un costruttore esplicito (C++11: brace-init non funziona con default member initializer + std::function).
- La discovery `collectDisplayCellsRec` è una static free function nel .cpp che scende ricorsivamente nell'albero dei widget del ModuleWidget.
- `buildItemsFromMenu` usa lazy creation dei submenu: la probe `createChildMenu()` viene chiamata solo per detect (poi `delete probe`), e una seconda volta nel lambda quando l'utente naviga nel sottomenù.
- Tier B (learn): `setSelectedWidget(learningCell)`, polling in `onTimer` ogni 200ms, VK_SPACE per toggle, VK_ESCAPE per annullare.
- `cleanupCapturedMenu()` aggiunta anche ai due path di rimozione modulo (context menu Elimina + handleRackKey VK_DELETE/VK_BACK) e in `reloadRackAfterMutation`.

### Fix UX focus display (2026-06-08, ✅ VERIFICATO)
Due correzioni applicate al gestore `VK_RETURN` in `CONTEXT_MENU` (ChildSubclassProc):

1. **Focus resta nella display view dopo selezione Tier A:** quando `!displayCells.empty() && menuStack.size() >= 2` (siamo nelle opzioni di una cella, non nella lista celle), invece di `switchView(previousView)` si riesegue `collectDisplayCells` + si ripopola `listContextMenu` + `switchView(CONTEXT_MENU)`. `previousView` non viene toccato → ESC continua a tornare a RACK/PARAM.

2. **Focus torna sulla cella che ha aperto il sottomenù:** nuovo membro `int lastDisplayCellRow = 0` (azzerato da `cleanupCapturedMenu`). Quando si apre una cella (livello 0→1, `menuStack.size()==1`), si salva la riga in `lastDisplayCellRow`. Al ritorno, `lvFocusRow` usa quel valore (con clamp) invece di 0.
