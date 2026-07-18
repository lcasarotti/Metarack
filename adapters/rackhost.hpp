// Core condiviso dagli adapter plugin (CLAP, VST3).
//
// Tutto ciò che serve per far vivere Rack dentro un DAW — init dei singleton di
// processo, il driver audio finto che aggancia l'engine, il Context per istanza e la
// finestra accessibile — sta qui. I file adapters/clap.cpp e adapters/vst3.cpp sono
// gusci sottili che traducono soltanto l'ABI del formato in queste chiamate.
//
// Note di design (valgono per entrambi i formati):
//   - APP (il Context di Rack) è THREAD-LOCAL (vedi src/context.cpp): ogni callback del
//     DAW può girare su un thread diverso, quindi va chiamato useContext(inst) all'inizio
//     di OGNI callback che tocca APP.
//   - I sottosistemi inizializzati da retainProcess() sono SINGLETON DI PROCESSO (asset,
//     settings, plugin, audio, midi...). La separazione process-vs-istanza completa è
//     rimandata all'audit multi-istanza.
//   - LIMITE MONO-ISTANZA: un solo Driver e un solo Device globali -> una sola istanza per
//     processo. Per il multi-istanza servirà un Device per istanza, instradato per deviceId.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <arch.hpp>
#include <context.hpp>
#include <engine/Module.hpp>

#if defined ARCH_WIN
	#include <windows.h>
#endif

namespace rack {
namespace accessible {
struct AccessibleWindow;
}
}


namespace rackhost {


// Canali audio esposti al DAW, in ingresso e in uscita. Sono i 16 del modulo Core
// Audio-16 di Rack. L'adapter CLAP li affetta in 8 bus stereo (uno per coppia), ma a
// rackhost interessa solo il totale: device, buffer interleaved e conversione planare lo
// leggono da qui, così i tre punti non possono più andare fuori sincrono.
constexpr int kNumChannels = 16;


// --- ciclo di vita del processo -------------------------------------------------------
//
// Refcounted: il lavoro vero avviene solo quando il contatore passa per zero, così un host
// che carica e scarica la libreria più volte (o entrambi i formati nello stesso processo)
// non reinizializza Rack. logName è il nome del file di log dentro asset::user(), diverso
// per formato ("clap-log.txt" / "vst3-log.txt") per non sovrascriverli a vicenda.
bool retainProcess(const char* logName);
void releaseProcess();


// --- istanza --------------------------------------------------------------------------

struct Instance {
	rack::Context* context = nullptr;          // il Context di Rack PER ISTANZA
	rack::engine::Module* audioModule = nullptr; // il modulo Core Audio (ponte verso il DAW)
	double sampleRate = 44100.0;
	uint32_t maxFrames = 0;
	bool activated = false;

	// Buffer interleaved per la conversione planare<->interleaved in process().
	// Preallocati in activate() (= max frames * 2 canali): mai allocare sul thread audio.
	std::vector<float> inInterleaved;
	std::vector<float> outInterleaved;

#if defined ARCH_WIN
	// La finestra accessibile top-level (unica UI di MetaRack). Creata da createInstance(),
	// mostrata/nascosta dall'adapter, distrutta da destroyInstance().
	rack::accessible::AccessibleWindow* accessibleWindow = nullptr;
#endif
};

// createInstance/destroyInstance vanno chiamate sul MAIN THREAD: costruiscono i
// sottosistemi GUI (glfwInit vuole il main thread). Entrambi i formati lo garantiscono
// per i rispettivi punti del ciclo di vita.
Instance* createInstance();
void destroyInstance(Instance* inst);

// Rende "corrente" il Context di questa istanza sul thread chiamante. Da chiamare
// all'inizio di ogni callback dell'host che tocca APP.
void useContext(Instance* inst);

bool activate(Instance* inst, double sampleRate, uint32_t maxFrames);
void deactivate(Instance* inst);

// CLAP e VST3 passano entrambi buffer PLANARI (canale -> array di frame), mentre
// audio::Device::processBuffer vuole buffer INTERLEAVED con stride: la conversione nei due
// sensi vive qui. `in`/`out` sono array di kNumChannels puntatori-per-canale; un singolo
// puntatore nullo (bus non collegato dall'host) vale silenzio per quel canale.
void processPlanar(Instance* inst, const float* const* in, uint32_t numIn,
                   float* const* out, uint32_t numOut, uint32_t frames);


// --- MIDI dal DAW ---------------------------------------------------------------------
//
// Simmetrico al ponte audio: processInit() registra un driver MIDI "DAW" con un solo
// InputDevice (mono-istanza). L'utente aggiunge un modulo Core MIDI nel rack e ne seleziona
// il device "DAW" dalla finestra accessibile; da lì in poi gli eventi che l'adapter riceve
// dall'host finiscono in quel modulo.
//
// `bytes`/`len` sono UN messaggio MIDI grezzo (status + dati). `sampleOffset` è l'offset in
// campioni dell'evento dentro il blocco corrente: il messaggio viene timestampato con
// engine->getFrame() + sampleOffset, così è sample-accurate. CHIAMARE PRIMA di
// processPlanar(): getFrame() vale allora il frame d'inizio blocco, e lo stepBlock() che
// processPlanar avvia consuma il messaggio nello stesso blocco.
void pushMidiMessage(Instance* inst, const uint8_t* bytes, int len, int32_t sampleOffset);

// Contatore di processo dei messaggi MIDI ricevuti dal DAW. Serve SOLO all'harness di test
// (vst3test) per verificare il ponte MIDI senza un modulo sottoscritto: pushMidiMessage lo
// incrementa comunque, anche quando nessun Input è collegato al device.
uint64_t debugMidiMessageCount();

#if defined ARCH_WIN
	// --- finestra accessibile -------------------------------------------------------------
	// La finestra è top-level e vive quanto l'istanza: queste sono un telecomando show/hide.
	bool guiShow(Instance* inst);
	bool guiHide(Instance* inst);
	bool guiGetSize(Instance* inst, uint32_t* width, uint32_t* height);
	// Rende hostWindow "owner" della nostra finestra, così MetaRack resta sopra l'editor del
	// DAW invece di finirci dietro.
	bool guiSetTransient(Instance* inst, HWND hostWindow);
#endif


} // namespace rackhost
