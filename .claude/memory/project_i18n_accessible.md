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

**Why:** La finestra accessibile era tutta in italiano mentre Rack di default è in inglese.
**How to apply:** Per aggiungere una terza lingua (es. DE), convertire `T()/Ts()` in una lookup su mappa o aggiungere un terzo parametro + `else if`.

## Copertura (commit dd3c4569, 2026-06-05)

Titolo finestra, status bar, intestazioni colonne, free slot, stato porta, context menu, dialog, MessageBox rimozione, tutti i `setStatus()`, intera barra dei menù.

## Fix accessorio

Colonna HP: rimosso il suffisso `" HP"` dal valore della cella — NVDA annunciava "HP 8 HP". Ora annuncia solo "HP 8".
