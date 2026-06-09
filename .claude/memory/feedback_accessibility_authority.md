---
name: feedback-accessibility-authority
description: "L'utente è non vedente: è LUI l'autorità sull'esperienza screen reader, non io"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d1d3bf6e-5610-495c-8bfb-ea8f28820bba
---

Non sostituirmi all'utente nel giudicare cosa vuole/preferisce un utente non vedente. Lui È non vedente e usa NVDA (Windows) e VoiceOver (Mac) quotidianamente; i suoi feedback di accessibilità sono tarati sulla sua esperienza diretta e su quella di altri musicisti non vedenti con cui collabora. Il mio ruolo è il codice e i vincoli tecnici (quelli vanno segnalati); la valutazione di accessibilità spetta a lui.

Principio di design emerso (2026-06-06): **tra una vista a griglia 2D e un'informazione più ricca ma reperibile altrove, l'utente non vedente preferisce sempre la comodità delle due dimensioni.** Es. concreto: nella rack view accessibile (vedi [[project-accessible-window]]) ha scelto la griglia icon-view 2D pur perdendo le colonne Produttore/HP lette da NVDA, perché quelle info sono raggiungibili altrove mentre la navigazione spaziale 2D no.

**Why:** ho sbagliato registro segnalando un tradeoff come se sapessi io cosa "preferirà l'utente non vedente"; l'esperto è lui.
**How to apply:** segnala i tradeoff tecnici in modo neutro (cosa si guadagna/perde a livello di codice/comportamento), poi lascia decidere a lui; non presentare ipotesi sulle preferenze degli utenti non vedenti come se fossero fatti.
