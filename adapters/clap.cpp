// Adapter CLAP per VCV Rack (Metarack).
//
// Questo file è solo il GUSCIO ABI: traduce il ciclo di vita e le estensioni CLAP in
// chiamate a rackhost, il core condiviso con l'adapter VST3 (vedi adapters/rackhost.hpp).
// Tutta la logica di Rack — init dei singleton di processo, driver audio "DAW", Context
// per istanza, finestra accessibile — vive lì.
//
// Su Windows un .clap è semplicemente una DLL che esporta il simbolo `clap_entry`.

#include <clap/clap.h>

#include <cstring>

#include "rackhost.hpp"

#include <arch.hpp>
#include <logger.hpp>

#if defined ARCH_WIN
	#include <windows.h>
#endif


namespace {


// L'istanza CLAP tiene insieme la struct C esposta al DAW e il puntatore al core.
struct ClapInstance {
	clap_plugin_t plugin;                  // l'interfaccia C esposta al DAW
	const clap_host_t* host = nullptr;
	rackhost::Instance* rack = nullptr;    // nullo finché il DAW non chiama init()
};

inline ClapInstance* owner(const clap_plugin_t* p) {
	return static_cast<ClapInstance*>(p->plugin_data);
}

// Le funzioni che toccano APP passano da qui. rackhost applica useContext() per conto
// nostro: ogni callback del DAW può arrivare su un thread diverso e APP è thread-local.
inline rackhost::Instance* self(const clap_plugin_t* p) {
	return owner(p)->rack;
}


// --- estensione audio-ports ----------------------------------------------------------
//
// Esponiamo i kNumChannels canali del modulo Audio-16 come BUS STEREO separati (uno per
// coppia): 8 in ingresso e 8 in uscita. Così il DAW ci vede come uno strumento multi-uscita
// e puoi mandare ogni coppia — "Out 1/2", "Out 3/4", ... — a una traccia diversa. Il bus 0
// porta il flag MAIN (alcuni host lo pretendono); gli altri sono ausiliari.
constexpr uint32_t kStereoBuses = (uint32_t) rackhost::kNumChannels / 2;

uint32_t CLAP_ABI audioPortsCount(const clap_plugin_t* /*plugin*/, bool /*is_input*/) {
	return kStereoBuses;
}

bool CLAP_ABI audioPortsGet(const clap_plugin_t* /*plugin*/, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
	if (index >= kStereoBuses)
		return false;
	info->id = index;
	// Canali 1-based nel nome, come li conta un DAW: bus 0 -> "1/2", bus 1 -> "3/4", ...
	std::snprintf(info->name, sizeof(info->name), "%s %u/%u",
	              is_input ? "In" : "Out", index * 2 + 1, index * 2 + 2);
	info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

const clap_plugin_audio_ports_t g_audioPorts = {
	audioPortsCount,
	audioPortsGet,
};


// --- ciclo di vita dell'istanza -----------------------------------------------------

bool CLAP_ABI pluginInit(const clap_plugin_t* plugin) {
	// La spec CLAP garantisce che plugin->init() giri sul main thread, cosa che
	// entry.init() non fa: è qui che rackhost può inizializzare GLFW.
	ClapInstance* inst = owner(plugin);
	inst->rack = rackhost::createInstance();
	return inst->rack != nullptr;
}

void CLAP_ABI pluginDestroy(const clap_plugin_t* plugin) {
	ClapInstance* inst = owner(plugin);
	if (inst->rack)
		rackhost::destroyInstance(inst->rack);
	delete inst;
}

bool CLAP_ABI pluginActivate(const clap_plugin_t* plugin, double sampleRate,
                             uint32_t /*minFrames*/, uint32_t maxFrames) {
	return rackhost::activate(self(plugin), sampleRate, maxFrames);
}

void CLAP_ABI pluginDeactivate(const clap_plugin_t* plugin) {
	rackhost::deactivate(self(plugin));
}

bool CLAP_ABI pluginStartProcessing(const clap_plugin_t* /*plugin*/) {
	return true;
}

void CLAP_ABI pluginStopProcessing(const clap_plugin_t* /*plugin*/) {
}

void CLAP_ABI pluginReset(const clap_plugin_t* /*plugin*/) {
}

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* plugin,
    const clap_process_t* process) {
	// CLAP usa buffer PLANARI (data32[canale][frame]) e un array separato per ogni bus.
	// rackhost::processPlanar vuole invece UN array piatto di kNumChannels puntatori-per-
	// canale, quindi "srotoliamo" qui gli 8 bus stereo in ingresso e in uscita. Gli array
	// nascono azzerati ({}), così un bus non fornito dall'host resta silenzio.
	const float* inChans[rackhost::kNumChannels] = {};
	float* outChans[rackhost::kNumChannels] = {};

	for (uint32_t p = 0; p < process->audio_inputs_count && p < kStereoBuses; p++) {
		const clap_audio_buffer_t& buf = process->audio_inputs[p];
		for (uint32_t c = 0; c < buf.channel_count && c < 2; c++)
			inChans[p * 2 + c] = buf.data32 ? buf.data32[c] : nullptr;
	}
	for (uint32_t p = 0; p < process->audio_outputs_count && p < kStereoBuses; p++) {
		const clap_audio_buffer_t& buf = process->audio_outputs[p];
		for (uint32_t c = 0; c < buf.channel_count && c < 2; c++)
			outChans[p * 2 + c] = buf.data32 ? buf.data32[c] : nullptr;
	}

	rackhost::processPlanar(self(plugin), inChans, rackhost::kNumChannels,
	                        outChans, rackhost::kNumChannels, process->frames_count);
	return CLAP_PROCESS_CONTINUE;
}

#if defined ARCH_WIN
// =====================================================================================
// Estensione GUI — finestra accessibile top-level "floating"
// =====================================================================================
//
// MetaRack non incorpora nulla nell'editor del DAW: la UI è la finestra accessibile
// Win32 top-level, che l'host mostra/nasconde via questa estensione. Dichiariamo di
// supportare SOLO l'API Win32 in modalità FLOATING (is_floating=true): niente set_parent,
// niente reparent nell'HWND dell'host. La finestra è già stata creata da
// rackhost::createInstance(); qui facciamo essenzialmente da telecomando show/hide.

bool CLAP_ABI guiIsApiSupported(const clap_plugin_t* /*plugin*/, const char* api, bool is_floating) {
	return is_floating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}
bool CLAP_ABI guiGetPreferredApi(const clap_plugin_t* /*plugin*/, const char** api, bool* is_floating) {
	*api = CLAP_WINDOW_API_WIN32;
	*is_floating = true;
	return true;
}
bool CLAP_ABI guiCreate(const clap_plugin_t* /*plugin*/, const char* api, bool is_floating) {
	// La finestra vive già (createInstance). Accettiamo solo la modalità floating Win32.
	return is_floating && (!api || !*api || std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0);
}
void CLAP_ABI guiDestroy(const clap_plugin_t* /*plugin*/) {
	// La finestra dura quanto l'istanza: distrutta in pluginDestroy. Qui nessuna azione:
	// evita di distruggere risorse che il ciclo di vita gestisce altrove.
}
bool CLAP_ABI guiSetScale(const clap_plugin_t* /*plugin*/, double /*scale*/) {
	return false;
}
bool CLAP_ABI guiGetSize(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
	return rackhost::guiGetSize(self(plugin), width, height);
}
bool CLAP_ABI guiCanResize(const clap_plugin_t* /*plugin*/) {
	// Floating: la finestra si ridimensiona da sé, l'host non la gestisce.
	return false;
}
bool CLAP_ABI guiGetResizeHints(const clap_plugin_t* /*plugin*/, clap_gui_resize_hints_t* /*hints*/) {
	return false;
}
bool CLAP_ABI guiAdjustSize(const clap_plugin_t* /*plugin*/, uint32_t* /*width*/, uint32_t* /*height*/) {
	return false;
}
bool CLAP_ABI guiSetSize(const clap_plugin_t* /*plugin*/, uint32_t /*width*/, uint32_t /*height*/) {
	return false;
}
bool CLAP_ABI guiSetParent(const clap_plugin_t* /*plugin*/, const clap_window_t* /*window*/) {
	// Floating-only: non ci incorporiamo mai in una finestra dell'host.
	return false;
}
bool CLAP_ABI guiSetTransient(const clap_plugin_t* plugin, const clap_window_t* window) {
	if (!window || !window->api || std::strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0)
		return false;
	return rackhost::guiSetTransient(self(plugin), (HWND) window->win32);
}
void CLAP_ABI guiSuggestTitle(const clap_plugin_t* /*plugin*/, const char* /*title*/) {
	// Il titolo resta "MetaRack" (impostato dalla finestra accessibile).
}
bool CLAP_ABI guiShow(const clap_plugin_t* plugin) {
	return rackhost::guiShow(self(plugin));
}
bool CLAP_ABI guiHide(const clap_plugin_t* plugin) {
	return rackhost::guiHide(self(plugin));
}

const clap_plugin_gui_t g_gui = {
	guiIsApiSupported,
	guiGetPreferredApi,
	guiCreate,
	guiDestroy,
	guiSetScale,
	guiGetSize,
	guiCanResize,
	guiGetResizeHints,
	guiAdjustSize,
	guiSetSize,
	guiSetParent,
	guiSetTransient,
	guiSuggestTitle,
	guiShow,
	guiHide,
};
#endif // ARCH_WIN


const void* CLAP_ABI pluginGetExtension(const clap_plugin_t* /*plugin*/, const char* id) {
	if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
		return &g_audioPorts;
#if defined ARCH_WIN
	if (std::strcmp(id, CLAP_EXT_GUI) == 0)
		return &g_gui;
#endif
	return nullptr;
}

void CLAP_ABI pluginOnMainThread(const clap_plugin_t* /*plugin*/) {
}


// =====================================================================================
// Descrittore + factory
// =====================================================================================

const char* const g_features[] = {
	CLAP_PLUGIN_FEATURE_INSTRUMENT,
	CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
	CLAP_PLUGIN_FEATURE_STEREO,
	nullptr,
};

const clap_plugin_descriptor_t g_descriptor = {
	CLAP_VERSION_INIT,
	"com.metarack.rack",                 // id
	"Metarack",                          // name
	"Metarack",                          // vendor
	"",                                  // url
	"",                                  // manual_url
	"",                                  // support_url
	"2.6.4",                             // version
	"VCV Rack come plugin CLAP (accessibile)",  // description
	g_features,
};


// Quante volte entry.init() è riuscita senza una entry.deinit() corrispondente: finché è
// > 0 i singleton di processo sono vivi e la factory può creare istanze.
int g_entryCount = 0;


const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t* /*factory*/,
    const clap_host_t* host, const char* pluginId) {
	if (g_entryCount <= 0)
		return nullptr;
	if (std::strcmp(pluginId, g_descriptor.id) != 0)
		return nullptr;

	ClapInstance* inst = new ClapInstance;
	inst->host = host;
	inst->plugin.desc = &g_descriptor;
	inst->plugin.plugin_data = inst;
	inst->plugin.init = pluginInit;
	inst->plugin.destroy = pluginDestroy;
	inst->plugin.activate = pluginActivate;
	inst->plugin.deactivate = pluginDeactivate;
	inst->plugin.start_processing = pluginStartProcessing;
	inst->plugin.stop_processing = pluginStopProcessing;
	inst->plugin.reset = pluginReset;
	inst->plugin.process = pluginProcess;
	inst->plugin.get_extension = pluginGetExtension;
	inst->plugin.on_main_thread = pluginOnMainThread;
	return &inst->plugin;
}

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t* /*factory*/) {
	return 1;
}

const clap_plugin_descriptor_t* CLAP_ABI factoryGetDescriptor(
  const clap_plugin_factory_t* /*factory*/, uint32_t index) {
	return (index == 0) ? &g_descriptor : nullptr;
}

const clap_plugin_factory_t g_factory = {
	factoryCount,
	factoryGetDescriptor,
	factoryCreate,
};


// =====================================================================================
// Entry-point della DSO
// =====================================================================================
//
// CLAP permette (raramente) chiamate multiple a entry.init()/deinit(); rackhost::
// retainProcess/releaseProcess sono refcounted proprio per questo.

bool CLAP_ABI entryInit(const char* /*pluginPath*/) {
	if (!rackhost::retainProcess("clap-log.txt"))
		return false;
	g_entryCount++;
	return true;
}

void CLAP_ABI entryDeinit() {
	rackhost::releaseProcess();
	if (g_entryCount > 0)
		g_entryCount--;
}

const void* CLAP_ABI entryGetFactory(const char* factoryId) {
	if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
		return &g_factory;
	return nullptr;
}


} // anonymous namespace


// Simbolo esportato che il DAW cerca nella DSO.
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT,
	entryInit,
	entryDeinit,
	entryGetFactory,
};
