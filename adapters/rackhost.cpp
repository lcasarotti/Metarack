// Core condiviso dagli adapter plugin (CLAP, VST3) — vedi rackhost.hpp.
//
// Il grosso di questo file è stato estratto da adapters/clap.cpp, dove era nato insieme
// all'adapter CLAP; è codice indipendente dal formato di plugin.

#include "rackhost.hpp"

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
	int numInputs = 2;
	int numOutputs = 2;

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
	for (int i = 0; i < 2; i++) {
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
	for (int i = 0; i < 2; i++) {
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
	plugin::Model* audioModel = plugin::getModel("Core", "AudioInterface2");
	if (!audioModel) {
		WARN("Modello Core/AudioInterface2 non trovato: nessun ponte audio (Core caricato?)");
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
	midi::destroy();
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
	plugin::Model* audioModel = plugin::getModel("Core", "AudioInterface2");
	if (audioModel) {
		inst->audioModule = audioModel->createModule();
		APP->engine->addModule(inst->audioModule);
		bindAudioModuleToDaw(inst->audioModule, inst->sampleRate, (int) inst->maxFrames);
		addPassthroughCablesHeadless(inst->audioModule);
		INFO("Grafo pass-through creato (modulo Audio id=%lld)",
		     (long long) inst->audioModule->id);
	}
	else {
		WARN("Modello Core/AudioInterface2 non trovato: nessun audio (Core caricato?)");
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
	// v1 mono-istanza: smonta i sottosistemi GUI sul main thread, specchio dell'init in
	// createInstance(). window::destroy() (= glfwTerminate) va dopo la distruzione
	// dell'oggetto Window fatta da ~Context.
	if (g_guiSubsystemsInited) {
		window::destroy();
		ui::destroy();
		g_guiSubsystemsInited = false;
	}
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

	// Preallocazione buffer interleaved (2 canali). Mai allocare in process().
	inst->inInterleaved.assign((size_t) maxFrames * 2, 0.f);
	inst->outInterleaved.assign((size_t) maxFrames * 2, 0.f);

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

	// Sorgenti di input (NULL-safe: se l'host non collega l'ingresso, trattiamo come zero;
	// con un solo canale duplichiamo L su R).
	const float* inL = (in && numIn > 0) ? in[0] : nullptr;
	const float* inR = (in && numIn > 1) ? in[1] : inL;

	float* inter = inst->inInterleaved.data();
	for (uint32_t i = 0; i < frames; i++) {
		inter[2 * i + 0] = inL ? inL[i] : 0.f;
		inter[2 * i + 1] = inR ? inR[i] : 0.f;
	}

	float* outInter = inst->outInterleaved.data();
	g_dawDriver->device.processBuffer(inter, 2, outInter, 2, (int) frames);

	// De-interleave verso le uscite planari del DAW.
	for (uint32_t ch = 0; ch < numOut && ch < 2; ch++) {
		float* dst = out ? out[ch] : nullptr;
		if (!dst)
			continue;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = outInter[2 * i + ch];
	}
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
	SetWindowLongPtrW(inst->accessibleWindow->hwnd, GWLP_HWNDPARENT, (LONG_PTR) hostWindow);
	// Remember the DAW's window so the accessible UI's F6 shortcut can return focus to it.
	inst->accessibleWindow->hostWindow = hostWindow;
	return true;
}

#endif // ARCH_WIN


} // namespace rackhost
