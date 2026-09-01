// Interfaccia C++ pura verso il segnaposto NSView dell'editor VST3 (adapters/vst3_mac.mm).
// adapters/vst3.cpp è C++ semplice e non può vedere tipi Cocoa: qui passano solo void*.

#pragma once

namespace metarack {
namespace vst3mac {


// Dimensione del pannellino che l'host incorpora. Non è la dimensione della UI: la UI è il
// pannello accessibile, che è una finestra a sé.
constexpr int kWidth = 420;
constexpr int kHeight = 90;


// Crea il segnaposto dentro la view dell'host e lo restituisce (retained: va liberato con
// destroyPlaceholder). `text` è già localizzato. `onFocus` viene chiamata quando l'host
// consegna il focus al segnaposto, per rimbalzarlo sul pannello accessibile.
void* createPlaceholder(void* parentNSView, const char* text,
                        void (*onFocus)(void*), void* userData);

void destroyPlaceholder(void* placeholder);

void resizePlaceholder(void* placeholder, int x, int y, int w, int h);


} // namespace vst3mac
} // namespace metarack
