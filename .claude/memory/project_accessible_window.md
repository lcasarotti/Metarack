---
name: project-accessible-window
description: "Stato corrente dell'AccessibleWindow Win32 per VCV Rack — architettura, file, shortcut, TODO aperti"
metadata: 
  node_type: memory
  type: project
  originSessionId: 30607400-6056-4e6b-ba5a-64583aa47520
---

## Cos'è

Un'interfaccia di accessibilità completamente navigabile via tastiera e compatibile con screen reader (MSAA). **Testata su Windows con NVDA E Jaws** (non solo NVDA); supporta anche Narrator. Quando si scrive documentazione o si elencano gli screen reader supportati, citare **NVDA, Jaws, Narrator**. Disponibile solo su Windows (`#if defined ARCH_WIN`). **Dal 2026-06-25 è l'UNICA UI dell'app rebrandizzata MetaRack** (vedi sezione sotto): non più un layer a toggle, ma la finestra primaria.

## ⭐ MetaRack — app accessibile indipendente, niente più toggle (2026-06-25, ✅ VERIFICATO DAL VIVO "funziona tutto")

Accordo con gli sviluppatori VCV: distribuiamo un eseguibile **indipendente** (stessa licenza Rack Free/GPLv3) brandizzato **MetaRack**. Essendo un'app il cui scopo è *fornire* l'accessibilità, **si presenta SOLO la GUI accessibile** — rimosso il ciclo tra GUI originale e accessibile.

**Decisioni utente (AskUserQuestion 2026-06-25):** (1) finestra OpenGL di Rack **nascosta del tutto**; (2) branding **mirato** (nome visibile MetaRack, identificatori di compatibilità restano VCV).

**Fatto chiave verificato:** la cartella utente deriva da `"Rack" + APP_VERSION_MAJOR` (= `Rack2`, `src/asset.cpp:118`), **NON** da `APP_NAME` → rinominare `APP_NAME` non sposta plugin/impostazioni/login library.

**Modifiche (tutte buildate OK cross-compile MinGW):**
- `src/common.cpp:18`: `APP_NAME` `"VCV Rack"` → `"MetaRack"` (propaga a titolo, help, log, user-agent, status bar).
- `src/accessible/AccessibleWindow.cpp` `create()`: finestra **top-level non posseduta** (rimosso `WS_EX_TOOLWINDOW`, `hwndParent = nullptr`); titolo `L"MetaRack"`. `rackHwnd` resta assegnato ma non più letto (silenzia il param `owner`).
- Rimosso **del tutto** il toggle: handler `WM_HOTKEY`, `RegisterHotKey(Ctrl+Shift+A)`, `UnregisterHotKey`, e il metodo `setLayerVisible(bool)` (sia `.cpp` che dichiarazione in `.hpp`).
- `onCreate`: show incondizionato (`ShowWindow(SW_SHOW)` + `SetForegroundWindow` + `SetFocus(activeControl())`) invece di `if (settings::accessibleLayerVisible) setLayerVisible(true)`.
- `WM_CLOSE`: ora `APP->window->close()` → **chiude l'app** (prima `setLayerVisible(false)`). È l'unico modo di uscire insieme a File→Esci.
- `adapters/standalone.cpp`: dopo `AccessibleWindow::create()`, `glfwHideWindow(APP->window->win)` → la finestra GLFW non si vede mai; motore audio + widget tree restano vivi (il render è saltato da `if (visible)` in `Window::step()`), la finestra accessibile (ora non posseduta) resta visibile.
- `adapters/standalone.cpp:67` **mutex istanza singola tenuto a nome "VCV Rack"** (NON APP_NAME): MetaRack condivide la cartella `Rack2` con un eventuale VCV Rack reale → mutua esclusione per non corrompere l'autosave condiviso. Messaggio "already running" aggiornato via `APP_NAME`.
- `installer.nsi`: `NAME`/`NAME_FULL`/`RACK_DIR`/`INSTALL_REG`/`UNINSTALL_REG`/InstallDir → MetaRack. Binario resta `Rack.exe`; etichette file `.vcv/.vcvm/.vcvs`, URL API vcvrack.com, client audio/MIDI restano VCV (compat).
- `settings::accessibleLayerVisible` resta orfano su Windows (ancora usato da settings.cpp load/save e dal path Mac) — innocuo, non rimosso.

**macOS NON toccato:** `AccessibleWindowMac.mm` + blocco `ARCH_MAC` in standalone hanno ancora il loro toggle; il rebranding `APP_NAME` (cross-platform) li tocca già. Rispecchiare quando si farà VoiceOver.

**✅ VERIFICATO DAL VIVO (2026-06-25, "funziona tutto"):** avvio diretto su finestra "MetaRack" (nessuna finestra VCV in taskbar/Alt+Tab); NVDA annuncia "MetaRack"; Ctrl+Shift+A inerte; audio gira con GL window nascosta; X/Alt+F4 chiude pulito senza crash al riavvio.

## 🐛 Crash all'uscita (SIGSEGV in ~Engine) — fix fast-exit (2026-06-25, ✅ VERIFICATO DAL VIVO "chiusura e riavvio puliti")

**Sintomo:** dopo l'installazione pulita, alla chiusura di MetaRack il processo crasha; al riavvio scatta il dialog "Rack crashed" (perché il log non viene finalizzato).

**Diagnosi (dal log `%LOCALAPPDATA%/Rack2/log.txt`):** `Fatal signal 11` dentro `rack::engine::Engine::~Engine()` → `RtlDeleteCriticalSection`, durante `delete APP` (teardown del Context, fase "Deleting engine"), ~175 ms dopo. = distruzione di un mutex dell'Engine mentre un thread lo usa ancora.

**Isolamento del difetto (test fatti):**
- **Headless** (`Rack.exe --headless`, uscita con invio su stdin): teardown **PULITO**, log finalizzato con token `END`, exit 0. → il teardown **core** dell'Engine NON è il problema.
- In headless il flusso **WASAPI non viene aperto** (driver solo enumerati). Nel run windowed invece il modulo Audio apre il device WASAPI ed è il **master module** che pilota il motore.
- Conclusione: corsa al teardown del **thread audio WASAPI** col distruttore dell'Engine. RtAudio `closeStream` (WASAPI) può ritornare prima che il suo callback thread sia davvero fermo → quel thread sta ancora steppando il motore quando `~Engine` distrugge il mutex → segfault. È codice **core di Rack** (non toccato da noi); emerge ora perché l'utente esce e riavvia con audio attivo + libreria plugin piena.
- ⚠️ **Non riproducibile in automatico da qui:** il tool PowerShell gira non-interattivo, su desktop/window-station diverso → `FindWindowW` non vede le finestre GUI del processo lanciato, quindi non posso inviare WM_CLOSE per far ritornare `run()`. Verifica windowed solo dal vivo.

**Fix (`adapters/standalone.cpp`, ramo `#if defined ARCH_WIN` dopo `run()`):** invece di eseguire i distruttori che corrono col thread audio, si fa **clean fast-exit**: `APP->patch->saveAutosave()` + `settings::save()` (persistono stato) → `logger::destroy()` (scrive il token `END` che `wasTruncated()` cerca → niente falso dialog) → `TerminateProcess(GetCurrentProcess(), 0)`. `APP` è heap, distrutto solo dall'esplicito `delete APP` (che saltiamo) → l'Engine non viene mai distrutto, nessuna corsa. Lo stato è già flushato su disco (logger fa fflush/fclose, save fanno fclose). Headless e Mac/Lin **non** toccati (restano sul teardown ordinato, già pulito). Confidenza alta anche senza repro: il path non esegue il codice che crasha e usa lo stesso `logger::destroy()` che in headless ha prodotto `END`.

**✅ VERIFICATO DAL VIVO (2026-06-25):** reinstallato `dist/MetaRack-<ver>-win.exe`, chiusura e riavvio entrambi puliti, nessun dialog "crashed".

## (STORICO — superato da MetaRack) Layer posseduto da Rack + toggle Ctrl+Shift+A (commit ef26a521, v2, 2026-06-08, ✅ VERIFICATO DAL VIVO "funziona perfettamente")

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

## LIBRARY multi-pane: ricerca fuzzy + filtro per tag (2026-07-09, ✅ VERIFICATO DAL VIVO "funziona tutto come atteso"; commit 28970c97 pushato su metarack)

La vista LIBRARY, prima un solo `treeLibrary`, ora è **a tre pannelli impilati** mostrati insieme (albero in alto, campo di ricerca `searchLibrary` = `EDIT` una riga, `listTags` = ListView in basso). `switchView` non mostra più un solo HWND per LIBRARY ma il gruppo dei tre; `onSize` li impila (search ~24px, tags ~170px, tree il resto — cap tags a `listH/2`); `onCreate` chiama `onSize()` esplicito così il layout è corretto anche prima del primo WM_SIZE. `activeControl()` per LIBRARY resta `treeLibrary` (focus di default entrando con Shift+L).

**Tag di Rack riusati dal core:** i moduli SONO taggati nel codice (`include/tag.hpp`/`src/tag.cpp`, 56 tag canonici con alias; `plugin::Model::tagIds` = `std::list<int>`). `buildTagList()` popola `listTags` con riga 0 "Tutti i moduli"/"All modules" (`lParam=-1` = nessun filtro) + tutti e 56 i tag ordinati per nome tradotto (`string::translate("tag."+tag::getTag(i))`), `lParam=tagId`.

**Ricerca fuzzy** riusa lo stesso motore del browser nativo: `static fuzzysearch::Database<plugin::Model*> g_libraryDb` (`#include <FuzzySearchDatabase.hpp>`), `libraryDbInit()` clona `browser::modelDbInit` (pesi `{0.9,0.75,1.0,0.8,0.9}`, soglia `0.5`, campi brand/plugin/nome/descrizione/alias-tag). Costruita una volta alla prima entrata in LIBRARY.

**`rebuildLibraryTree()`** (refactor di `refreshLibraryView`, ora wrapper) ricostruisce l'albero applicando **entrambi** i filtri in AND: se `librarySearch` non vuota → `g_libraryDb.search()` → `set<Model*>` dei match; se `librarySelectedTag>=0` → solo modelli il cui `tagIds` contiene il tag. Con un filtro attivo espande i nodi brand (`TVE_EXPAND`); il conteggio va nella status bar **in modo silenzioso** (`SetWindowTextW` diretto, NON `setStatus`, per non parlare a ogni tasto). Membri nuovi (header): `std::string librarySearch`, `int librarySelectedTag=-1`.

**Aggancio live:** `WM_COMMAND`+`EN_CHANGE` su `ID_SEARCH` → aggiorna `librarySearch` e `rebuildLibraryTree` (albero non a fuoco → niente flood NVDA). `WM_NOTIFY`+`LVN_ITEMCHANGED` su `ID_TAGS` con nuova `LVIS_SELECTED` → aggiorna `librarySelectedTag` e ricostruisce. Nuovi id controllo `ID_SEARCH=107`, `ID_TAGS=108`; sottoclassi uid 6 (search) e 7 (tags).

**Tastiera:** `Ctrl+F` globale (blocco `if(ctrl)` in `ChildSubclassProc`) → `switchView(LIBRARY)` + focus su `searchLibrary` + `EM_SETSEL 0,-1` (stile Ableton). Il campo di ricerca è un **"santuario" di digitazione** (guard `hwnd==searchLibrary` in cima al proc, PRIMA di Shift+K/scorciatoie a lettera): gestisce solo `↓`/`Invio`→albero, `Tab`/`Shift+Tab`→ciclo, `Esc`→RACK, `Ctrl+F`→select-all; tutto il resto va a `DefSubclassProc` (così digitare "l"/"r"/"k" scrive e non naviga). Ingoia anche i `WM_CHAR` di Invio/Tab/Esc/LF per evitare il beep dell'edit. Ordine Tab dei tre pannelli: **albero → ricerca → tag** (gestito nel `case VK_TAB` per LIBRARY con array `{treeLibrary, searchLibrary, listTags}`; il search ha il suo ramo nel santuario). Invio su una riga tag → sposta all'albero (non seleziona un modulo). `focusLibraryPane(HWND)` fa SetFocus e, sull'albero senza selezione, seleziona la prima radice.

## Annuncio status bar — NVDA via Controller Client (2026-07-14, build OK, DLL da procurare)

**Problema:** NVDA non leggeva i messaggi della status bar (cavi connessi, MIDI keyboard, ecc.); Jaws e Narrator sì. **Causa radice:** la finestra è Win32 con controlli comuni → NVDA la pilota via MSAA/IAccessible, NON UIA. `setStatus()`/`onTimer()` annunciava solo con `UiaRaiseNotificationEvent` (UIA): la chiamata ritorna `S_OK` anche se nessuno ascolta → `ok=true` → il fallback MSAA (`EVENT_SYSTEM_ALERT` sullo STATIC `announcer`) non scattava mai. Jaws/Narrator ascoltano le notifiche UIA globalmente → OK; NVDA, non agganciato a UIA su questa finestra, restava muto.

**Fix (`src/accessible/AccessibleWindow.cpp`):** aggiunto caricamento dinamico del **`nvdaControllerClient` DLL** di NV Access (blocco `nvdaInit()`/`nvdaSpeak()` subito dopo `uiaInit()`, stesso stile lazy-load). `nvdaSpeak(text)`: `nvdaInit()` → se `nvdaController_testIfRunning()==0` (NVDA attivo) → `nvdaController_speakText(text)`. In `onTimer()`, PRIMA del ramo UIA: `bool ok = nvdaSpeak(...)`; se riesce si **salta del tutto** UIA (niente doppia lettura). Se NVDA non c'è → resta l'attuale UIA (Jaws/Narrator) + fallback MSAA. Ogni screen reader riceve il messaggio da UN solo canale.

- **Modalità INTERRUPT (scelta utente confermata dal vivo 2026-07-14):** `nvdaSpeak()` chiama `nvdaController_cancelSpeech()` PRIMA di `speakText()`, così il messaggio di stato interrompe la voce in corso e viene letto subito (l'utente: "importante che il messaggio di stato venga letto immediatamente"). Non più accodamento.
- Caricamento prova i nomi `nvdaControllerClient.dll` → `nvdaControllerClient64.dll` → `nvdaControllerClient32.dll`.
- ⚠️ **La DLL NON è nel repo né nell'installer di NVDA** — va procurata dal "Controller Client" di NV Access e messa accanto a `Rack.exe` (e nella dist, vedi [[reference-installer]]). Senza DLL: nessun crash, solo NVDA muto sulla status bar (UIA regge Jaws/Narrator).
- **DA VERIFICARE DAL VIVO:** con la DLL in `C:\Rack\` e NVDA attivo, connettere un cavo / attivare MIDI keyboard e sentire l'annuncio.

## Scorciatoie da tastiera

| Tasto | Contesto | Azione |
|---|---|---|
| `Shift+R` | Ovunque | Vai alla vista RACK; se già in RACK, forza refresh |
| `Shift+L` | Ovunque | Vai alla vista LIBRARY (focus sull'albero) |
| `Ctrl+F` | Ovunque | Apri LIBRARY e vai al campo di ricerca, selezionando il testo esistente (stile Ableton) |
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
| `F2` | RACK / PARAM / OUTPUT / INPUT | Vai a INPUT (dal RACK: modulo a fuoco; da una vista di dettaglio: `currentModule`) |
| `F3` | RACK / PARAM / OUTPUT / INPUT | Vai a OUTPUT (idem) |
| `F4` | RACK / PARAM / OUTPUT / INPUT | Vai a PARAM (idem) |
| `F5` | Ovunque tranne CONTEXT_MENU | Menu contestuale SPECIFICO del modulo (gemello di Ctrl+Tasto Applicazioni) |
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
| `Invio` | PARAM | Apre il dialog "Imposta valore" per il parametro focalizzato. **NB:** lo shortcut era `V`, poi spostato su `Invio` per non confliggere con la ricerca per iniziale (type-ahead) che funziona anche nella lista parametri. Il ramo `vk == 'V'` in `handleParamKey` (`src/accessible/AccessibleWindow.cpp:2533`) è ormai un residuo morto: la `V` semplice viene consumata dalla ListView per la type-ahead e non arriva mai a `handleParamKey`. |
| `Tasto Applicazioni` / `Shift+F10` | PARAM | Apre menu contestuale del parametro ("Imposta valore…" + azzera al default) |
| `Invio` | CONTEXT_MENU | Esegue la voce selezionata e torna alla vista precedente; se voce con `isSubmenu=true` → push nuovo livello (display menu nav) |
| `Esc` | CONTEXT_MENU | Se `menuStack.size()>1` → pop un livello; altrimenti chiude e torna a `previousView` |
| `Shift+D` | RACK / PARAM / OUTPUT / INPUT | Apre la lista dei display cliccabili del modulo corrente (era `D` nudo fino al 2026-08-20) |
| `Spazio` | Ovunque (quando `learningCell` attivo) | Toggle Tier B learn mode (seleziona/deseleziona la cella) |
| `Esc` | Ovunque (quando `learningCell` attivo) | Annulla il Tier B learn mode |
| `Ctrl+C` | RACK (su modulo) | Copia il preset del modulo negli appunti (`ModuleWidget::copyClipboard()`) |
| `Ctrl+V` | RACK (su modulo) | Incolla il preset copiato sul modulo a fuoco, in loco (`pasteClipboardAction()`) |
| `Ctrl+V` | RACK (su `[ Slot libero ]`) | Inserisce un NUOVO modulo dagli appunti in coda alla fila (`pasteModuleFromClipboard()`) |
| `Ctrl+D` / `Ctrl+Shift+D` | RACK (su modulo) | Duplica senza / con cavi (`cloneAction(shift)` + refresh lista) |
| `Tab` | PARAM / OUTPUT / INPUT | Cicla avanti tra le tre viste del modulo: PARAM→OUTPUT→INPUT→PARAM |
| `Shift+Tab` | PARAM / OUTPUT / INPUT | Cicla indietro: PARAM→INPUT→OUTPUT→PARAM |
| `Tab` / `Shift+Tab` | LIBRARY | Cicla i tre pannelli: albero → ricerca → tag (Shift inverte) |
| `↓` / `Invio` | LIBRARY (campo di ricerca) | Salta all'albero filtrato |
| `↑`/`↓` | LIBRARY (elenco tag) | Cambia il tag selezionato → filtra l'albero dal vivo ("Tutti i moduli" = nessun filtro) |

## ✅ CRASH RIMOZIONE/DUPLICAZIONE MODULO — CAUSA RADICE TROVATA E RISOLTA (2026-06-04)

**Causa radice:** `AccessibleWindow::placeModule()` creava il modulo (`model->createModule()`) e il widget ma **non chiamava mai `APP->engine->addModule(m)`**. `RackWidget::addModule()` inserisce SOLO il widget in `moduleContainer`, NON registra il modulo nell'engine (confermato a `RackWidget.cpp:658`). Quindi ogni modulo piazzato dalla finestra accessibile non esisteva per l'engine.

Quando quel widget veniva distrutto — su Canc/Backspace, su Duplica (`cloneAction`→`prepareSaveModule`), o allo shutdown via `RackWidget::clear()` che distrugge tutti i widget — `~ModuleWidget` → `setModule(NULL)` → `Engine::removeModule()` faceva `assert(it != internal->modules.end())` (`Engine.cpp:801`, modulo non in lista) → SIGABRT. Per questo **anche un Alt+F4 normale segnalava un crash al riavvio**: bastava aver piazzato un modulo nella sessione.

**Perché le ipotesi precedenti erano sbagliate:** lo stack di SHUTDOWN passa da `RackWidget::clear()` che chiama `clearCables()` PRIMA di rimuovere i moduli → niente cavi al momento dell'assert → NON poteva essere l'assert del cavo (820/821), era sempre il 801. Lo stack FRESCO interattivo (2026-06-04) confermava: `abort←wassert←Engine::removeModule←~ModuleWidget←~BraidsWidget←removeAction←lambda←drainCommands←Window::step`. (La deferred queue funziona; non era lei il problema.)

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

**Nota su autosave esistenti:** un modulo "senza engine" salvato in autosave viene risanato al reload perché il caricamento patch passa da `ModuleWidget::fromJson`→`Engine::moduleFromJson` che lo aggiunge all'engine. Non serve cancellare l'autosave.

---

## (storico) Crash — diagnosi cavo/plug (2026-06-03)

Causa ipotizzata via stack trace nel crash handler di Rack (`%LOCALAPPDATA%/Rack2/log.txt`, Rack linka `dbghelp` e scrive lo stack su signal fatale):

```
assert  ← Engine::removeModule  ← ModuleWidget::~ModuleWidget  ← ... (delete modulo o shutdown)
```

`Engine::removeModule_NoLock` ha `assert(cable->inputModule != module)` / `outputModule`: **tutti i cavi devono essere scollegati PRIMA di rimuovere il modulo**. La build di debug (`make run`, niente `-DNDEBUG`) ha gli assert attivi → abort.

**Perché i nostri cavi non venivano scollegati:** in questa versione di Rack le connessioni cavo↔porta sono tracciate da oggetti **`PlugWidget`** dentro `RackWidget::plugContainer`. `getCompleteCablesOnPort()` (usata da `ModuleWidget::disconnect()`/`appendDisconnectActions()` e da `clearCables()` allo shutdown) itera i **plug**, non i `CableWidget`. I plug vengono creati dal costruttore di `CableWidget` e registrati da `CableWidget::onAdd()` (chiamato da `addCable()` → `addChild`), e l'`engine::Cable` viene creato da `CableWidget::updateCable()` a partire dalle due `PortWidget`.

Il nostro vecchio codice creava a mano l'`engine::Cable` + `setCable()` → percorso diverso che lasciava l'engine con un cavo che `getCompleteCablesOnPort()` non trovava → mai scollegato → assert al `removeModule` (sia su Canc sia allo shutdown via `RackWidget::clear`).

**Fix corretto:** in `handlePortEnter` creare il cavo come fa Rack nativamente:
```cpp
CableWidget* cw = new CableWidget;
cw->color = rack->getNextCableColor();
cw->outputPort = outPortWidget;   // risolte da rack->getModule(id)->getOutput/getInput
cw->inputPort  = inPortWidget;
cw->updateCable();   // crea l'engine::Cable dalle due porte e lo aggiunge all'engine
rack->addCable(cw);  // onAdd() registra i PlugWidget nel plugContainer
```

## Coda di comandi differiti — 2026-06-03

`AccessibleWindow` ha una coda `std::vector<std::function<void()>> commandQueue` con `pushCommand()` / `drainCommands()` e singleton `static AccessibleWindow* instance`. Tutte le mutazioni dell'albero (rimozione modulo, `placeModule`, creazione/rimozione cavo) sono **accodate**; `Window::step()` (sezione `ARCH_WIN`, subito dopo `glfwPollEvents()`/`glfwMakeContextCurrent`) chiama `instance->drainCommands()` — stesso punto sicuro della gestione eventi nativa. Mantenuto come buona pratica (esegue le mutazioni a frame-boundary, non dal pump di idle), anche se non era la causa del crash. READ e cambi valore parametro restano inline. `MessageBoxW` di conferma resta inline (solo query).

## Cavi (routing)

Usa `PendingCable` (struct interna): primo `Invio` su una porta imposta `active=true`; secondo `Invio` su porta di tipo opposto crea `engine::Cable` e `app::CableWidget` con il colore ciclico di Rack.

## Gestione focus e MSAA

- All'avvio: `SetForegroundWindow(hwnd)` → la finestra prende il primo piano
- `WM_ACTIVATE` handler → `SetFocus(control attivo)` quando l'utente torna sulla finestra via Alt+Tab
- `WM_HOTKEY` handler → `Ctrl+Shift+A` fa il **toggle** del layer (`setLayerVisible`), non più "porta in foreground"
- Tutti i controlli hanno `WS_TABSTOP` per segnalare interattività agli screen reader
- `LVM_SETITEM` (via `lvSetSubtext`) emette automaticamente `EVENT_OBJECT_NAMECHANGE` — nessun `NotifyWinEvent` esplicito necessario

## Lazy rebuild per ridurre eventi MSAA

Il problema della latenza NVDA nasce dal flood di eventi MSAA durante i rebuild delle liste. Mitigazioni applicate:

- **`rackDirty` flag**: `refreshRackView()` viene chiamata solo se il rack è cambiato. Si setta `rackDirty=true` dopo `placeModule()`, `removeAction()`, e quando l'utente preme R da RACK (force refresh).
- **`lastParamModule`**: `repopulateParamView()` viene chiamata solo se `currentModule` è cambiato dall'ultimo rebuild.
- **Timer disabilitato per PARAM**: `refreshCurrentView()` è intenzionalmente vuota. Il timer da 100ms esisteva ma causava N `EVENT_OBJECT_NAMECHANGE` al secondo (uno per parametro), inondando la coda di NVDA.
- **WM_NOTIFY semplificato**: non chiama più `lvSetSubtext` sulla navigazione, evitando un evento duplicato per ogni freccia verticale in PARAM.
- **`NotifyWinEvent` rimosso da `handleParamKey`**: `LVM_SETITEM` lo emette già automaticamente.

## Barra dei menù nativa Win32 (aggiunto 2026-06-04, ✅ VERIFICATA DAL VIVO con NVDA)

Riproduce la barra dei menù di Rack (File/Edit/View/Engine/Library/Help, vedi https://vcvrack.com/manual/MenuBar) come **vera `HMENU` nativa** attaccata con `SetMenu(hwnd, menuBar)`. Scelta deliberata: Windows gestisce da solo Alt per entrare, ←/→ tra i menù, ↑/↓ tra le voci, Invio/Esc; e i menu nativi hanno supporto MSAA/UIA di prima classe → NVDA li legge (nome, ruolo, stato checkbox) senza codice di accessibilità custom. Era esattamente il workflow richiesto, ottenuto "gratis".

**Architettura (tutto in `AccessibleWindow.cpp`, sezione `// ── Menu bar ─`):**
- Registro azioni: `std::vector<MenuCmd> menuCmds` (`{action, checked}`), id comando = `MENU_CMD_BASE (2000) + indice`. Nessuna collisione con gli id controlli (101–106) o status bar (999).
- `addMenuCmd(h, label, action, checked=nullptr, flags=0)`: appende voce + registra azione, ritorna l'id.
- `buildMenuBar()`: costruita UNA volta in `onCreate()` PRIMA di `GetClientRect` (così `SetMenu` riduce il client e i controlli si dimensionano giusti). `onSize()` ha guard `if(!statusBar||!listRack) return` perché `SetMenu` può emettere WM_SIZE prima dei controlli.
- Dispatch: `WM_COMMAND` in `WndProc` → `menuCmds[id-BASE].action()`. L'azione decide inline vs `pushCommand()`.
- Stato dinamico: `WM_INITMENUPOPUP` → `refreshPopupChecks(popup)` setta i checkmark di OGNI voce del popup che si apre (toggle + preset radio) leggendo il getter `checked` (vale anche per i sottomenù, che ricevono il proprio WM_INITMENUPOPUP). I checkmark via `CheckMenuItem(MF_CHECKED/UNCHECKED)` (non MFT_RADIOCHECK; NVDA annuncia comunque "selezionato").
- Popup a **struttura variabile** ricostruiti on-open: `popupRecent` (File→Apri recenti, da `settings::recentPatchPaths`) e `popupLibrary` (login state via `library::isLoggedIn()`). `DeleteMenu` in loop + ri-`addMenuCmd` → i loro entry in `menuCmds` crescono di poco a ogni apertura (accettabile; gli id vecchi restano morti e innocui).

**Voci/azioni** (replicate da `src/app/MenuBar.cpp`):
- **File**: Nuovo/Apri/Apri recenti/Salva/Salva come/Salva copia/Ripristina/Sovrascrivi template/Importa selezione/Esci → `APP->patch->*` e `rack->loadSelectionDialog()`, `APP->window->close()`. Tutte in `pushCommand()`; quelle che cambiano il set di moduli chiamano `reloadRackAfterMutation()`.
- **Modifica**: Annulla/Ripristina (`APP->history->undo()/redo()` con guard `canUndo/canRedo` + reload), Scollega tutti i cavi, e le azioni di selezione (`rack->selectAll/deselectAll/copyClipboardSelection/pasteClipboardAction/saveSelectionDialog/resetSelectionAction/randomizeSelectionAction/disconnectSelectionAction/bypassSelectionAction`).
- **Vista**: Schermo intero (toggle); Zoom/Rapporto pixel/Tema/Rotellina/Opacità cavi/Tensione cavi/Luminosità stanza/Bagliore/Modalità manopole/Sensibilità rotellina come **sottomenù di preset radio** (gli slider continui non stanno in un menu nativo); toggle Tooltip/Blocca cursore/Scorrimento manopole/Blocca moduli/Comprimi moduli/Preferisci pannelli scuri. `uiTheme` setta + `pushCommand(ui::refreshTheme)`.
- **Motore**: Indicatore CPU (toggle), Frequenza campionamento (Auto+44.1/48/88.2/96/176.4/192 kHz), Thread (1..2×core, `getLogicalCoreCount()/2`).
- **Libreria**: se non loggato → "Registrati…" (browser) + **"Accedi…"** (dialog nativo con campi Email/Password, vedi sotto); se loggato → "Esci"/Account/Sfoglia/Aggiorna tutto + `checkUpdates()` in thread all'apertura.
- **Aiuto**: Lingua (sottomenù da `string::getLanguages()`, con prompt riavvio via `MessageBoxW`), Suggerimenti (`tipWindowCreate`), Manuale/Supporto/VCVRack.com, Cartella utente (`asset::user("")`), Changelog, Controlla aggiornamenti Rack.

**`reloadRackAfterMutation()`**: dopo patch load/undo/redo/paste/import chiama `cleanupCapturedMenu()`, azzera `currentModule`/`lastParamModule`/`pendingCable`, `rackDirty=true`, `switchView(RACK)` → evita puntatori penzolanti a moduli distrutti.

**✅ VERIFICATO DAL VIVO (2026-06-04):** Alt entra nella barra (il `WM_SYSKEYDOWN` cade correttamente in `DefSubclassProc` → attiva il menu; nessun inoltro esplicito necessario), navigazione frecce, annuncio voci/checkbox e azioni mutanti OK.

**Login Library — dialog Win32 nativo (2026-06-06, build OK, da verificare dal vivo):**
- Voce "Accedi…" nel popup Libreria (visibile solo quando non loggati) apre un `DialogBoxIndirectParamW` con 6 controlli: label Email + edit (id 1002), label Password + edit ES_PASSWORD (id 1004), pulsante "Accedi"/"Sign in" (IDOK), "Annulla"/"Cancel" (IDCANCEL). Dialog costruito in memoria con `buildLoginDlgTemplate()` come già fatto per "Imposta valore".
- Su OK: `library::logIn(email, password)` + `library::checkUpdates()` in thread separato (`std::thread::detach`); il thread al termine posta `WM_LOGIN_DONE` (= `WM_USER+1`, definito prima di WndProc) sulla finestra accessibile. WndProc gestisce `WM_LOGIN_DONE`: se `library::isLoggedIn()` → status "Signed in"; altrimenti mostra `library::loginStatus` (messaggio d'errore dal server, es. "invalid credentials").
- Lo stato del popup Libreria si aggiorna automaticamente alla prossima apertura (WM_INITMENUPOPUP → rebuildLibraryPopup → controlla isLoggedIn).

**TODO menu (minori):** editor Cable Colors; voce Help "Aggiorna Rack" condizionale a `isAppUpdateAvailable()` (ora c'è solo "Controlla aggiornamenti"); slider come valore continuo (ora solo preset).

## Stato alla data 2026-06-06 (aggiornato fine sessione)

**Funzionante:**
- Navigazione rack, libreria, parametri, porte
- Aggiunta moduli dalla libreria (rack si aggiorna immediatamente)
- Rimozione moduli con conferma (rack si aggiorna immediatamente)
- Creazione cavi
- Focus e lettura via NVDA (finestra in foreground all'avvio, WM_ACTIVATE gestito)
- Menu contestuali via App key / Shift+F10 (verificati dal vivo 2026-06-04)
- Duplicazione modulo da menu e da `Ctrl+D`/`Ctrl+Shift+D` (lista aggiornata correttamente — vedi bug risolto sotto)
- Copia/incolla preset modulo (`Ctrl+C`/`Ctrl+V`) e paste-as-new-module su slot libero (verificati dal vivo 2026-06-04)
- **Rack view popolata correttamente al lancio** (patch già caricata — vedi bug risolto sotto)
- **Imposta valore parametro via dialog** (menu contestuale PARAM → "Set value…", verificato dal vivo 2026-06-05)
- **Display cliccabili via tasto D** (commit 55297c6d, 2026-06-05, build OK — NON ancora verificato dal vivo con NVDA)
- **Menu contestuale specifico del modulo via Ctrl+Tasto Applicazioni** (commit 3cbdb814, 2026-06-05, build OK, VERIFICATO funzionante dall'utente — vedi sezione dedicata)
- **Localizzazione IT/EN** (commit dd3c4569, 2026-06-05): la finestra accessibile ora segue la lingua selezionata in Rack (EN di default, IT se impostato); vedi [[project-i18n-accessible]] per dettagli. Fix contestuale: colonna HP annuncia solo il numero (rimosso suffisso " HP" ridondante).
- **Tab/Shift+Tab cicla tra Parametri/Uscite/Ingressi** (commit d7f6db11, 2026-06-06): Tab avanza PARAM→OUTPUT→INPUT→PARAM, Shift+Tab inverte; NVDA annuncia il nome del contenitore ("Parametri/Uscite/Ingressi elenco") grazie al window text impostato sui tre ListView. VERIFICATO funzionante dall'utente.
- **Shortcut Rack/Library spostati a Ctrl+R / Ctrl+L** (commit faea0a74, 2026-06-06): le lettere semplici R/L entravano in conflitto con la type-ahead della ListView (es. parametro "Randomise" raggiungibile con R). Con Ctrl il conflitto è impossibile. VERIFICATO funzionante dall'utente.

**Display cliccabili — tasto D (aggiunto 2026-06-05, build OK, da verificare dal vivo):**
- `handleDisplayKey()` → `collectDisplayCells(mw)` (ricorsiva via `collectDisplayCellsRec`) → lista in CONTEXT_MENU con `menuStack[0]`.
- Attivazione cella → `openDisplayCell()`: spara `onAction` e controlla se `APP->scene->children.back()` è cambiato in un `ui::MenuOverlay` (Tier A) o no (Tier B learn).
- **Tier A**: `capturedOverlay` = overlay capturato, `buildItemsFromMenu(menu)` costruisce voci. Voci foglia: `mi->doAction(false)` + `cleanupCapturedMenu()`. Voci sottomenù (`isSubmenu=true`): lazy push `menuStack` + `ownedSubmenus` su Enter. Esc pops un livello.
- **Tier B**: `APP->event->setSelectedWidget(learningCell)`, polling in `onTimer`, Space toggle, Esc annulla.
- **Struttura dati chiave**: `menuStack` = stack di livelli di ContextMenuItem. `ownedSubmenus` = vettore di `ui::Menu*` creati con `createChildMenu()`. `capturedOverlay` = puntatore al MenuOverlay in `APP->scene`. Tutto azzerato da `cleanupCapturedMenu()`.
- **ContextMenuItem** ha ora un costruttore esplicito (C++11 non ammette brace-init su struct con std::function + default member initializer). Tutti i push_back esistenti usano `{label, lambda}` (2 arg, `isSubmenu` default `false`); nuovi display push_back usano `{label, lambda, true/false}`.
- `cleanupCapturedMenu()` è chiamata anche dai path di rimozione modulo (Elimina da context menu + handleRackKey VK_DELETE/BACK) e da `reloadRackAfterMutation()`.

**Dettaglio menu contestuali (aggiunto 2026-06-04, VERIFICATO DAL VIVO):**
- `WM_CONTEXTMENU` intercettato in `ChildSubclassProc` (copre sia `VK_APPS` che `Shift+F10`)
- Trigger in RACK view → `buildModuleContextMenu(mw)`: azzera / randomizza / disconnetti / bypass toggle / duplica (senza cavi) / duplica con cavi / elimina. Tutte le azioni su widget tree avvolte in `pushCommand()`.
- Trigger in PARAM view → `buildParamContextMenu(paramId)`: **"Imposta valore…"** (dialog modale Win32) + azzera al valore predefinito. Il dialog usa `DialogBoxIndirectParamW` con template DLGTEMPLATE costruito in memoria (no .rc); campo edit pre-compilato con valore corrente; su OK chiama `pq->setDisplayValueString(toUtf8(text))` che passa per tinyexpr → supporta tutte le espressioni del manuale (note, log2, dbtogain, vtof, ecc.). **VERIFICATO DAL VIVO 2026-06-05.**
- `Esc` torna a `previousView` (non sempre RACK); così da PARAM il menu si chiude tornando a PARAM.
- In CONTEXT_MENU view, `Ctrl+R` / `Ctrl+L` funzionano ancora come shortcuts globali (chiudono il menu e navigano alla vista richiesta).

**Menu contestuale SPECIFICO del modulo — Ctrl+Tasto Applicazioni (commit 3cbdb814, 2026-06-05, build OK, VERIFICATO funzionante dall'utente):**
Stile Reaper: contestuale distinto, separato dal generico (7 voci). Espone le voci che ogni modulo aggiunge tramite l'override `ModuleWidget::appendContextMenu(ui::Menu*)` (`include/app/ModuleWidget.hpp:78`, default vuoto). Es. MIDI-to-CV (`src/core/MIDI_CV.cpp:154`): Polyphony channels (1=Monophonic…16), Monophonic priority, Polyphony allocation (Rotate/Reuse/Reset/MPE), Pitch bend range, CLK/N divider, Reset MIDI (Panic). **Conferma utente: questi contestuali contengono tutto ciò che il manuale documenta e che non era esposto altrove.**
- **Meccanismo Rack**: `ModuleWidget::createContextMenu()` (`src/app/ModuleWidget.cpp:995`) costruisce il menù nativo completo = voci standard + `appendContextMenu(menu)` all'ultima riga (1125). NB: `model->appendContextMenu` a riga 1007 è un metodo DIVERSO su `plugin::Model`, solo per il sottomenù "Info" (link), non le opzioni operative.
- **Trigger**: `ChildSubclassProc` `WM_CONTEXTMENU` legge `GetKeyState(VK_CONTROL)`. Ctrl+Apps genera comunque `WM_CONTEXTMENU` (Ctrl premuto → `handleModuleSpecificContextMenuKey()`; libero → `handleContextMenuKey()` generico).
- **`handleModuleSpecificContextMenuKey()`**: risolve il `ModuleWidget` dalla riga RACK a fuoco, oppure da `currentModule` (via `rack->getModule(id)`) se in PARAM/OUTPUT/INPUT.
- **`buildModuleSpecificContextMenu(mw)`**: `cleanupCapturedMenu()` → crea `ui::Menu* extra = new ui::Menu` staccato → `mw->appendContextMenu(extra)` → `buildItemsFromMenu(extra)` (RIUSA il walker del tasto D: gestisce sottomenù via `createChildMenu()`, checkmark `✓`, right-arrow `▸`). Se vuoto: `delete extra` + status "Nessuna opzione specifica". Altrimenti `ownedRootMenu = extra` + `menuStack.push_back(items)` + `showContextMenu(items)`.
- **Lifetime**: nuovo membro `ownedRootMenu` (`AccessibleWindow.hpp`) tiene vivo il menù staccato mentre l'utente naviga (le lambda delle voci puntano ai figli di `extra`); liberato in `cleanupCapturedMenu()` accanto a `ownedSubmenus`/`capturedOverlay`. Navigazione sottomenù + Esc-pop identici al tasto D.
- Vedi [[reference-display-choice-mechanism]] per il walker condiviso.

**✅ BUG DUPLICAZIONE — lista RACK non aggiornata (RISOLTO e VERIFICATO 2026-06-04):**
Le voci "Duplica" del menu (e poi le shortcut `Ctrl+D`) chiamavano `cloneAction()` ma NON ricostruivano la `listRack`. La lista è ricostruita in modo lazy (solo se `rackDirty` è settato allo `switchView(RACK)`), e nel flusso del menu `switchView(previousView)` gira PRIMA di `action()` con `rackDirty=false` → il duplicato restava invisibile nella lista finché un altro rebuild (es. aggiunta da libreria → `placeModule`→`refreshRackView`) non lo mostrava (insieme al nuovo). Sintomo riportato: "il duplicato non appare finché non se ne aggiunge un altro dalla libreria". **Nota:** la vista OpenGL mostrava già il duplicato (ridisegna ogni frame); il bug era SOLO nella lista accessibile. **Fix:** dentro la `pushCommand` differita, dopo `cloneAction()`, chiamare `refreshRackView()` + `rackDirty=false` + `setStatus(...)` (stesso pattern di "Elimina"). Vale per menu e shortcut.

**Clipboard + duplicazione via tastiera (aggiunto 2026-06-04, VERIFICATO DAL VIVO):**
- Gestite in `ChildSubclassProc` (`WM_KEYDOWN`): calcola `ctrl`/`shift` con `GetKeyState(VK_CONTROL/VK_SHIFT) & 0x8000`; se `ctrl && currentView==RACK && wp∈{C,V,D}` → `handleRackCtrlKey(wp, shift)` e consuma. Senza Ctrl le lettere cadono nel type-ahead della ListView.
- `handleRackCtrlKey`: su slot libero solo `V` (paste-as-new); su riga modulo: `C`=copyClipboard, `V`=pasteClipboardAction (preset in loco), `D`=cloneAction(shift). Tutto in `pushCommand()`.
- `pasteModuleFromClipboard()` (nuovo): legge gli appunti via **API Win32** (`getClipboardTextUtf8()`, `OpenClipboard`/`CF_UNICODETEXT`) per NON tirare GLFW in questa TU — è lo stesso clipboard di sistema che GLFW usa su Windows, quindi interopera col Ctrl+C/V della GUI nativa. Poi `json_loads` → `jsonStripIds` → `plugin::modelFromJson` (in try/catch: lancia se plugin/model non installato) → `createModule` → `fromJson` PRIMA di `addModule` all'engine (niente lock necessario finché non è live, come `cloneAction`) → `createModuleWidget` → posiziona con `nextModulePos()` → `addModule` + `history::ModuleAdd` + `refreshRackView(mw)`.
- Helper statico `nextModulePos()` estratto da `placeModule()` (posizione subito a destra del modulo più a destra), ora condiviso tra `placeModule` e `pasteModuleFromClipboard`.

**Latenza NVDA (~1 secondo) — CAUSA RADICE TROVATA (2026-06-03):**

Le ipotesi precedenti (flood eventi MSAA, vsync, Browse Mode) erano sbagliate. La vera causa è la **condivisione del thread**:

- La finestra accessibile è creata sul **thread principale** (`standalone.cpp:249`), poi `APP->window->run()` prende quel thread con il render loop.
- I messaggi Win32 della finestra — e soprattutto le **chiamate COM/MSAA `IAccessible`** che NVDA fa per leggere nome/ruolo/valore dell'item a fuoco — vengono servite SOLO quando il thread principale pompa i messaggi, cioè dentro `glfwPollEvents()`, chiamato **una volta per frame** (`Window.cpp:441`).
- A ogni frame, dopo il render, `Window::step()` fa `system::sleep(remaining)` (frame-limit, default 30 fps → `settings.cpp:54`) **senza servire alcun messaggio**.
- Quando muovi il focus, NVDA fa **molte chiamate COM in sequenza** (name, role, state, value, location, parent…), e ognuna deve attendere il prossimo `glfwPollEvents`. Con quanto ~33 ms (o molto più se la patch è pesante e il rendering del frame è lento) → latenza ~1 s.
- Expand/collapse è immediato perché richiede pochissime query COM aggiuntive (lo stato arriva nell'evento). Tutto ciò che muove il *focus* scatena la raffica gated dal pump.

**Fix applicato (versione leggera, 2026-06-03) — CONFERMATO FUNZIONANTE dal vivo con NVDA:**
In `src/window/Window.cpp`, sezione frame-limit di `Window::step()`: solo su `ARCH_WIN`, invece di `system::sleep(remaining)` il thread ora fa un loop `MsgWaitForMultipleObjects(... QS_ALLINPUT)` + `PeekMessageW`/`DispatchMessageW` fino a esaurire il frame budget, salvando/ripristinando il `Context*` di Rack attorno (i callback GLFW possono cambiarlo) e rifacendo `glfwMakeContextCurrent(win)`. Così le chiamate COM di NVDA sono servite quasi istantaneamente durante l'idle invece di aspettare il frame successivo. Aggiunto `#include <windows.h>` sotto `ARCH_WIN` in cima al file. Build cross-compile OK. **Esito: la latenza è ora pari a quella di qualunque altra applicazione.** Da ri-verificare con patch complesse (l'utente non le ha ancora potute costruire: libreria moduli non ancora compilata).

**Se con patch complesse il fix leggero non bastasse** (es. rendering di un singolo frame intrinsecamente lento con patch grosse): passare al **thread dedicato** — la finestra accessibile gira su un proprio thread con message loop dedicato che non fa altro che pompare. Richiede però di marshalare le MUTAZIONI (placeModule/addModule/addCable/removeAction/setValue, che toccano i widget) verso il thread principale tramite una coda drenata nel run loop, perché toccare i widget da un altro thread può crashare. I READ per popolare le liste restano leggermente racy.

**Annuncio valore parametro (punto 2) — STATO 2026-06-04: tornati all'annuncio VERBOSO (ri-fire focus). Problema "solo-valore" ANCORA APERTO.**

Cronologia tentativi (cosa NON funziona, da non riprovare a vuoto):
- **Iter. 1 — verboso (`lvFocusRow` = re-fire `EVENT_OBJECT_FOCUS`):** NVDA legge l'INTERA riga "nome + valore". Udibile ma verboso. ← **è la versione attualmente in uso** (`handleParamKey` e reset di `buildParamContextMenu`).
- **Iter. 2 — `EVENT_OBJECT_VALUECHANGE` sull'item + NAMECHANGE sul subitem, senza ri-fire focus:** NVDA MUTO. Causa: una voce di `ListView` Win32 espone il testo colonne come *Name*, NON come *Value* (`accValue` vuoto) → VALUECHANGE annuncia valore vuoto = silenzio; il NAMECHANGE è su un subitem non a fuoco → ignorato.
- **Iter. 3 — NVDA Controller Client (`nvdaControllerClient64.dll`, `nvdaController_speakText`/`cancelSpeech`, caricata lazy via LoadLibraryW+GetProcAddress):** PROVATO DAL VIVO con la DLL presente accanto a Rack.exe → **NON ha funzionato** (nessun annuncio). Causa non ancora chiarita (possibili: la DLL non trovava NVDA / sessione/arch, oppure speakText falliva silenziosamente). Helper `announce()` RIMOSSO dal codice.

**Prossime piste da escogitare (TODO):** (a) UIA Notification — `UiaRaiseNotificationEvent` su un provider UIA dell'item (NVDA 2018+/JAWS/Narrator lo annunciano come live text, è la via "moderna" raccomandata, ma serve un `IRawElementProviderSimple`); (b) proxy `IAccessible` custom sull'item della ListView che esponga il valore come `accValue` reale, così `EVENT_OBJECT_VALUECHANGE` verrebbe letto; (c) debug del controller client (verificare ritorno di speakText, eventuale mismatch a 64 bit, log NVDA).

Per i cavi si USA `lvFocusRow` (`focusPortRow`) di proposito: lì serve leggere nome porta + stato connessione.

**Annuncio collegamento/scollegamento cavo (punti 3-4 risolti):** dopo connect/disconnect, il comando rifà `refreshPortView` e chiama `focusPortRow(isOutput, portId)` che ri-mette a fuoco la riga della porta → NVDA legge "NomePorta → ModuloRemoto" (o "libero"). Scollegamento via `handlePortDelete`: `getCompleteCablesOnPort` + `removeCable`+`delete` con `history::ComplexAction`/`CableRemove` per undo.

**Focus iniziale liste (punto 3 risolto):** entrando in PARAM/OUTPUT/INPUT, se nessun item è a fuoco `switchView()` mette il focus sull'item 0 (`lvFocusRow(lv, 0, false)` + `SetFocus`), così freccia giù va al secondo item. RACK già focalizzava riga 0.

**Focus dopo inserimento (punto 4 risolto):** `refreshRackView(app::ModuleWidget* focusModule)` accetta un modulo da focalizzare; `placeModule` passa il modulo appena inserito così il focus resta sulla sua riga invece di saltare allo slot libero.

**Focus dopo eliminazione (commit 600ebcde, v2, 2026-06-07, ✅ VERIFICATO DAL VIVO):** prima il focus saltava al primo modulo. Causa: `refreshRackView` ripristinava il focus cercando l'item col `lParam` (puntatore `ModuleWidget`) della riga prima a fuoco, ma quello era il modulo appena distrutto → nessun match → fallback a riga 0. Fix: nuovo 2° parametro `refreshRackView(focusModule=nullptr, int focusRowFallback=-1)`; se l'item precedente non si ritrova e `focusRowFallback>=0`, mette il focus su quell'**indice di riga** clampato a `count-1`. Entrambi i path di delete (tasto Canc/Backspace in `handleRackKey`; voce "Elimina"/"Delete" del menu contestuale) catturano `int row = lvFocused(listRack)` PRIMA della cancellazione e lo passano nella `pushCommand` → `refreshRackView(nullptr, row - 1)`. **Scelta utente (verificata dal vivo 2026-06-07): focus sul modulo PRECEDENTE all'eliminato (`row - 1`), non sul successivo.** Se si elimina il primo modulo (row 0) → `row - 1 = -1` disattiva il fallback → focus su riga 0 (il nuovo primo modulo), nessun "precedente". NB: VCV non ricompatta la fila (lascia il buco fisico), ma la lista accessibile usa slot sequenziali → gli indici sono contigui.

**⚠️ TODO APERTO — Annuncio messaggi di stato (setStatus) droppati da NVDA quando sta già parlando (2026-06-04):**

`setStatus(msg)` aggiorna la status bar visibile e accoda `pendingAnnouncement`. Il timer (`onTimer`, 200 ms, nel message pump) prova a notificare con `UiaRaiseNotificationEvent` (`NotificationKind_ActionCompleted=2`, `NotificationProcessing_ImportantMostRecent=1`) via `UIAutomationCore.dll` caricata lazy, con fallback `EVENT_SYSTEM_ALERT` sul controllo STATIC nascosto `announcer` (1×1 px, WS_VISIBLE, off-screen a -2,-2). Entrambi funzionano SOLO quando NVDA non sta già parlando. Se l'utente preme Invio mentre NVDA legge ancora il nome della porta, il messaggio (es. "Connesso", "Modulo aggiunto") viene droppato.

Tentativi falliti (da non ripetere):
- `EVENT_OBJECT_NAMECHANGE` sul STATIC → droppato (bassa priorità, NVDA scarta se coda piena)
- `EVENT_SYSTEM_ALERT` sul STATIC → stessa situazione
- `UiaRaiseNotificationEvent` con `ImportantMostRecent` → ancora droppato; NVDA non sembra rispettare la priorità quando è in speech

Piste ancora da provare:
- **nvdaController Client** (`nvdaControllerClient64.dll`, `nvdaController_speakText`) — provato in sessione precedente sul problema "solo-valore" parametro ma non ha funzionato (causa non chiara: possibile DLL non trovata o mismatch); ri-provare ora per lo use case diverso (messaggi di azione) con logging del codice di ritorno
- **Interrompere NVDA prima di annunciare** (`nvdaController_cancelSpeech`) poi parlare
- **`SetFocus` sull'`announcer` STATIC** durante l'annuncio (NVDA interrompe sempre su cambio focus), poi ripristino focus — il problema è che il ripristino causa un secondo annuncio indesiderato
- Qualsiasi altra API/pattern NVDA che garantisce coda non-droppable

**Altri punti aperti:**
- `libraryLoaded` non viene mai resettato: se si carica una nuova patch i plugin potrebbero non aggiornarsi
- La vista port mostra solo il PRIMO cavo nello stato (`cables[0]`), anche se `handlePortDelete` ora li scollega TUTTI
- Manca shortcut per salvare patch / undo globale
- ✅ **CRASH rimozione/duplicazione modulo: RISOLTO e VERIFICATO DAL VIVO (2026-06-04)** — mancava `APP->engine->addModule()` in `placeModule`; vedi sezione dedicata.
- Da verificare dal vivo con NVDA: annuncio valore parametro "solo-valore" (`EVENT_OBJECT_VALUECHANGE`); annuncio connect/disconnect cavo; reset parametro con Backspace; scollega cavo con Canc/Backspace

---

## ✅ BUG RACK VIEW VUOTA ALL'AVVIO — RISOLTO (2026-06-04)

**Sintomo:** all'avvio la rack view della finestra accessibile mostrava un solo slot libero come se la patch fosse vuota; la patch reale appariva solo aggiungendo un modulo dalla libreria.

**Causa radice:** in `adapters/standalone.cpp`, `AccessibleWindow::create()` veniva chiamata **prima** di `APP->patch->launch(patchPath)`. Quindi `onCreate()` → `refreshRackView()` trovava un rack vuoto. Dopo il `launch()`, `rackDirty` rimaneva `false` e nessun evento scatenava un refresh.

**Fix:** spostato il blocco `#if defined ARCH_WIN` (creazione AccessibleWindow) a **dopo** `patch->launch()` e `engine->startFallbackThread()`, ma ancora prima di `window->run()`. Il null-check `if (accessible::AccessibleWindow::instance)` in `Window.cpp:461` garantisce sicurezza durante il `launch()`.

**Why:** Progetto di accessibilità per utenti con disabilità visive che vogliono usare VCV Rack con screen reader su Windows.
**How to apply:** Prima di aggiungere funzionalità, verificare se la vista coinvolta richiede aggiornamento del dirty flag (`rackDirty`, `lastParamModule`) o se basta una chiamata esplicita a `switchView()`.


## Lettere liberate per la type-ahead — F2/F3/F4/F5 + Shift+D (2026-08-20)

Le scorciatoie a **lettera nuda** rubavano la ricerca per iniziale delle liste (premere "o" apriva
gli output *e* saltava al modulo che inizia per "o"). Spostate su tasti funzione, che non producono
carattere: **F2=INPUT, F3=OUTPUT, F4=PARAM** (`switchToDetailView`, gemella Mac `switchToDetailView`),
**F5=menu specifico del modulo**, **Shift+D=display cliccabili**. I tasti di vista restano
`Shift+R`/`Shift+L`. Ogni lettera è ora libera per la type-ahead in tutte le liste.

- **Win32**: un `WM_KEYDOWN` consumato lascia comunque in coda il `WM_CHAR` già tradotto da
  `TranslateMessage` → serve `swallowChar()` (flag `swallowNextChar`, drenato in `ChildSubclassProc`
  **dopo** il santuario del campo di ricerca) su ogni scorciatoia a lettera (Shift+D/R/L, Shift+K).
  Su macOS non serve: consumare `keyDown:` basta, il type-select non parte.
- **F2/F3/F4 funzionano anche DA una vista di dettaglio** (usano `currentModule`), quindi saltano
  direttamente tra PARAM/OUTPUT/INPUT senza il ciclo con Tab.
- **Parità Mac completata il 2026-08-20**: F2–F5 + Shift+D nelle tre table view, `Shift+R` aggiunto
  alla rack table (rebuild con `rackDirty`), e **rimossa la `v` nuda** nella param table (Invio apre
  già il dialog valore, come su Win32). File `.mm` non compilabile da Windows → build da fare sul Mac.
- ⚠️ **macOS**: su tastiere Apple F1–F5 sono tasti media salvo "Usa F1, F2 ecc. come tasti funzione
  standard" (o Fn premuto). Se dà fastidio, servono alternative con Cmd.
- **VST3**: nessun codice di scorciatoie proprio (la UI è la stessa `AccessibleWindow`) → allineare
  = **ricompilare**: il bundle porta una copia propria di `libRack.dll`. Fatto il 2026-08-20 con
  `make vst3dist` + copia in `%COMMONPROGRAMFILES%\VST3\Luca Casarotti\MetaRack.vst3`; gate
  `RackVst3Test.exe` verde da cartella neutra sul bundle installato.
- **Manuali aggiornati** (commit `e9992377`): tutti e quattro i `docs/*.html`. Nei due Mac c'è
  una nota apposita sui tasti funzione Apple (Fn, oppure *Impostazioni di Sistema → Tastiera →
  Abbreviazioni da tastiera → Tasti funzione*).
- ✅ **VERIFICATO DAL VIVO 2026-08-20 dall'utente: "funziona tutto"** — F2/F3/F4/F5 e Shift+D
  provati nel **VST3 dentro Reaper e Ableton Live** col bundle ricompilato. Conferma anche il
  dubbio che avevo lasciato aperto: la finestra MetaRack è top-level e tiene il focus, quindi i
  tasti funzione arrivano a noi e **non vengono intercettati dagli acceleratori della DAW**.
- Codice committato in `c020e6f9` (Win + Mac insieme).
