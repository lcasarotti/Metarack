# Memory Index — VCV Rack (C:\Rack)

- [User Profile](user_profile.md) — Learner in C++ and Rack internals; Italian-speaking; personal mods not for upstream; è non vedente e usa NVDA
- [Accessibility Authority](feedback_accessibility_authority.md) — L'utente è non vedente: l'autorità sull'esperienza screen reader è lui, non io; preferisce la griglia 2D all'info ricca reperibile altrove
- [Accessible Window — Implementation](project_accessible_window.md) — Win32 accessible UI, ora UNICA UI dell'app rebrandizzata MetaRack (niente più toggle Ctrl+Shift+A, finestra GLFW nascosta): architettura, shortcut, TODO aperti
- [Build Command Windows](feedback_build_command.md) — Cross-compilazione Windows da WSL Ubuntu: comando completo con tutte le variabili MinGW
- [Display Choice Mechanism](reference_display_choice_mechanism.md) — Come funzionano i display cliccabili dei moduli (Audio/MIDI/CC): due famiglie di LedDisplayChoice, menù vs learn
- [Accessible Display Plan](project_accessible_display_plan.md) — Piano a fasi per il tasto D che espone i display cliccabili nella finestra accessibile
- [Accessible Window — i18n](project_i18n_accessible.md) — Localizzazione IT/EN dell'AccessibleWindow: helper T()/Ts(), copertura completa, build OK 2026-06-05
- [GitHub Repo](reference_github_repo.md) — Fork: github.com/lcasarotti/Metarack, branch Screen-Reader-Accessibility, remote locale: metarack
- [Installer Build](reference_installer.md) — Come produrre il .exe NSIS della versione accessibile (ora v2.0, installer combinato standalone + VST3 nella cartella VST3 di sistema): build WSL → dist assembly → makensis
- [Mac Accessibility](project_mac_accessibility.md) — Prossimo passo: interfaccia VoiceOver su macOS; architettura logica invariata, strato UI nativo Cocoa/NSAccessibility
- [Memory in Repo](reference_memory_in_repo.md) — La memoria è nel repo a .claude/memory/ (commit dd02a609): come caricarla su nuove macchine
- [Plugin Adapter (VST3/CLAP)](project_plugin_adapter.md) — Core condiviso adapters/rackhost.cpp; CLAP validato, VST3 hand-written (travesty + stub): letto da NVDA in Reaper; packaging VST3 FATTO 2026-07-19 (bundle rilocabile MetaRack.vst3, vendor "Luca Casarotti", res/ dentro Contents/Resources, install in "VST3/Luca Casarotti"); prossimo: provarlo in un DAW vero
- [VST3 DAW Crashes](project_vst3_daw_crashes.md) — RISOLTO: crash rimozione/chiusura era use-after-free tearoff (fix identità-COM, commit 6a2278dd, verificato live 2026-07-19); anche l'espulsione ASIO al caricamento sparita col fix (probabile radice comune). Filone chiuso; breadcrumb diagnostici rimossi (commit e34019c8), crash handler tenuto in pianta stabile
- [Port Name Recovery](reference_port_name_recovery.md) — Porte "#N" senza nome: recuperate dai placement id dell'SVG del pannello (portSvgId); funziona per voxglitch, Erica Synths sfugge
