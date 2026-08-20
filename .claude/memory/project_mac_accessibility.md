---
name: project-mac-accessibility
description: "Piano per portare l'accessibilità di VCV Rack su macOS con VoiceOver — lavoro futuro"
metadata:
  node_type: memory
  type: project
  originSessionId: session-2026-06-09
---

Il passo successivo dopo il lavoro Win32/NVDA è **costruire un'interfaccia accessibile nativa per macOS**, navigabile con VoiceOver. Il lavoro verrà fatto su un Mac fisico per poter testare live.

**Why:** L'interfaccia Win32 usa HWND/ListView/TreeView + MSAA — tutte API solo Windows. Su Mac servono tecnologie native: NSAccessibility o AppKit con attributi di accessibilità, oppure un overlay Cocoa simile al layer Win32.

**How to apply:** Quando si inizia il lavoro Mac, il punto di partenza è `#if defined ARCH_MAC` in parallelo a `#if defined ARCH_WIN` nei file esistenti. L'architettura logica (viste RACK/LIBRARY/PARAM/OUTPUT/INPUT/CONTEXT_MENU, griglia 2D, shortcut) può restare invariata; cambia lo strato UI nativo.

**Memoria trasferita:** la memoria di progetto è ora nel repo a `.claude/memory/` (commit dd02a609, branch v2, push su metarack). Sul Mac, dopo il clone, basta chiedere a Claude Code di caricarla da lì.

## Scelta UI "solo interfaccia accessibile" rispecchiata su Mac (2026-06-27, CODICE FATTO, DA COMPILARE/TESTARE su Mac)

Portata su macOS la stessa decisione MetaRack già verificata su Windows (vedi [[project-accessible-window]]): interfaccia nativa Rack nascosta, sola finestra accessibile visibile, **niente più toggle ⇧⌘A**.

**Vincolo chiave macOS:** il panel accessibile era una **child window** del rackWindow (`addChildWindow:`). Una child window viene ordinata fuori insieme al genitore quando questo va in `orderOut` → impossibile nascondere Rack tenendo il panel. Soluzione = renderlo **top-level autonomo** (esatto parallelo della "top-level non posseduta" Win32).

**Modifiche (`src/accessible/AccessibleWindowMac.mm`):** `setLayerVisible`→`showLayer` (top-level via `makeKeyAndOrderFront` senza `addChildWindow`, mostrato incondizionatamente, ramo hide rimosso); voce di menù toggle ⇧⌘A rimossa dal menù Vista; `AXMenuTarget` ora anche `NSWindowDelegate` con `windowShouldClose:`→`APP->window->close()` (ritorna NO) = chiusura panel chiude l'app (analogo WM_CLOSE Win32); titolo panel `nsstr(APP_NAME)` (=MetaRack); `create()` chiama `showLayer` e setta il panel delegate=menuTarget; detach delegate nel distruttore. `settings::accessibleLayerVisible` resta orfano (come Win).

**`adapters/standalone.cpp` (ramo ARCH_MAC):** dopo `create()`, `glfwHideWindow(APP->window->win)` — la guardia `GLFW_VISIBLE` in `Window::step` salta il render, engine/widget tree restano vivi. `isVisible()` resta true → il pump eventi Mac in Window::step (reattività VoiceOver) continua.

**Da osservare dal vivo:** (1) il panel prende/mantiene il foco come unica finestra visibile; (2) chiusura dal pulsante finestra pulita; (3) audio gira con rackWindow nascosta.

## Tastiera del computer come MIDI keyboard (⇧K) portata su Mac (2026-06-27, CODICE FATTO, DA COMPILARE/TESTARE su Mac)

Portata su macOS la funzione Win32 (commit acd21760, vedi [[project-accessible-window]]): ⇧K attiva/disattiva una modalità in cui i tasti che mappano a una nota/ottava vanno al driver MIDI "Computer keyboard" di Rack (`keyboard::press/release`, GLFW key code), mentre ogni altro tasto continua a navigare.

**Differenza architetturale chiave vs Win32:** Win32 instrada le note dentro l'unico `ChildSubclassProc`. Su Mac le liste sono 5 `keyDown` separati (RackAX*TableView/OutlineView) e per giunta NSTableView **non consegna keyUp** alle sottoclassi. Soluzione = **un singolo `NSEvent addLocalMonitorForEventsMatchingMask` (keyDown|keyUp)** installato in `create()`, rimosso nel distruttore (`removeMonitor:`) — un solo hub, identico a Win32 concettualmente, e l'unico modo di ricevere keyUp. Handler ritorna `nil` per ingoiare l'evento, `e` per lasciar scorrere la navigazione keyDown.

**File:** `src/accessible/AccessibleWindowMac.mm`. Stato in `Internal`: `midiKeyboardMode`, `heldMidiKeys`, `keyMonitor`. Helper: `midiKeyForKeyCode` (virtual key code macOS kVK_* in hex → GLFW key code, posizionale, stesso set di Win32 `midiKeyForVk`), `toggleMidiKeyboard` (annuncia stato, rilascia note appese allo spegnimento), `midiKeyboardMonitor` (l'hub). Guardie: agisce solo se `visible` e `e.window==panel`, e **mai** se il firstResponder è un `NSText` (campo in editing → digitazione nei dialoghi valore/login intatta; i dialoghi sono comunque NSAlert app-modali con `e.window!=panel`). Ignora autorepeat (`isARepeat`).

**Manuali:** già documentano ⇧K dal commit Win (shortcut identico), nessuna modifica doc necessaria.
