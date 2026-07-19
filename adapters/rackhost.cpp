// Core condiviso dagli adapter plugin (CLAP, VST3) — vedi rackhost.hpp.
//
// Il grosso di questo file è stato estratto da adapters/clap.cpp, dove era nato insieme
// all'adapter CLAP; è codice indipendente dal formato di plugin.

#include "rackhost.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#include <common.hpp>
#include <logger.hpp>
#include <math.hpp>
#include <random.hpp>
#include <asset.hpp>
#include <settings.hpp>
#include <system.hpp>
#include <string.hpp>
#include <network.hpp>
#include <audio.hpp>
#include <midi.hpp>
#include <midiloopback.hpp>
#include <plugin.hpp>
#include <plugin/Model.hpp>
#include <engine/Engine.hpp>
#include <engine/Cable.hpp>
#include <app/common.hpp>
#include <app/Scene.hpp>
#include <widget/event.hpp>
#include <patch.hpp>
#include <history.hpp>
#include <library.hpp>
#include <app/Browser.hpp>

// GUI / accessibility path. L'interfaccia accessibile è Win32-only e legge l'albero dei
// widget (RackWidget / ModuleWidget), quindi costruirla richiede una vera window::Window
// (il suo contesto nanovg rasterizza i pannelli dei moduli) anche se quella finestra GL
// resta nascosta e non viene mai disegnata.
#if defined ARCH_WIN
	#include <ui/common.hpp>
	#include <window/Window.hpp>
	#include <app/RackWidget.hpp>
	#include <app/ModuleWidget.hpp>
	#include <app/CableWidget.hpp>
	#include <app/PortWidget.hpp>
	#include <accessible/AccessibleWindow.hpp>
#endif

#include <jansson.h>

#if defined ARCH_WIN
	#define GLFW_EXPOSE_NATIVE_WIN32
	#include <GLFW/glfw3.h>
	#include <GLFW/glfw3native.h> // glfwGetWin32Window() — HWND passato ad AccessibleWindow::create()
#endif

using namespace rack;


namespace rackhost {

namespace {


// =====================================================================================
// Driver audio "DAW"
// =====================================================================================
//
// Rack instrada l'audio tramite audio::Driver -> audio::Device -> audio::Port. Il modulo
// Core Audio possiede un Port; quando il suo Device viene pompato, il Port (se master)
// chiama engine->stepBlock(). Noi non implementiamo I/O nel driver: è un contenitore
// passivo. È il process() dell'adapter a chiamare device->processBuffer() coi buffer del
// DAW, esattamente come fa il callback di rtaudio (src/rtaudio.cpp).

const int DAW_DRIVER_ID = 0x44415721; // "DAW!" — qualunque id stabile != -1

struct DawDevice : audio::Device {
	double sampleRate = 44100.0;
	int blockSize = 512;
	int numInputs = kNumChannels;
	int numOutputs = kNumChannels;

	std::string getName() override {
		return "DAW";
	}
	int getNumInputs() override {
		return numInputs;
	}
	int getNumOutputs() override {
		return numOutputs;
	}
	std::set<float> getSampleRates() override {
		return { (float) sampleRate };
	}
	float getSampleRate() override {
		return (float) sampleRate;
	}
	void setSampleRate(float sr) override {
		sampleRate = sr;
	}
	std::set<int> getBlockSizes() override {
		return { blockSize };
	}
	int getBlockSize() override {
		return blockSize;
	}
	void setBlockSize(int bs) override {
		blockSize = bs;
	}
};

struct DawDriver : audio::Driver {
	DawDevice device;

	std::string getName() override {
		return "DAW";
	}
	std::vector<int> getDeviceIds() override {
		return { 0 };
	}
	int getDefaultDeviceId() override {
		return 0;
	}
	std::string getDeviceName(int) override {
		return "DAW";
	}
	int getDeviceNumInputs(int) override {
		return device.numInputs;
	}
	int getDeviceNumOutputs(int) override {
		return device.numOutputs;
	}
	audio::Device* subscribe(int /*deviceId*/, audio::Port* port) override {
		device.subscribe(port); // base Device::subscribe: aggiunge al set di Port
		return &device;
	}
	void unsubscribe(int /*deviceId*/, audio::Port* port) override {
		device.unsubscribe(port);
	}
};

DawDriver* g_dawDriver = nullptr;


// =====================================================================================
// Driver MIDI "DAW"
// =====================================================================================
//
// Gemello MIDI del driver audio qui sopra. Un solo InputDevice (id 0, nome "DAW"): l'utente
// lo seleziona da un modulo Core MIDI nella finestra accessibile e, quando l'adapter riceve
// eventi dall'host, pushMidiMessage() li inoltra a questo device -> ai moduli sottoscritti.
// Solo INPUT (DAW -> Rack): niente device di output, la direzione opposta non serve al task.
// Come l'audio è MONO-ISTANZA: un unico device globale per l'intero processo.

const int DAW_MIDI_DRIVER_ID = 0x444D4921; // "DMI!" — id stabile, distinto da DAW audio e loopback

struct DawMidiDevice : midi::InputDevice {
	std::string getName() override {
		return "DAW";
	}
};

struct DawMidiDriver : midi::Driver {
	DawMidiDevice device;

	std::string getName() override {
		return "DAW";
	}
	std::vector<int> getInputDeviceIds() override {
		return { 0 };
	}
	int getDefaultInputDeviceId() override {
		return 0;
	}
	std::string getInputDeviceName(int deviceId) override {
		return (deviceId == 0) ? "DAW" : "";
	}
	midi::InputDevice* subscribeInput(int deviceId, midi::Input* input) override {
		if (deviceId != 0)
			return nullptr;
		device.subscribe(input); // base InputDevice::subscribe: aggiunge al set di Input
		return &device;
	}
	void unsubscribeInput(int deviceId, midi::Input* input) override {
		if (deviceId != 0)
			return;
		device.unsubscribe(input);
	}
	// Nessun device di output: getOutputDeviceIds() eredita il default vuoto della base.
};

DawMidiDriver* g_dawMidiDriver = nullptr;

// Contatore dei messaggi MIDI ricevuti dal DAW (vedi debugMidiMessageCount in rackhost.hpp).
// Atomico perché letto dall'harness di test mentre il thread audio lo incrementa.
std::atomic<uint64_t> g_dawMidiCount{0};


// =====================================================================================
// Inizializzazione "una volta per processo"
// =====================================================================================

std::mutex g_initMutex;
int g_initCount = 0;
bool g_initOk = false;

#if defined ARCH_WIN
	// I sottosistemi GUI (ui::init/window::init = GLFW + contesto GL + font) sono singleton
	// DI PROCESSO ma vanno inizializzati sul MAIN THREAD (glfwInit), cosa che il caricamento
	// della libreria non garantisce. Li facciamo quindi alla prima createInstance() (che gli
	// adapter chiamano dal main thread) e li distruggiamo all'ultima destroyInstance().
	bool g_guiSubsystemsInited = false;
#endif


// Ricava la cartella in cui vive QUESTA libreria, per puntarci asset::systemDir.
// Quando il DAW carica il plugin, GetModuleFileNameW(NULL) restituirebbe la cartella
// dell'EXE host (es. reaper.exe), non la nostra: res/ e i plugin Core non si
// troverebbero. Risolviamo prendendo il modulo che contiene questa funzione.
std::string getModuleDir() {
#if defined ARCH_WIN
	HMODULE hm = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCWSTR>(&getModuleDir), &hm);
	wchar_t bufW[MAX_PATH] = L"";
	GetModuleFileNameW(hm, bufW, MAX_PATH);
	std::string path = string::UTF16toUTF8(bufW);
	size_t slash = path.find_last_of("/\\");
	return (slash == std::string::npos) ? path : path.substr(0, slash);
#else
	return system::getWorkingDirectory();
#endif
}


// La radice dell'albero Rack: la cartella che contiene res/. Di solito è la cartella del
// modulo stesso (il .clap accanto a res/), ma un bundle VST3 mette la DLL in
// Contents/x86_64-win/, qualche livello sotto. Risaliamo finché non troviamo res/.
std::string findSystemDir() {
	std::string dir = getModuleDir();
	for (int i = 0; i < 4; i++) {
		if (system::isDirectory(dir + "/res"))
			return dir;
		size_t slash = dir.find_last_of("/\\");
		if (slash == std::string::npos)
			break;
		dir = dir.substr(0, slash);
	}
	// Nessun res/ trovato: torna alla cartella del modulo e lascia che il chiamante avvisi.
	return getModuleDir();
}


// Configura la porta audio di un modulo Core Audio perché usi il nostro driver "DAW".
// (Core dataFromJson: chiave "audio" -> audio::Port::fromJson.) Sottoscrivendosi al
// DawDevice la porta diventa master e fa girare l'engine quando process() pompa il device.
void bindAudioModuleToDaw(engine::Module* audioModule, double sampleRate, int blockSize) {
	json_t* dataJ = json_object();
	json_t* audioJ = json_object();
	json_object_set_new(audioJ, "driver", json_integer(DAW_DRIVER_ID));
	json_object_set_new(audioJ, "deviceName", json_string("DAW"));
	json_object_set_new(audioJ, "sampleRate", json_real(sampleRate));
	json_object_set_new(audioJ, "blockSize", json_integer(blockSize));
	json_object_set_new(dataJ, "audio", audioJ);
	audioModule->dataFromJson(dataJ);
	json_decref(dataJ);
}


#if !defined ARCH_WIN
// Cavi di loopback dalle uscite del modulo Audio ai suoi stessi ingressi: DAW in -> DAW out.
// Versione bare-engine, per il percorso headless dove non esiste un albero di widget.
void addPassthroughCablesHeadless(engine::Module* audioModule) {
	for (int i = 0; i < kNumChannels; i++) {
		engine::Cable* cable = new engine::Cable;
		cable->outputModule = audioModule;
		cable->outputId = i;
		cable->inputModule = audioModule;
		cable->inputId = i;
		APP->engine->addCable(cable);
	}
}
#endif // !ARCH_WIN


#if defined ARCH_WIN
// Variabile d'ambiente impostata dagli harness da console (claptest / vst3test) per
// ottenere un grafo pass-through verificabile anche su Windows, dove il rack di default è
// vuoto. Mai impostata da un DAW: in produzione questo ramo non si attiva.
// Letta con GetEnvironmentVariable e non con getenv: la CRT tiene una PROPRIA copia
// dell'ambiente, popolata al suo init, che SetEnvironmentVariable non aggiorna.
const char* const TEST_PASSTHROUGH_ENV = "METARACK_TEST_PASSTHROUGH";

bool testPassthroughRequested() {
	return GetEnvironmentVariableA(TEST_PASSTHROUGH_ENV, nullptr, 0) > 0;
}

// Loopback DAW in -> DAW out quando ESISTE l'albero dei widget. Va costruito come lo
// costruisce Rack nativamente: si impostano le due PortWidget sulla CableWidget e si lascia
// che updateCable() crei il cavo dell'engine, poi addCable() perché onAdd() registri i
// plug. Fabbricare a mano l'engine::Cable (come fa il ramo headless) qui salta i plug: il
// cavo non viene trovato da getCompleteCablesOnPort(), non viene mai scollegato alla
// rimozione del modulo, e fa asserire Engine::removeModule allo shutdown. Stessa lezione
// documentata in AccessibleWindow.cpp:3080.
void addPassthroughCablesWithWidgets(engine::Module* audioModule) {
	app::RackWidget* rack = APP->scene->rack;
	app::ModuleWidget* mw = rack->getModule(audioModule->id);
	if (!mw)
		return;
	for (int i = 0; i < kNumChannels; i++) {
		app::PortWidget* outPort = mw->getOutput(i);
		app::PortWidget* inPort = mw->getInput(i);
		if (!outPort || !inPort)
			continue;
		app::CableWidget* cw = new app::CableWidget;
		cw->color = rack->getNextCableColor();
		cw->outputPort = outPort;
		cw->inputPort = inPort;
		cw->updateCable();   // crea il cavo dell'engine dalle due porte
		rack->addCable(cw);  // onAdd() registra i plug
	}
}

// Inserisce nel rack un modulo Core Audio-2 (il ponte verso il DAW) CON il suo
// ModuleWidget, così compare nella rack view accessibile e l'utente può instradarci i
// propri moduli. Mutua l'ordine critico di AccessibleWindow::placeModule: creare il
// modulo -> engine->addModule() PRIMA del widget -> createModuleWidget -> rack->addModule.
// Saltare engine->addModule farebbe asserire l'engine quando il widget viene distrutto.
void addDawAudioModule(Instance* inst) {
	plugin::Model* audioModel = plugin::getModel("Core", "AudioInterface16");
	if (!audioModel) {
		WARN("Modello Core/AudioInterface16 non trovato: nessun ponte audio (Core caricato?)");
		return;
	}
	engine::Module* m = audioModel->createModule();
	if (!m)
		return;
	APP->engine->addModule(m);   // PRIMA del widget

	app::ModuleWidget* mw = audioModel->createModuleWidget(m);
	if (!mw) {
		APP->engine->removeModule(m);
		delete m;
		return;
	}
	APP->scene->rack->setModulePosNearest(mw, math::Vec(0, 0));
	APP->scene->rack->addModule(mw);
	inst->audioModule = m;

	// Lega la porta al DAW DOPO l'add (dataFromJson opera sul modulo engine).
	bindAudioModuleToDaw(m, inst->sampleRate, (int) inst->maxFrames);
	INFO("Modulo Audio DAW inserito con widget (id=%lld)", (long long) m->id);

	if (testPassthroughRequested()) {
		addPassthroughCablesWithWidgets(m);
		INFO("%s impostata: aggiunti i cavi di pass-through per l'harness di test",
		     TEST_PASSTHROUGH_ENV);
	}
}
#endif // ARCH_WIN


#if defined ARCH_WIN
// --- logger dei crash -----------------------------------------------------------------
// Logga modulo+offset del punto di crash e uno stack minimale per qualunque eccezione non
// gestita, su qualunque thread. Localizza i crash dentro il DAW senza un debugger: il modulo
// che fa fault (nostro / libRack / driver GL Intel / driver ASIO) dice subito dov'è il
// problema. Ritorna EXCEPTION_CONTINUE_SEARCH, quindi NON altera il comportamento del crash:
// è a costo zero e lo teniamo in pianta stabile come rete di sicurezza per i bug futuri.
extern "C" USHORT WINAPI RtlCaptureStackBackTrace(ULONG framesToSkip, ULONG framesToCapture,
    PVOID* backTrace, PULONG backTraceHash);

void logAddressModule(const char* tag, void* addr) {
	HMODULE mod = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCWSTR) addr, &mod) && mod) {
		wchar_t nameW[MAX_PATH] = L"";
		GetModuleFileNameW(mod, nameW, MAX_PATH);
		std::string name = string::UTF16toUTF8(nameW);
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos)
			name = name.substr(slash + 1);
		WARN("  %s %s+0x%llx (addr=%p)", tag, name.c_str(),
		     (unsigned long long)((uintptr_t) addr - (uintptr_t) mod), addr);
	}
	else {
		WARN("  %s <modulo sconosciuto> addr=%p", tag, addr);
	}
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
	WARN("=== CRASH: eccezione 0x%08lx sul thread %lu ===",
	     (unsigned long) info->ExceptionRecord->ExceptionCode,
	     (unsigned long) GetCurrentThreadId());
	logAddressModule("fault ", info->ExceptionRecord->ExceptionAddress);
	void* frames[24];
	USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
	for (USHORT i = 0; i < n; i++)
		logAddressModule("stack ", frames[i]);
	return EXCEPTION_CONTINUE_SEARCH; // lascia proseguire la normale gestione del crash
}
#endif


// Mutua adapters/standalone.cpp:main() FINO A (escluso) il contextSet: tutto ciò che
// è singleton di processo.
bool processInit(const char* logName) {
#if defined ARCH_WIN
	// Costruiamo la GUI accessibile, quindi NON siamo headless. La finestra GLFW/OpenGL di
	// Rack viene comunque tenuta nascosta (vedi createInstance): serve solo il suo contesto
	// nanovg per rasterizzare i pannelli SVG dei moduli, mai per disegnare.
	settings::headless = false;
#else
	// Fuori da Windows non c'è ancora un layer accessibile: resta la modalità headless
	// bare-engine.
	settings::headless = true;
#endif

	system::init();
	system::resetFpuFlags();

	// systemDir = la radice dell'albero Rack (dove vivono res/, Core.json, Fundamental),
	// cercata risalendo dalla cartella del modulo.
	// userDir = la stessa cartella: in sviluppo l'albero Rack è in C:\Rack (dove il plugin
	// viene buildato) e lì stanno settings.json / plugins-win-x64 / log. Così l'adapter
	// condivide l'ambiente col Rack-da-sorgente.
	// NB packaging (fase successiva): per un plugin installato, userDir andrà puntato
	// ad AppData/Local/Rack2 per condividere la libreria col Rack standalone installato.
	const std::string systemDir = findSystemDir();
	asset::systemDir = systemDir;
	asset::userDir = systemDir;
	asset::init();

	// Log separato da quello dello standalone (e degli altri formati), per non
	// sovrascriverli a vicenda.
	logger::logPath = asset::user(logName);
	logger::init();
	random::init();

#if defined ARCH_WIN
	// Installa il logger dei crash appena il logger di Rack è pronto (vedi crashHandler).
	SetUnhandledExceptionFilter(crashHandler);
#endif

	INFO("=== Metarack plugin adapter — process init ===");
	INFO("%s", system::getOperatingSystemInfo().c_str());
	INFO("systemDir: %s", asset::systemDir.c_str());
	INFO("userDir: %s", asset::userDir.c_str());

	std::string resDir = asset::system("res");
	if (!system::isDirectory(resDir)) {
		// Non fatale: senza res/ i moduli non si caricano, ma il plugin resta istanziabile.
		// In un DAW non possiamo mostrare dialog modali.
		WARN("res/ non trovata in %s — i moduli non si caricheranno", resDir.c_str());
	}

	string::init();
	settings::init();
	try {
		settings::load();
	}
	catch (Exception& e) {
		WARN("settings::load fallito: %s (continuo con i default)", e.what());
	}

	network::init();
	audio::init();
	// Registra il nostro driver "DAW". audio::addDriver prende possesso del puntatore
	// (audio::destroy lo eliminerà).
	g_dawDriver = new DawDriver;
	audio::addDriver(DAW_DRIVER_ID, g_dawDriver);
	midi::init();
	// Registra il nostro driver MIDI "DAW" (gemello del driver audio). midi::addDriver prende
	// possesso del puntatore (midi::destroy lo eliminerà).
	g_dawMidiDriver = new DawMidiDriver;
	midi::addDriver(DAW_MIDI_DRIVER_ID, g_dawMidiDriver);
	midiloopback::init();
	plugin::init();
	// Browser e library alimentano la vista LIBRARY della finestra accessibile (fuzzy
	// search + albero plugin). Innocui e a basso costo anche in headless; li inizializziamo
	// sempre, dopo plugin::init() (che popola i modelli). Nessuno dei due tocca la finestra.
	app::browserInit();
	library::init();

	INFO("Process init completato");
	return true;
}


void processDeinit() {
	// Specchio inverso di processInit(). NB: NON salviamo le settings, per non
	// sovrascrivere quelle dello standalone dell'utente.
	library::destroy();
	plugin::destroy();
	midi::destroy(); // elimina anche g_dawMidiDriver (ne aveva preso possesso addDriver)
	g_dawMidiDriver = nullptr;
	audio::destroy();
	network::destroy();
	settings::destroy();
	logger::destroy();
}


} // anonymous namespace


// =====================================================================================
// API pubblica
// =====================================================================================

bool retainProcess(const char* logName) {
	std::lock_guard<std::mutex> lock(g_initMutex);
	if (g_initCount++ == 0) {
		g_initOk = processInit(logName);
	}
	return g_initOk;
}

void releaseProcess() {
	std::lock_guard<std::mutex> lock(g_initMutex);
	if (--g_initCount == 0 && g_initOk) {
		processDeinit();
		g_initOk = false;
	}
}


void useContext(Instance* inst) {
	contextSet(inst->context);
}


Instance* createInstance() {
	Instance* inst = new Instance;

#if defined ARCH_WIN
	// Sottosistemi GUI (GLFW + contesto GL + font) UNA volta, sul main thread.
	if (!g_guiSubsystemsInited) {
		INFO("Init sottosistemi GUI (ui + window) sul main thread");
		ui::init();
		window::init();
		g_guiSubsystemsInited = true;
	}
#endif

	// Costruisci il Context per-istanza. Da qui in poi APP punta a questa istanza
	// sul thread corrente (main thread).
	inst->context = new Context;
	useContext(inst);

	APP->midiLoopbackContext = new midiloopback::Context;
	APP->engine = new engine::Engine;
	APP->history = new history::State;
	APP->event = new widget::EventState;
	APP->scene = new app::Scene;
	APP->event->rootWidget = APP->scene;
	APP->patch = new patch::Manager;

#if defined ARCH_WIN
	// --- albero widget + finestra accessibile top-level -------------------------------
	// La finestra GL serve SOLO per il suo contesto nanovg (rasterizza i pannelli SVG dei
	// moduli); non verrà mai disegnata (la teniamo nascosta). Il suo costruttore rende il
	// contesto GL corrente, così createModuleWidget può caricare i pannelli.
	APP->window = new window::Window;

	// Patch vuota deterministica: niente dialog modali "missing modules" in un DAW.
	APP->patch->clear();

	// Ponte audio verso il DAW, con widget (vedi sopra).
	addDawAudioModule(inst);

	// La finestra accessibile è l'unica UI. È top-level non posseduta: nasconderemo la
	// finestra GL senza nasconderla. La creiamo NASCOSTA; sarà l'adapter a mostrarla.
	HWND rackHwnd = glfwGetWin32Window(APP->window->win);
	inst->accessibleWindow = accessible::AccessibleWindow::create(rackHwnd);
	if (inst->accessibleWindow) {
		// In un plugin non c'è run loop di Rack che chiami drainCommands(): la finestra
		// accessibile lo fa dal proprio WM_TIMER (pompato dal message loop dell'host).
		inst->accessibleWindow->pluginMode = true;
		ShowWindow(inst->accessibleWindow->hwnd, SW_HIDE);
	}
	glfwHideWindow(APP->window->win);
#else
	// --- Fuori da Windows: percorso headless bare-engine ------------------------------
	// Grafo pass-through diretto nell'engine (niente widget, niente finestra): DAW in ->
	// engine -> DAW out.
	plugin::Model* audioModel = plugin::getModel("Core", "AudioInterface16");
	if (audioModel) {
		inst->audioModule = audioModel->createModule();
		APP->engine->addModule(inst->audioModule);
		bindAudioModuleToDaw(inst->audioModule, inst->sampleRate, (int) inst->maxFrames);
		addPassthroughCablesHeadless(inst->audioModule);
		INFO("Grafo pass-through creato (modulo Audio id=%lld)",
		     (long long) inst->audioModule->id);
	}
	else {
		WARN("Modello Core/AudioInterface16 non trovato: nessun audio (Core caricato?)");
	}
#endif

	INFO("Istanza inizializzata (context=%p)", (void*) inst->context);
	return inst;
}


void destroyInstance(Instance* inst) {
	useContext(inst);

#if defined ARCH_WIN
	// Distruggi la finestra accessibile PRIMA del Context: legge scene/engine e li
	// referenzia (instance singleton, timer). Il suo distruttore smonta il proprio HWND.
	delete inst->accessibleWindow;
	inst->accessibleWindow = nullptr;
#endif

	// Il distruttore di Context elimina engine/scene/patch/window in ordine.
	delete inst->context;
	inst->context = nullptr;
	contextSet(nullptr);

#if defined ARCH_WIN
	// NON smontiamo i sottosistemi GUI qui. window::destroy() (= glfwTerminate) mandava in
	// crash la DAW alla rimozione del plugin: il log del teardown arriva pulito fino
	// all'ultima riga di ~Context e poi il processo muore, e gli unici passi rimasti sono
	// glfwTerminate (ui::destroy è vuota). È lo STESSO teardown che lo standalone su Windows
	// si rifiuta di eseguire — vedi il commento in standalone.cpp, dove al posto di
	// `delete APP`/window::destroy() fa TerminateProcess proprio perché su Windows quel
	// percorso va in crash. Il plugin non può terminare il processo (è quello della DAW),
	// quindi la soluzione è semplicemente NON eseguirlo: glfwTerminate in-process, dopo che
	// ~Context ha già distrutto la finestra GL, tocca stato globale GLFW/driver GPU ancora
	// referenziato e fa cadere l'host.
	//
	// Conseguenza: GLFW/ui restano inizializzati per tutta la vita del processo (g_guiSubsystems
	// Inited resta true). È corretto e anzi utile con questo adapter mono-istanza: se l'utente
	// ri-aggiunge il plugin, createInstance riusa i sottosistemi già pronti e crea solo una
	// nuova window::Window. Alla chiusura della DAW / unload della DLL è l'OS a reclamare tutto,
	// esattamente come lo standalone lascia leakare APP di proposito.
	(void) g_guiSubsystemsInited;
#endif

	delete inst;
}


bool activate(Instance* inst, double sampleRate, uint32_t maxFrames) {
	useContext(inst);
	inst->sampleRate = sampleRate;
	inst->maxFrames = maxFrames;
	inst->activated = true;

	// Allinea il device DAW al formato negoziato col DAW. L'AudioPort del modulo legge
	// device->getSampleRate() a ogni blocco, quindi questo basta a far seguire all'engine
	// il sample rate dell'host.
	g_dawDriver->device.setSampleRate((float) sampleRate);
	g_dawDriver->device.setBlockSize((int) maxFrames);

	// Preallocazione buffer interleaved (kNumChannels canali). Mai allocare in process().
	inst->inInterleaved.assign((size_t) maxFrames * kNumChannels, 0.f);
	inst->outInterleaved.assign((size_t) maxFrames * kNumChannels, 0.f);

	INFO("Istanza attivata: sampleRate=%g maxFrames=%u", sampleRate, maxFrames);
	return true;
}


void deactivate(Instance* inst) {
	useContext(inst);
	inst->activated = false;
}


void processPlanar(Instance* inst, const float* const* in, uint32_t numIn,
                   float* const* out, uint32_t numOut, uint32_t frames) {
	useContext(inst);

	// Il buffer preallocato in activate() dimensiona il massimo blocco ammesso; un host che
	// ne chiede di più violerebbe il contratto, ma non ci schiantiamo per questo.
	if (frames > inst->maxFrames)
		return;

	// Planare -> interleaved (stride = kNumChannels). Un canale oltre numIn o con puntatore
	// nullo (bus non collegato dall'host) vale silenzio. Le ricerche del puntatore stanno
	// fuori dal loop dei frame: è il thread audio, niente lavoro inutile per campione.
	float* inter = inst->inInterleaved.data();
	for (int ch = 0; ch < kNumChannels; ch++) {
		const float* src = (in && (uint32_t) ch < numIn) ? in[ch] : nullptr;
		if (src) {
			for (uint32_t i = 0; i < frames; i++)
				inter[kNumChannels * i + ch] = src[i];
		}
		else {
			for (uint32_t i = 0; i < frames; i++)
				inter[kNumChannels * i + ch] = 0.f;
		}
	}

	float* outInter = inst->outInterleaved.data();
	g_dawDriver->device.processBuffer(inter, kNumChannels, outInter, kNumChannels, (int) frames);

	// Interleaved -> planare verso le uscite del DAW.
	for (uint32_t ch = 0; ch < numOut && (int) ch < kNumChannels; ch++) {
		float* dst = out ? out[ch] : nullptr;
		if (!dst)
			continue;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = outInter[kNumChannels * i + ch];
	}
}


void pushMidiMessage(Instance* inst, const uint8_t* bytes, int len, int32_t sampleOffset) {
	if (!g_dawMidiDriver || !bytes || len <= 0)
		return;
	useContext(inst);

	// Timestamp assoluto in frame dell'engine. getFrame() qui (prima di stepBlock) è il frame
	// d'inizio blocco; sommando l'offset otteniamo il campione esatto in cui il modulo Core
	// MIDI rilascerà il messaggio dalla sua InputQueue (tryPop confronta con args.frame).
	int64_t frame = APP->engine->getFrame() + (sampleOffset > 0 ? sampleOffset : 0);

	midi::Message msg;
	// bytes parte a size 3 (vedi midi::Message): assign non rialloca per i messaggi a 3 byte
	// (note, aftertouch), quindi sul thread audio non allochiamo nel caso comune. Solo un
	// SysEx più lungo di 3 byte causerebbe una realloc.
	msg.bytes.assign(bytes, bytes + len);
	msg.setFrame(frame);

	// onMessage inoltra a tutti gli Input sottoscritti (con filtro di canale). Se nessun
	// modulo è collegato al device "DAW", il messaggio si perde qui: è corretto, sta all'utente
	// instradarlo. Il contatore sale comunque, così l'harness verifica il ponte a monte.
	g_dawMidiCount.fetch_add(1, std::memory_order_relaxed);
	g_dawMidiDriver->device.onMessage(msg);
}


uint64_t debugMidiMessageCount() {
	return g_dawMidiCount.load(std::memory_order_relaxed);
}


#if defined ARCH_WIN

bool guiShow(Instance* inst) {
	if (!inst->accessibleWindow || !inst->accessibleWindow->hwnd)
		return false;
	HWND target = inst->accessibleWindow->hwnd;
	// Defeat Windows' foreground lock: SetForegroundWindow is ignored unless our thread
	// owns the current foreground, so we borrow the foreground thread's input queue for
	// the duration. Without this the DAW keeps focus and MetaRack only comes forward
	// visually (leaving the screen reader stranded on the host's editor).
	HWND fg = GetForegroundWindow();
	// Whatever was in front the moment before we steal focus is the DAW — remember it as
	// the F6 "return to host" target. This is more robust than guiSetTransient()'s host
	// window (some hosts never call set_transient, or hand us an ancestor we can't
	// foreground), and it costs nothing when they do: it just refreshes the same target.
	if (fg && fg != target)
		inst->accessibleWindow->hostWindow = fg;
	DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
	DWORD myThread = GetCurrentThreadId();
	if (fgThread != myThread)
		AttachThreadInput(myThread, fgThread, TRUE);
	ShowWindow(target, SW_SHOW);
	BringWindowToTop(target);
	SetForegroundWindow(target);
	SetFocus(target);
	if (fgThread != myThread)
		AttachThreadInput(myThread, fgThread, FALSE);
	return true;
}

bool guiHide(Instance* inst) {
	if (!inst->accessibleWindow || !inst->accessibleWindow->hwnd)
		return false;
	ShowWindow(inst->accessibleWindow->hwnd, SW_HIDE);
	return true;
}

bool guiGetSize(Instance* inst, uint32_t* width, uint32_t* height) {
	if (inst->accessibleWindow && inst->accessibleWindow->hwnd) {
		RECT rc;
		if (GetWindowRect(inst->accessibleWindow->hwnd, &rc)) {
			*width = (uint32_t)(rc.right - rc.left);
			*height = (uint32_t)(rc.bottom - rc.top);
			return true;
		}
	}
	*width = 900;
	*height = 600;
	return true;
}

bool guiSetTransient(Instance* inst, HWND hostWindow) {
	if (!inst->accessibleWindow || !inst->accessibleWindow->hwnd || !hostWindow)
		return false;
	// We deliberately do NOT make MetaRack an owned window of the host here. Ownership
	// pins it visually above the DAW editor (worthless to a screen-reader user) but, once
	// applied to an already-shown top-level window, unreliably strips it from Alt+Tab even
	// with WS_EX_APPWINDOW — leaving a blind user no keyboard route back after F6. So we
	// keep MetaRack unowned and only record the host window as the F6 return target.
	inst->accessibleWindow->hostWindow = hostWindow;
	return true;
}

// True while the accessible window has asked adapters to hold off re-focusing it. Set
// briefly when the user presses F6 to hand focus back to the DAW: the VST3 placeholder's
// bounce-on-focus (see vst3.cpp) would otherwise yank MetaRack straight back the instant
// the host refocuses its editor (observed in Ableton, where F6 returns to the plugin
// window rather than the arrange view).
bool isBounceSuppressed(Instance* inst) {
	if (!inst->accessibleWindow)
		return false;
	DWORD until = inst->accessibleWindow->suppressBounceUntil;
	return until != 0 && (int32_t)(GetTickCount() - until) < 0;
}

#endif // ARCH_WIN


} // namespace rackhost
