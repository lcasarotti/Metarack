---
name: project-i18n-accessible
description: "Localizzazione bilingue (IT/EN) dell'AccessibleWindow — meccanismo e stato"
metadata: 
  node_type: memory
  type: project
  originSessionId: 3080fc76-34de-409e-89d3-30550bc9e246
---

## Meccanismo

Due helper statici in `AccessibleWindow.cpp` subito dopo `toUtf8()`:

```cpp
static const wchar_t* T(const wchar_t* en, const wchar_t* it) {
    return settings::language == "it" ? it : en;
}
static std::string Ts(const char* en, const char* it) {
    return settings::language == "it" ? it : en;
}
```

- `T()` per literal wide usati nelle API Win32 (titolo, colonne, menu, dialog).
- `Ts()` per `std::string` usati in `setStatus()`.
- Fallback inglese per qualsiasi lingua diversa da `"it"`.
- Lettura di `settings::language` a ogni chiamata; praticamene fissa per sessione perché il menu Lingua chiede riavvio.

**Why:** La finestra accessibile era tutta in italiano mentre Rack di default è in inglese — discrepanza con la lingua selezionata dalla barra menù.

**How to apply:** Per aggiungere una terza lingua (es. DE), convertire `T()/Ts()` in una lookup su mappa o aggiungere un terzo parametro + `else if`.

## Copertura (commit dd3c4569, 2026-06-05)

- Titolo finestra, status bar iniziale
- Intestazioni colonne: Module/Parameter/Value/State/Action
- Free slot: `"[ Free slot ]"` / `"[ Slot libero ]"`
- Stato porta non connessa: `"free"` / `"libero"`; nome porta di fallback: `"Port N"` / `"Porta N"`
- Context menu modulo e parametro, dialog "Set value", MessageBox rimozione
- Tutti i `setStatus()`: aggiunta/rimozione moduli, cavi, clipboard, apprendimento, ecc.
- Intera barra dei menù: File/Edit/View/Engine/Library/Help con tutte le voci

## Fix accessorio nello stesso commit

Colonna HP della rack view: rimosso il suffisso `" HP"` dal valore della cella — NVDA annunciava "HP 8 HP" (intestazione + valore). Ora annuncia solo "HP 8".

## Stato 2026-06-05

Committato, build MinGW OK. NON ancora verificato dal vivo (richiede avvio Rack con EN poi con IT).
