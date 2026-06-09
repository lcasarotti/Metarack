---
name: project-accessible-window
description: "Stato corrente dell'AccessibleWindow Win32 per VCV Rack — architettura, file, shortcut, TODO aperti"
metadata: 
  node_type: memory
  type: project
  originSessionId: 30607400-6056-4e6b-ba5a-64583aa47520
---

## Cos'è

Un **layer di accessibilità a toggle** sovrapposto alla finestra principale di VCV Rack, progettato per essere completamente navigabile via tastiera e compatibile con screen reader (MSAA/NVDA). Disponibile solo su Windows (`#if defined ARCH_WIN`).

## Layer posseduto da Rack + toggle Ctrl+Shift+A (commit ef26a521, v2, 2026-06-08, ✅ VERIFICATO DAL VIVO "funziona perfettamente")

**Migrazione da finestra separata a layer** (richiesta utente: "il codice dell'accessible window doveva migrare dentro la finestra principale come layer di accessibilità senza una sua finestra dedicata"). Vincolo di fondo: l'accessibilità nasce dai controlli nativi Win32 (ListView/TreeView/StatusBar + menu bar `HMENU`), che hanno bisogno di un HWND ospite → quindi **resta un HWND**, ma:
- **`create(HWND owner)`** (firma cambiata): la finestra è creata con `WS_EX_TOOLWINDOW` (niente Alt+Tab / taskbar) e **`hwndParent = owner`** = posseduta dalla finestra Rack. Niente più `WS_VISIBLE` alla creazione (parte nascosta). `owner` = `glfwGetWin32Window(APP->window->win)` ricavato in `standalone.cpp` (aggiunto `#define GLFW_EXPOSE_NATIVE_WIN32` + `#include <GLFW/glfw3native.h>` sotto ARCH_WIN; `Window::win` è membro pubblico).
- **`setLayerVisible(bool)`** (nuovo helper): show → dimensiona il layer sul rect di Rack (`GetWindowRect(rackHwnd)` → `SetWindowPos`), `ShowWindow(SW_SHOW)`, `SetForegroundWindow(hwnd)`, focus su `activeControl()`; hide → `ShowWindow(SW_HIDE)` + `SetForegroundWindow(rackHwnd)` (focus torna alla GUI di Rack).
- **`activeControl()`** (nuovo helper): HWND del controllo della vista attiva; estratto dall'array `views[]` prima duplicato in `WM_ACTIVATE`/`WM_HOTKEY`.
- **`Ctrl+Shift+A` = toggle**: `WM_HOTKEY` ora fa `setLayerVisible(!IsWindowVisible(hwnd))`. L'hotkey resta `RegisterHotKey` **globale** → scatta sia da layer nascosto (Rack a fuoco) sia da layer visibile.
- **`WM_CLOSE`**: da `SW_MINIMIZE` a `setLayerVisible(false)` (la X / Alt+F4 sul layer lo spegne e ridà il focus a Rack).
- **Avvio: layer SPENTO** (scelta utente 2026-06-08, VERIFICATA DAL VIVO che il toggle funziona). `onCreate` fa solo `refreshRackView()`+`rackDirty=false` per pre-popolare la lista (pronta al primo show) ma NON chiama `setLayerVisible(true)` → la finestra resta nascosta (creata senza `WS_VISIBLE`). L'utente accende a piacere con `Ctrl+Shift+A`. (Prima il default era acceso; cambiato a spento su richiesta.)

**Membro nuovo:** `HWND rackHwnd` (header). `standalone.cpp`: `create(rackHwnd)` resta dopo `patch->launch()` (refreshRackView vede il rack popolato) e prima di `window->run()`; `delete` dopo `run()` invariato.

## File coinvolti

| File | Ruolo |
|---|---|
| `include/accessible/AccessibleWindow.hpp` | Dichiarazione della struttura `rack::accessible::AccessibleWindow` |
| `src/accessible/AccessibleWindow.cpp` | Implementazione completa |
| `adapters/standalone.cpp` | Punto di integrazione: `AccessibleWindow::create()` dopo `APP->window`, `delete` alla chiusura |

La build include automaticamente `src/accessible/AccessibleWindow.cpp` grazie al wildcard `$(wildcard src/*.cpp src/*/*.cpp)` nel Makefile.

## Architettura della finestra

La finestra ha **6 viste** selezionabili via tasto, una visibile alla volta:

| Enum | HWND | Tipo controllo | Contenuto |
|---|---|---|---|
| `RACK` | `listRack` | `WC_LISTVIEW` (**icon view 2D**, vedi sezione dedicata) | Tutti i moduli nel rack, disposti su una griglia che rispecchia le file fisiche |
| `LIBRARY` | `treeLibrary` | `WC_TREEVIEW` | Albero plugin → modelli |
| `PARAM` | `listParam` | `WC_LISTVIEW` | Parametri del modulo selezionato (col: Nome, Valore) |
| `OUTPUT` | `listOutput` | `WC_LISTVIEW` | Output del modulo selezionato (col: Nome, Stato) |
| `INPUT` | `listInput` | `WC_LISTVIEW` | Input del modulo selezionato (col: Nome, Stato) |
| `CONTEXT_MENU` | `listContextMenu` | `WC_LISTVIEW` | Voci contestuali (colonna singola "Azione") |

Più una **status bar** in fondo con messaggi contestuali in italiano.

## Rack view come griglia 2D spaziale (icon view) — punto 1 fatto 2026-06-06, ✅ VERIFICATO DAL VIVO (navigazione ←/→ ok)

**Motivazione:** il rack di VCV è una griglia 2D — ogni `ModuleWidget` ha `getGridPosition()` con `y` = fila (RACK_GRID_HEIGHT=380px = 3U) e `x` = colonna in HP (RACK_GRID_WIDTH=15px). La vecchia rack view (`LVS_REPORT`, lista verticale a colonne Nome/Produttore/HP) appiattiva tutto in una lista verticale, scorribile solo con ↑/↓. Nelle patch grandi diventa scomodo saltare da cima a fondo. Scelta utente: replicare il layout fisico con una **griglia 2D** in cui ←/→ scorrono dentro una fila e ↑/↓ saltano tra file.

**Vincolo tecnico chiave:** una ListView `LVS_REPORT` naviga solo verticalmente (←/→ non spostano il focus tra item). Per avere frecce orizzontali/2D serve `LVS_ICON` (o smallicon) con posizionamento manuale. **Tradeoff accettato:** in icon view spariscono le sottocolonne — NVDA non legge più Produttore/HP. Per ora l'etichetta dell'item è **solo il nome** del modulo. Brand/HP "parcheggiati" (TODO: ripiegarli nell'etichetta es. "VCO — Befaco, 10 HP" o annunciarli sulla status bar).

**Implementazione (punto 1):**
- `onCreate`: `listRack` ha stile proprio `LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS` (NIENTE `LVS_AUTOARRANGE`, che rifluirebbe per larghezza finestra e distruggerebbe le posizioni manuali; niente più `lvAddColumn`/`SetExtendedListViewStyle` per il rack). Le altre 5 liste restano `LVS_REPORT`.
- `refreshRackView`: ordina i moduli per `(getGridPosition().y, .x)`; legge il passo con `ListView_GetItemSpacing(lv, FALSE)` → `stepX=LOWORD, stepY=HIWORD`; per ogni modulo `ListView_SetItemPosition(item, col*stepX, visRow*stepY)`. **Emette una fila visiva nuova ogni volta che cambia la `y` di griglia** → già pronta per le file multiple. `[ Slot libero ]` (lParam==0) va nell'ultima cella a destra dell'ultima fila.
- **Tasti invariati:** in RACK le frecce non sono intercettate da `ChildSubclassProc` → la ListView le gestisce spazialmente. Type-ahead per iniziale resta attivo. (←/→ sono intercettate SOLO in PARAM.)
- Oggi `nextModulePos()` piazza sempre su `y=0` → in pratica una sola fila orizzontale, slot libero a destra. ↑/↓ in RACK non fanno nulla finché non ci sono più file.

## Punto 2 — Ctrl+Invio crea nuova fila + uno slot libero per fila — commit 714cc0d5 (v2), 2026-06-06, ✅ MECCANISMO VERIFICATO DAL VIVO ("funziona perfettamente")

Scelta utente: **uno slot libero per FILA** (modello 2D pieno), non uno globale.

**`refreshRackView`** ora chiude ogni fila con il proprio `[ Slot libero ]` (lParam==0) posizionato subito a destra dell'ultimo modulo della fila. Per ciascuno registra il target di griglia in **`freeSlotTargets`** (nuovo membro: `vector<FreeSlotTarget{int item; int gridX; int gridY;}>`, `item`=indice nell'item list, `gridX`=`max(gridPos.x+gridSize.x)` della fila, `gridY`=`y` di griglia della fila). Rack vuoto → un solo slot libero a (0,0). `visRow` parte da **−1** (++ sulla prima fila → 0).

**Inserimento per fila:** `nextModulePos()` (che metteva sempre `y=0`) è stato **rimosso**; al suo posto helper statico `gridToPixel(gridX,gridY)` = `Vec(gridX,gridY)*RACK_GRID_SIZE + RACK_OFFSET` (specchio di `setGridPosition`). `placeModule` e `pasteModuleFromClipboard` ora prendono `(int gridX, int gridY)` e chiamano `setModulePosNearest(mw, gridToPixel(...))`. I call site (`handleRackKey` Invio su slot libero; `handleRackCtrlKey` Ctrl+V su slot libero) leggono il target dello slot a fuoco con **`freeSlotTargetForItem(item, gx, gy)`** e lo catturano **per valore** nella `pushCommand` (così un rebuild successivo non lo invalida).

**Ctrl+Invio** (`moveFocusedModuleToNewRow`, intercettato in `ChildSubclassProc` PRIMA del `VK_RETURN` normale, solo in RACK): sposta il modulo a fuoco su una fila nuova = `maxGy+1` di tutti i moduli, bordo sinistro (`gridToPixel(0,newGy)`). Usa `requestModulePos` (fila vuota → riesce) con fallback `setModulePosForce`. Spostamento annullabile: push di `history::ModuleMove` (`moduleId`/`oldPos`/`newPos` in pixel — `moduleId` è pubblico in `ModuleAction`). Differito via `pushCommand`; `refreshRackView(mw)` tiene il focus sul modulo spostato. Su slot libero a fuoco → no-op con status. ↑/↓ tra le file funzionano da sé in icon view.

**Coordinate annunciate da NVDA (richiesta utente, commit 714cc0d5, 2026-06-06):** helper lambda `coordSuffix(fila1, slot1)` in `refreshRackView` genera `" — fila N, slot M"` / `" — row N, slot M"` (localizzato via T()) appeso all'etichetta di OGNI item (moduli + slot liberi + slot del rack vuoto). **fila** = riga 1-based (`visRow+1`), **slot** = posizione sequenziale 1-based nella fila (`col+1`), NON la coordinata HP (scelta esplicita: più utile per navigare). Il nome modulo resta in testa → type-ahead invariato. NVDA legge il suffisso automaticamente (nessun evento MSAA extra).

**Ancora da osservare in uso (non bloccanti, meccanismo già OK):** Ctrl+Z sull'annullamento spostamento; comportamento con moduli expander affiancati (updateExpanders è chiamato da requestModulePos/Force); eventuale verbosità del suffisso coordinate a ogni freccia (se fastidioso → accorciare o mettere dietro toggle).

## Bottoni momentanei: Spazio = un toggle (commit 87474cc2, v2, 2026-06-07, ✅ VERIFICATO DAL VIVO)

**Problema:** in VCV i bottoni hanno due nature. Gli **switch latching** (`configSwitch`) tengono lo stato nel valore del parametro → `Spazio` con increment-and-wrap già funzionava (un colpo = un cambio). I **bottoni momentanei** (`configButton`, es. il Run di un sequencer) sono invece pilotati dal **fronte di salita** rilevato dal modulo nel thread audio (Schmitt trigger): il widget porta il param a `max` alla pressione e a `min` al rilascio, e il modulo ribalta il proprio stato interno sul fronte. Il vecchio codice trattava TUTTO come latching → alla prima pressione il param saliva a 1 (toggle ✓) ma restava lì; serviva una seconda pressione solo per riportarlo a 0 prima che una terza potesse togglare di nuovo → **due colpi per ogni cambio**.

**Vincolo chiave:** il flag `momentary` vive sul widget `app::Switch` (`include/app/Switch.hpp`), **non** sulla `ParamQuantity`. Per scoprirlo bisogna risalire al widget on-screen: `APP->scene->rack->getModule(id)->getParam(paramId)` → `dynamic_cast<app::Switch*>` → `->momentary`. Helper nuovo `isMomentaryParam(int)`.

**Fix (`handleParamKey`, ramo `VK_SPACE && snapEnabled`):** se il param è momentaneo, simula press+release: `setValue(maxValue)` ora, registra `momentaryModule`/`momentaryParamId`, avvia timer one-shot `TIMER_MOMENTARY` (id=2, **80 ms**), e al fire `onMomentaryRelease()` riporta a `minValue`. Il ritardo è necessario perché il thread audio campiona i parametri a blocchi: settare alto e subito basso nello stesso handler perderebbe il fronte. 80 ms coprono diversi blocchi. `onMomentaryRelease` ri-valida il modulo contro l'engine (`APP->engine->getModule(id) == momentaryModule`) prima di toccarlo, nel caso (raro) venga rimosso nella finestra di 80 ms. Gli switch latching cadono invariati nell'increment-and-wrap.

**Nota:** per i momentanei il valore a riposo è sempre 0, quindi la cella valore non riflette lo stato "in esecuzione" (interno al modulo, non esposto come param) — feedback solo udibile. Inerente alla natura del bottone.

## Selezione multipla nella rack view — Spazio toggle (v2, 2026-06-08, build OK, da verificare dal vivo)

**Scelta utente (2 domande poste):** (1) costruire la selezione con **toggle esplicito Spazio** (non ListView a selezione estesa nativa), navigazione invariata a selezione singola; (2) **Canc = elimina tutta la selezione** se presente, altrimenti il singolo modulo a fuoco.

**Fonte di verità unica:** il set nativo `RackWidget::selectedModules` (`std::set<ModuleWidget*>`, API `select/isSelected/hasSelection/getSelected/selectAll/deselectAll`). Così il toggle accessibile interopera con le voci di menù Modifica già presenti (Seleziona tutto, Copia selezione, Reset/Randomizza/Scollega/Bypass selezione) e con `Ctrl+R`. NB: `RackWidget::removeModule` fa già `selectedModules.erase(m)` → eliminare un singolo modulo selezionato non lascia puntatori penzolanti.

**Toggle (`toggleRackSelection()`, Spazio in RACK):** intercettato nel `case VK_SPACE` di `ChildSubclassProc` (dopo i rami learn-mode e PARAM). Su slot libero → status "niente da selezionare". Su modulo: `rack->select(mw, !isSelected)`. Feedback: **aggiornamento mirato della SOLA riga** (`lvSetSubtext(listRack, row, 0, label)` — niente `refreshRackView` completo, evita flood MSAA e doppia lettura) aggiungendo/togliendo il marcatore `" — selezionato"`/`" — selected"` (legge il testo corrente con `ListView_GetItemText`, striscia il marcatore se già presente, lo riappende se `nowSel`), poi `NotifyWinEvent(EVENT_OBJECT_FOCUS, ...)` per far rileggere l'etichetta a NVDA (canale affidabile: lo status può essere droppato mentre NVDA parla). Status: "N moduli selezionati".

**Marcatore in `refreshRackView`:** ogni modulo con `isSelected(mw)` ottiene `+= " — selezionato"` **dopo** il `coordSuffix` (suffisso, non prefisso: il nome resta in testa per la type-ahead, NVDA legge il marcatore scorrendo). Così i rebuild (dopo operazioni, selectAll, ecc.) rendono i marcatori in modo consistente.

**Canc/Backspace (`handleRackKey`):** il ramo `hasSelection()` è valutato **prima** del check slot-libero → conferma `MessageBoxW` "Eliminare N moduli?" → `deleteSelectionAction()` differita (azzera `currentModule`/`lastParamModule`, `cleanupCapturedMenu`, `refreshRackView(nullptr, 0)` = focus sul primo modulo rimasto perché i cancellati possono coprire più file). Selezione vuota → path singolo invariato (conferma per nome, focus a `row-1`).

**Menu Modifica → Seleziona tutto / Deseleziona:** ora catturano `this`, fanno `refreshRackView()` se `currentView==RACK` (markers) altrimenti `rackDirty=true`, e annunciano "N moduli selezionati"/"Selezione azzerata".

**Shift+Backspace (RACK) = azzera selezione rapida** (richiesta utente 2026-06-08, dopo che il toggle Spazio è stato VERIFICATO DAL VIVO funzionante): nel `case VK_BACK` del dispatch, ramo `shift` PRIMA del delete normale → `deselectAll()` + `refreshRackView()` + status "Selezione azzerata" (o "Nessuna selezione" se vuota). Nessuna eliminazione.

**✅ STATO 2026-06-08:** toggle Spazio + Canc-elimina-selezione + Ctrl+R randomizza VERIFICATI DAL VIVO dall'utente ("funziona"). Shift+Backspace aggiunto subito dopo, build OK.

**Possibile follow-up (non fatto):** voce "Elimina selezione" nel menu Modifica per coerenza con le altre azioni di selezione (oggi l'eliminazione multipla è solo via Canc).

## Scorciatoie da tastiera

| Tasto | Contesto | Azione |
|---|---|---|
| `Shift+R` | Ovunque | Vai alla vista RACK; se già in RACK, forza refresh |
| `Shift+L` | Ovunque | Vai alla vista LIBRARY |
| `Ctrl+N` | Ovunque | Nuova patch (`loadTemplateDialog` + reload) |
| `Ctrl+O` | Ovunque | Apri patch (`loadDialog` + reload) |
| `Ctrl+Shift+O` | Ovunque | Ripristina dal salvato (`revertDialog` + reload) |
| `Ctrl+S` | Ovunque | Salva patch |
| `Ctrl+Shift+S` | Ovunque | Salva come… |
| `Ctrl+Z` | Ovunque | Annulla (undo + reload) |
| `Ctrl+Shift+Z` | Ovunque | Ripristina (redo + reload) |
| `Ctrl+R` | Ovunque | Randomizza selezione (se presente); annuncia "N moduli randomizzati" o "Nessuna selezione" |
| `Spazio` | RACK | Toggle del modulo a fuoco dentro/fuori la selezione multipla (vedi sezione dedicata) |
| `Shift+Backspace` | RACK | Azzera rapidamente la selezione multipla (deselectAll, nessuna eliminazione) |
| `Ctrl+Q` | Ovunque | Chiudi Rack |
| `Esc` | Ovunque | Annulla cavo pendente; oppure torna a RACK |
| `Invio` | RACK | Apri PARAM del modulo; se slot libero + modello in coda → posiziona; se slot libero senza modello → apre LIBRARY |
| `F1` | Ovunque | Apre il manuale Rack nel browser di sistema |
| `Canc` | RACK | Se c'è una selezione multipla → elimina TUTTA la selezione (`deleteSelectionAction`, conferma "Eliminare N moduli?", un solo undo); altrimenti rimuovi il solo modulo a fuoco (con conferma MessageBox) |
| `P` | RACK | Vai a PARAM del modulo focalizzato |
| `O` | RACK | Vai a OUTPUT del modulo focalizzato |
| `I` | RACK | Vai a INPUT del modulo focalizzato |
| `Invio` | LIBRARY | Seleziona modello → torna a RACK; se nodo brand → espandi/comprimi |
| `←` / `→` | PARAM | Decrementa/incrementa valore (passo normale = 1% del range) |
| `Ctrl+←` / `Ctrl+→` | PARAM | Passo lento (×1/10) — specchio di Ctrl+drag |
| `Shift+←` / `Shift+→` | PARAM | Passo veloce (×4) — specchio di Shift+drag |
| `Ctrl+Shift+←` / `Ctrl+Shift+→` | PARAM | Passo lentissimo (×1/100) — specchio di Ctrl+Shift+drag |
| `Spazio` | PARAM | Switch latching (`configSwitch`): cicla i valori snap con wrap. Bottone **momentaneo** (`configButton`, es. Run): un colpo = un toggle (vedi sezione dedicata) |
| `Invio` | OUTPUT/INPUT | Avvia connessione cavo (primo invio) o completa (secondo invio su porta compatibile) |
| `Backspace` | RACK | Rimuove modulo (alias di Canc, con conferma) |
| `Backspace` | PARAM | Reset parametro al valore di default (stile Ableton, via `pq->reset()`) |
| `Canc` / `Backspace` | OUTPUT/INPUT | Scollega i cavi sulla porta a fuoco (`handlePortDelete`, con undo `CableRemove`) |
| `Ctrl+Shift+A` | Globale (hotkey) | **Toggle** del layer di accessibilità: ON = copre Rack e prende il focus; OFF = nascosto, focus alla GUI di Rack (vedi sezione "Layer posseduto da Rack") |
| `Tasto Applicazioni` / `Shift+F10` | RACK (su modulo) | Apre menu contestuale GENERICO del modulo (7 voci: azzera, randomizza, disconnetti, bypass toggle, duplica ×2, elimina) |
| `Ctrl+Tasto Applicazioni` | RACK / PARAM / OUTPUT / INPUT (su modulo) | Apre menu contestuale SPECIFICO del modulo, stile Reaper (le voci del suo `appendContextMenu`: es. MIDI-to-CV → Polyphony channels, Polyphony allocation Rotate/Reuse/Reset, CLK/N divider) |
| `V` | PARAM | Apre direttamente il dialog "Imposta valore" per il parametro focalizzato |
| `Tasto Applicazioni` / `Shift+F10` | PARAM | Apre menu contestuale del parametro ("Imposta valore…" + azzera al default) |
| `Invio` | CONTEXT_MENU | Esegue la voce selezionata e torna alla vista precedente; se voce con `isSubmenu=true` → push nuovo livello (display menu nav) |
| `Esc` | CONTEXT_MENU | Se `menuStack.size()>1` → pop un livello; altrimenti chiude e torna a `previousView` |
| `D` | RACK / PARAM | Apre la lista dei display cliccabili del modulo corrente (tasto `D` senza modificatori) |
| `Spazio` | Ovunque (quando `learningCell` attivo) | Toggle Tier B learn mode (seleziona/deseleziona la cella) |
| `Esc` | Ovunque (quando `learningCell` attivo) | Annulla il Tier B learn mode |
| `Ctrl+C` | RACK (su modulo) | Copia il preset del modulo negli appunti (`ModuleWidget::copyClipboard()`) |
| `Ctrl+V` | RACK (su modulo) | Incolla il preset copiato sul modulo a fuoco, in loco (`pasteClipboardAction()`) |
| `Ctrl+V` | RACK (su `[ Slot libero ]`) | Inserisce un NUOVO modulo dagli appunti in coda alla fila (`pasteModuleFromClipboard()`) |
| `Ctrl+D` / `Ctrl+Shift+D` | RACK (su modulo) | Duplica senza / con cavi (`cloneAction(shift)` + refresh lista) |
| `Tab` | PARAM / OUTPUT / INPUT | Cicla avanti tra le tre viste del modulo: PARAM→OUTPUT→INPUT→PARAM |
| `Shift+Tab` | PARAM / OUTPUT / INPUT | Cicla indietro: PARAM→INPUT→OUTPUT→PARAM |

## ✅ CRASH RIMOZIONE/DUPLICAZIONE MODULO — CAUSA RADICE TROVATA E RISOLTA (2026-06-04)

**Causa radice:** `AccessibleWindow::placeModule()` creava il modulo (`model->createModule()`) e il widget ma **non chiamava mai `APP->engine->addModule(m)`**. `RackWidget::addModule()` inserisce SOLO il widget in `moduleContainer`, NON registra il modulo nell'engine (confermato a `RackWidget.cpp:658`). Quindi ogni modulo piazzato dalla finestra accessibile non esisteva per l'engine.

Quando quel widget veniva distrutto — su Canc/Backspace, su Duplica (`cloneAction`→`prepareSaveModule`), o allo shutdown via `RackWidget::clear()` che distrugge tutti i widget — `~ModuleWidget` → `setModule(NULL)` → `Engine::removeModule()` faceva `assert(it != internal->modules.end())` (`Engine.cpp:801`, modulo non in lista) → SIGABRT. Per questo **anche un Alt+F4 normale segnalava un crash al riavvio**: bastava aver piazzato un modulo nella sessione.

**Fix applicata (`placeModule`, allineata al browser nativo `Browser.cpp:94-114`):**
```cpp
engine::Module* m = model->createModule();
APP->engine->addModule(m);                 // ← LO STEP MANCANTE (crash fix)
app::ModuleWidget* mw = model->createModuleWidget(m);
if (!mw) { APP->engine->removeModule(m); delete m; return; }
... rack->addModule(mw);
mw->loadTemplate();                        // preset di default come il nativo
history::ModuleAdd* ha = new history::ModuleAdd;  // undo dell'aggiunta
ha->setModule(mw); APP->history->push(ha);
```
Build cross-compile MinGW OK. **VERIFICATO DAL VIVO (2026-06-04):** niente più crash su Canc, Duplica (con/senza cavi), né Alt+F4/riavvio.

---

## (storico) Crash — diagnosi cavo/plug (2026-06-03)

`Engine::removeModule_NoLock` ha `assert(cable->inputModule != module)` / `outputModule`: **tutti i cavi devono essere scollegati PRIMA di rimuovere il modulo**. Fix corretto in `handlePortEnter`:
```cpp
CableWidget* cw = new CableWidget;
cw->color = rack->getNextCableColor();
cw->outputPort = outPortWidget;
cw->inputPort  = inPortWidget;
cw->updateCable();
rack->addCable(cw);
```

## Coda di comandi differiti — 2026-06-03

`AccessibleWindow` ha una coda `std::vector<std::function<void()>> commandQueue` con `pushCommand()` / `drainCommands()` e singleton `static AccessibleWindow* instance`. Tutte le mutazioni dell'albero sono **accodate**; `Window::step()` (sezione `ARCH_WIN`) chiama `instance->drainCommands()` a frame-boundary.

## Cavi (routing)

Usa `PendingCable` (struct interna): primo `Invio` su una porta imposta `active=true`; secondo `Invio` su porta di tipo opposto crea `engine::Cable` e `app::CableWidget` con il colore ciclico di Rack.

## Gestione focus e MSAA

- `WM_HOTKEY` handler → `Ctrl+Shift+A` fa il **toggle** del layer (`setLayerVisible`)
- `LVM_SETITEM` (via `lvSetSubtext`) emette automaticamente `EVENT_OBJECT_NAMECHANGE`

## Lazy rebuild per ridurre eventi MSAA

- **`rackDirty` flag**: `refreshRackView()` viene chiamata solo se il rack è cambiato
- **`lastParamModule`**: `repopulateParamView()` viene chiamata solo se `currentModule` è cambiato
- **Timer disabilitato per PARAM**: causava N `EVENT_OBJECT_NAMECHANGE` al secondo

## Barra dei menù nativa Win32 (aggiunto 2026-06-04, ✅ VERIFICATA DAL VIVO con NVDA)

**Voci:** File / Modifica / Vista / Motore / Libreria / Aiuto — replicano la barra di Rack come vera `HMENU` nativa con supporto MSAA di prima classe.

## Latenza NVDA — Fix (2026-06-03, CONFERMATO FUNZIONANTE)

In `src/window/Window.cpp`, sezione frame-limit di `Window::step()`: solo su `ARCH_WIN`, invece di `system::sleep(remaining)` il thread fa un loop `MsgWaitForMultipleObjects` + `PeekMessageW`/`DispatchMessageW` fino a esaurire il frame budget. Così le chiamate COM di NVDA sono servite quasi istantaneamente.

## ⚠️ TODO APERTO — Annuncio valore parametro "solo-valore"

Tre approcci falliti: `EVENT_OBJECT_VALUECHANGE`, NVDA Controller Client, `UiaRaiseNotificationEvent`. Attuale fallback: ri-fire `EVENT_OBJECT_FOCUS` → NVDA legge l'INTERA riga (verboso ma udibile).

## ✅ BUG RACK VIEW VUOTA ALL'AVVIO — RISOLTO (2026-06-04)

`AccessibleWindow::create()` spostato a **dopo** `patch->launch()` e `engine->startFallbackThread()`, prima di `window->run()`.

## Focus dopo eliminazione (commit 600ebcde, v2, 2026-06-07, ✅ VERIFICATO DAL VIVO)

`refreshRackView(focusModule=nullptr, int focusRowFallback=-1)`: se l'item precedente non si ritrova e `focusRowFallback>=0`, mette il focus su quell'indice clampato a `count-1`. Path di delete catturano `int row = lvFocused(listRack)` PRIMA della cancellazione e passano `row - 1`. Scelta utente: focus sul modulo PRECEDENTE.
