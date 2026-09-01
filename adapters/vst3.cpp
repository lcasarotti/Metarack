// Adapter VST3 per VCV Rack (MetaRack).
//
// Questo file è solo il GUSCIO ABI: traduce il ciclo di vita VST3 in chiamate a rackhost,
// il core condiviso con l'adapter CLAP (vedi adapters/rackhost.hpp). Tutta la logica di
// Rack — init dei singleton di processo, driver audio "DAW", Context per istanza, finestra
// accessibile — vive lì.
//
// NIENTE SDK Steinberg: usiamo travesty (adapters/vst3/travesty/, ISC), che dichiara l'ABI
// VST3 in C puro. Ogni "interfaccia" è una struct di puntatori a funzione esplicita, quindi
// non dipendiamo dal layout vtable del compilatore e possiamo costruire il .vst3 con lo
// stesso MinGW che produce libRack.dll, invece che con MSVC.
//
// L'IDIOMA ABI, che è la parte meno ovvia (cfr. DPF, DistrhoPluginVST3.cpp):
//   - Un oggetto EREDITA PER VALORE la struct-vtable (`struct RackComponent : v3_component_cpp`),
//     quindi i suoi primi byte SONO i puntatori a funzione.
//   - All'host non si passa mai l'oggetto, ma un puntatore a un PUNTATORE all'oggetto (T**).
//     Perciò ogni metodo recupera l'oggetto con `*static_cast<T**>(self)`.
//   - I sotto-oggetti (audio processor, ...) vivono come MEMBRI PUNTATORE: l'indirizzo del
//     membro è proprio il T** da consegnare all'host.
//
// Su Windows un .vst3 è una DLL (dentro un bundle) che esporta GetPluginFactory/InitDll/ExitDll.
// Su macOS è un bundle vero (Contents/MacOS/<nome>, caricato con dlopen dal loader dell'host)
// che esporta GetPluginFactory/bundleEntry/bundleExit. L'ABI è la stessa su entrambe le
// piattaforme; cambia solo il segnaposto che l'host incorpora — una finestra figlia Win32
// qui, una NSView in adapters/vst3_mac.mm — perché la UI vera è in tutti e due i casi la
// finestra accessibile, che sta per conto proprio.

#include "vst3/travesty/audio_processor.h"
#include "vst3/travesty/component.h"
#include "vst3/travesty/edit_controller.h"
#include "vst3/travesty/factory.h"
#include "vst3/travesty/view.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "rackhost.hpp"

#include <arch.hpp>
#include <logger.hpp>
#include <settings.hpp>

#if defined ARCH_WIN
	#include <windows.h>
#else
	#include "vst3_mac.hpp" // segnaposto NSView (implementato in vst3_mac.mm)
#endif


namespace {


// Identificatore stabile della nostra classe. DEVE restare costante nel tempo (i DAW lo
// salvano nei progetti e nelle cache di scansione) e DEVE essere diverso da quello che
// generava il vecchio VST3 costruito con clap-wrapper, altrimenti gli host confondono i due.
// "Meta" "Rack" "Comp" "onen"
constexpr v3_tuid g_classTuid = V3_ID(0x4D657461, 0x5261636B, 0x436F6D70, 0x6F6E656E);

// Nome mostrato nel browser del DAW e vendor/produttore. Coerenti con lo standalone:
// il PRODOTTO è "MetaRack", l'autore/vendor è "Luca Casarotti" (come per la versione desktop).
const char* const kPluginName = "MetaRack";
const char* const kVendor = "Luca Casarotti";
const char* const kVersion = "2.6.4";
// Sub-categorie VST3: come il CLAP, ci dichiariamo sia strumento che effetto stereo.
const char* const kSubCategories = "Instrument|Fx|Stereo";

// I rackhost::kNumChannels canali del ponte audio, esposti all'host come BUS STEREO separati
// (uno per coppia): 8 in ingresso e 8 in uscita. Bus 0 = MAIN, gli altri AUX → strumento
// multi-uscita, ogni coppia instradabile su una traccia diversa del DAW. Stessa scelta in
// clap.cpp (là come porte CLAP), qui come bus VST3.
constexpr int32_t kStereoBuses = rackhost::kNumChannels / 2;

// MIDI-CC come parametri VST3. VST3 non trasporta CC / pitch-bend / channel-pressure come
// EVENTI (a differenza delle note): li instrada come PARAMETER CHANGE, dopo che il plugin ha
// mappato (canale, controller) -> id parametro via IMidiMapping. Layout mutuato da DPF:
// 130 controller per canale — 0..127 = CC, 128 = channel pressure (aftertouch), 129 =
// pitch-bend — su 16 canali. id parametro = canale * 130 + controller. Sono gli UNICI
// parametri che esponiamo (un rack non si espone come manopole automatizzabili), quindi
// l'id parametro è direttamente questo indice, senza offset.
constexpr int32_t kMidiControllersPerChannel = 130;
constexpr int32_t kMidiCCParamCount = kMidiControllersPerChannel * 16; // 2080


// Copia una stringa ASCII in un campo UTF-16 di dimensione fissa (v3_str_128 e i campi
// "unicode" della factory v3), terminata a zero.
void copyUtf16(int16_t* dst, const char* src, size_t dstLen) {
	size_t i = 0;
	for (; src[i] && i + 1 < dstLen; i++)
		dst[i] = (int16_t)(unsigned char) src[i];
	dst[i] = 0;
}

void copyAscii(char* dst, const char* src, size_t dstLen) {
	std::snprintf(dst, dstLen, "%s", src);
}


// =====================================================================================
// Audio processor — tearoff del component
// =====================================================================================
//
// Non possiede il proprio ciclo di vita: nasce e muore col component, che lo tiene come
// membro puntatore. ref/unref muovono solo il contatore (l'host deve poterlo rilasciare
// senza distruggere nulla).

struct RackComponent;

// =====================================================================================
// Plug view — il ponte verso la finestra accessibile
// =====================================================================================
//
// VST3 non ha il concetto di finestra "floating" che il CLAP ci dà (is_floating=true):
// l'unica GUI prevista è IPlugView, che l'host INCORPORA nella propria finestra editor via
// attached(parent). MetaRack però non ha nulla da incorporare — la sua UI è la finestra
// accessibile top-level, l'unica che NVDA (Windows) o VoiceOver (macOS) può leggere.
//
// Quindi facciamo da ponte: nell'editor del DAW mettiamo un pannellino segnaposto, e
// attached()/removed() mostrano e nascondono la vera finestra. Il pulsante "apri editor"
// del DAW resta così significativo, e il comportamento combacia con quello del CLAP.
//
// A differenza dell'audio processor, la view NON è un tearoff: create_view() consegna
// all'host un riferimento nuovo, e la view si distrugge da sé quando l'host lo rilascia.

struct RackPlugView : v3_plugin_view_cpp {
	std::atomic_int refcounter;
	RackComponent* component;

#if defined ARCH_WIN
	HWND placeholder = nullptr;
	static const int kWidth = 320;
	static const int kHeight = 80;
#else
	void* placeholder = nullptr; // MetaRackPlaceholderView*, opaca qui (vedi vst3_mac.hpp)
	static const int kWidth = metarack::vst3mac::kWidth;
	static const int kHeight = metarack::vst3mac::kHeight;
#endif

	RackPlugView(RackComponent* c);
	~RackPlugView();

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface);
	static uint32_t V3_API refFn(void* self);
	static uint32_t V3_API unrefFn(void* self);

	static v3_result V3_API isPlatformTypeSupported(void* self, const char* platformType);
	static v3_result V3_API attached(void* self, void* parent, const char* platformType);
	static v3_result V3_API removed(void* self);
	static v3_result V3_API onWheel(void* self, float distance);
	static v3_result V3_API onKeyDown(void* self, int16_t keyChar, int16_t keyCode, int16_t modifiers);
	static v3_result V3_API onKeyUp(void* self, int16_t keyChar, int16_t keyCode, int16_t modifiers);
	static v3_result V3_API getSize(void* self, v3_view_rect* rect);
	static v3_result V3_API onSize(void* self, v3_view_rect* rect);
	static v3_result V3_API onFocus(void* self, v3_bool state);
	static v3_result V3_API setFrame(void* self, v3_plugin_frame** frame);
	static v3_result V3_API canResize(void* self);
	static v3_result V3_API checkSizeConstraint(void* self, v3_view_rect* rect);
};


// =====================================================================================
// MIDI mapping — IMidiMapping, tearoff del controller
// =====================================================================================
//
// L'unica via per cui l'host ci manda CC / pitch-bend / channel-pressure. get_midi_controller_
// assignment traduce (bus, canale, controller) nell'id del parametro nascosto corrispondente
// (vedi kMidiControllersPerChannel). Oggetto minuscolo e stateless, posseduto dal controller.

struct RackMidiMapping : v3_midi_mapping_cpp {
	std::atomic_int refcounter; // vestigiale: le ref inoltrano al component (identità COM)
	RackComponent* component;   // proprietario logico: ref/unref inoltrano al suo refcounter

	RackMidiMapping(RackComponent* c);

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface);
	static uint32_t V3_API refFn(void* self);
	static uint32_t V3_API unrefFn(void* self);

	static v3_result V3_API getMidiControllerAssignment(void* self, int32_t bus, int16_t channel,
	    int16_t cc, v3_param_id* id);
};


// =====================================================================================
// Edit controller — tearoff del component
// =====================================================================================
//
// Due ragioni per esistere: dare all'host un create_view() da cui aprire la GUI, ed esporre
// i parametri MIDI-CC + IMidiMapping così che l'host instradi qui CC/pitch-bend/aftertouch
// (i soli "parametri" che dichiariamo: vedi kMidiControllersPerChannel).

struct RackEditController : v3_edit_controller_cpp {
	std::atomic_int refcounter;
	RackComponent* component;
	RackMidiMapping* midiMapping = nullptr; // creato pigramente, posseduto qui

	RackEditController(RackComponent* c);
	~RackEditController();

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface);
	static uint32_t V3_API refFn(void* self);
	static uint32_t V3_API unrefFn(void* self);

	static v3_result V3_API initialize(void* self, v3_funknown** context);
	static v3_result V3_API terminate(void* self);

	static v3_result V3_API setComponentState(void* self, v3_bstream** stream);
	static v3_result V3_API setState(void* self, v3_bstream** stream);
	static v3_result V3_API getState(void* self, v3_bstream** stream);
	static int32_t V3_API getParameterCount(void* self);
	static v3_result V3_API getParameterInfo(void* self, int32_t paramIdx, v3_param_info* info);
	static v3_result V3_API getParameterStringForValue(void* self, v3_param_id id, double normalised,
	    v3_str_128 output);
	static v3_result V3_API getParameterValueForString(void* self, v3_param_id id, int16_t* input,
	    double* output);
	static double V3_API normalisedParameterToPlain(void* self, v3_param_id id, double normalised);
	static double V3_API plainParameterToNormalised(void* self, v3_param_id id, double plain);
	static double V3_API getParameterNormalised(void* self, v3_param_id id);
	static v3_result V3_API setParameterNormalised(void* self, v3_param_id id, double normalised);
	static v3_result V3_API setComponentHandler(void* self, v3_component_handler** handler);
	static v3_plugin_view** V3_API createView(void* self, const char* name);
};


struct RackAudioProcessor : v3_audio_processor_cpp {
	std::atomic_int refcounter;
	RackComponent* component;

	RackAudioProcessor(RackComponent* c);

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface);
	static uint32_t V3_API refFn(void* self);
	static uint32_t V3_API unrefFn(void* self);

	static v3_result V3_API setBusArrangements(void* self, v3_speaker_arrangement* inputs,
	    int32_t numInputs, v3_speaker_arrangement* outputs, int32_t numOutputs);
	static v3_result V3_API getBusArrangement(void* self, int32_t busDirection, int32_t idx,
	    v3_speaker_arrangement* arr);
	static v3_result V3_API canProcessSampleSize(void* self, int32_t symbolicSampleSize);
	static uint32_t V3_API getLatencySamples(void* self);
	static v3_result V3_API setupProcessing(void* self, v3_process_setup* setup);
	static v3_result V3_API setProcessing(void* self, v3_bool state);
	static v3_result V3_API process(void* self, v3_process_data* data);
	static uint32_t V3_API getTailSamples(void* self);
};


// =====================================================================================
// Component — l'oggetto principale
// =====================================================================================
//
// Scegliamo un SINGLE COMPONENT EFFECT: una sola classe nella factory, e questo stesso
// oggetto espone IComponent + IAudioProcessor (+ IEditController, Fase 4). Perciò
// get_controller_class_id restituisce V3_NOT_IMPLEMENTED: l'host interroga il component
// stesso. Evita la coppia component/controller da appaiare via IConnectionPoint e — cosa
// che conta davvero per noi — lascia al controller la strada diretta verso la finestra
// accessibile della SUA istanza.

struct RackComponent : v3_component_cpp {
	std::atomic_int refcounter;
	RackAudioProcessor* processor = nullptr; // &processor è il T** consegnato all'host
	RackEditController* controller = nullptr; // idem
	rackhost::Instance* rack = nullptr;      // nullo finché l'host non chiama initialize()

	// Il puntatore-a-puntatore che create_instance ha consegnato all'host come IComponent.
	// Serve a unrefFn per liberare SEMPRE l'allocazione giusta, anche quando l'ultimo rilascio
	// arriva da un handle tearoff (processor/controller/midi-mapping) invece che dall'handle
	// del component stesso (vedi la nota sull'identità COM sopra unrefFn).
	RackComponent** selfPtr = nullptr;

	// Formato negoziato con l'host. setup_processing può arrivare prima di set_active,
	// quindi lo memorizziamo e attiviamo il core quando entrambi sono noti.
	double sampleRate = 44100.0;
	uint32_t maxBlockSize = 512;

	RackComponent();
	~RackComponent();

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface);
	static uint32_t V3_API refFn(void* self);
	static uint32_t V3_API unrefFn(void* self);

	static v3_result V3_API initialize(void* self, v3_funknown** context);
	static v3_result V3_API terminate(void* self);

	static v3_result V3_API getControllerClassId(void* self, v3_tuid classId);
	static v3_result V3_API setIoMode(void* self, int32_t ioMode);
	static int32_t V3_API getBusCount(void* self, int32_t mediaType, int32_t busDirection);
	static v3_result V3_API getBusInfo(void* self, int32_t mediaType, int32_t busDirection,
	                                   int32_t busIdx, v3_bus_info* info);
	static v3_result V3_API getRoutingInfo(void* self, v3_routing_info* input, v3_routing_info* output);
	static v3_result V3_API activateBus(void* self, int32_t mediaType, int32_t busDirection,
	                                    int32_t busIdx, v3_bool state);
	static v3_result V3_API setActive(void* self, v3_bool state);
	static v3_result V3_API setState(void* self, v3_bstream** stream);
	static v3_result V3_API getState(void* self, v3_bstream** stream);
};


// --- plug view: implementazione --------------------------------------------------------

#if defined ARCH_WIN

// Classe Win32 del pannellino segnaposto. Registrata pigramente alla prima attached().
const wchar_t* const kPlaceholderClass = L"MetarackVst3Placeholder";

// Piccolo localizzatore, gemello del T() dell'AccessibleWindow (quelli sono static lì, non
// raggiungibili da qui): segue la stessa lingua scelta dalla UI del plugin. Fallback inglese.
const wchar_t* T(const wchar_t* en, const wchar_t* it) {
	return rack::settings::language == "it" ? it : en;
}

#else

// Gemello UTF-8 di T() per il lato Cocoa (le stringhe di AppKit non sono wide).
const char* Tu8(const char* en, const char* it) {
	return rack::settings::language == "it" ? it : en;
}

#endif

// Porta il focus dalla view dell'host alla finestra accessibile, l'unica UI leggibile.
// Il rimbalzo può rientrare (l'host reagisce alla perdita di focus rifocalizzando la
// propria view): la guardia lo taglia dopo un giro.
bool bouncingFocus = false;

void bounceFocusToAccessibleWindow(RackPlugView* v);

#if defined ARCH_WIN

LRESULT CALLBACK placeholderWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_CREATE: {
			CREATESTRUCTW* cs = (CREATESTRUCTW*) lp;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) cs->lpCreateParams);
			return 0;
		}
		// L'host ci ha dato il focus (F6 in Reaper, click sull'editor): non è qui che
		// l'utente deve stare.
		case WM_SETFOCUS:
			bounceFocusToAccessibleWindow((RackPlugView*) GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			return 0;
		// Rete di sicurezza se il rimbalzo automatico non scatta: Invio/Spazio ci riprovano.
		case WM_KEYDOWN:
			if (wp == VK_RETURN || wp == VK_SPACE) {
				bounceFocusToAccessibleWindow((RackPlugView*) GetWindowLongPtrW(hwnd, GWLP_USERDATA));
				return 0;
			}
			break;
		case WM_PAINT: {
			// Solo un riquadro grigio: il messaggio vive nel nome-finestra MSAA (vedi attached),
			// l'unico canale che NVDA annuncia in modo affidabile. Dipingerlo anche qui lo
			// duplicherebbe (NVDA legge il testo GDI via display model).
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
			EndPaint(hwnd, &ps);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerPlaceholderClass() {
	static bool registered = false;
	if (registered)
		return;
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = placeholderWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kPlaceholderClass;
	RegisterClassExW(&wc);
	registered = true;
}

#endif // ARCH_WIN — WndProc e classe del segnaposto

void bounceFocusToAccessibleWindow(RackPlugView* v) {
	if (bouncingFocus || !v || !v->component->rack)
		return;
#if defined ARCH_WIN
	// The user just pressed F6 to leave: don't fight them by pulling MetaRack forward when
	// the host refocuses its editor in response.
	if (rackhost::isBounceSuppressed(v->component->rack))
		return;
#endif
	bouncingFocus = true;
	// Windows: guiShow fa ShowWindow + SetForegroundWindow + SetFocus sulla finestra
	// accessibile; siamo in-process con l'host, che in questo momento è in primo piano,
	// quindi SetForegroundWindow non viene rifiutato. macOS: makeKeyAndOrderFront sul
	// pannello, che è una finestra dello stesso processo — nessun ostacolo da aggirare.
	rackhost::guiShow(v->component->rack);
	bouncingFocus = false;
}

#if !defined ARCH_WIN
// Il segnaposto Cocoa è C puro verso di noi (vst3_mac.hpp): questo è il ponte verso il
// rimbalzo, con la view come userData.
void placeholderFocusThunk(void* userData) {
	bounceFocusToAccessibleWindow((RackPlugView*) userData);
}
#endif

RackPlugView::RackPlugView(RackComponent* c) : refcounter(1), component(c) {
	// v3_funknown
	query_interface = queryInterface;
	ref = refFn;
	unref = unrefFn;
	// v3_plugin_view
	view.is_platform_type_supported = isPlatformTypeSupported;
	view.attached = RackPlugView::attached;
	view.removed = RackPlugView::removed;
	view.on_wheel = onWheel;
	view.on_key_down = onKeyDown;
	view.on_key_up = onKeyUp;
	view.get_size = getSize;
	view.on_size = onSize;
	view.on_focus = onFocus;
	view.set_frame = setFrame;
	view.can_resize = canResize;
	view.check_size_constraint = checkSizeConstraint;
}

RackPlugView::~RackPlugView() {
	if (!placeholder)
		return;
#if defined ARCH_WIN
	DestroyWindow(placeholder);
#else
	metarack::vst3mac::destroyPlaceholder(placeholder);
#endif
	placeholder = nullptr;
}

v3_result V3_API RackPlugView::queryInterface(void* self, const v3_tuid iid, void** iface) {
	RackPlugView* v = *static_cast<RackPlugView**>(self);
	if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_view_iid)) {
		++v->refcounter;
		*iface = self;
		return V3_OK;
	}
	*iface = nullptr;
	return V3_NO_INTERFACE;
}

uint32_t V3_API RackPlugView::refFn(void* self) {
	return ++(*static_cast<RackPlugView**>(self))->refcounter;
}

uint32_t V3_API RackPlugView::unrefFn(void* self) {
	RackPlugView** vptr = static_cast<RackPlugView**>(self);
	RackPlugView* v = *vptr;
	const int refcount = --v->refcounter;
	if (refcount > 0)
		return (uint32_t) refcount;
	// La view possiede sé stessa: l'host l'ha rilasciata, quindi sparisce (col puntatore
	// allocato da createView).
	delete v;
	delete vptr;
	return 0;
}

v3_result V3_API RackPlugView::isPlatformTypeSupported(void* /*self*/, const char* platformType) {
	// _NATIVE è HWND su Windows e NSView su macOS: l'unico tipo che sappiamo incorporare
	// è quello della piattaforma su cui siamo compilati.
	return (platformType && std::strcmp(platformType, V3_VIEW_PLATFORM_TYPE_NATIVE) == 0)
	       ? V3_OK : V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackPlugView::attached(void* self, void* parent, const char* platformType) {
	RackPlugView* v = *static_cast<RackPlugView**>(self);
	if (!platformType || std::strcmp(platformType, V3_VIEW_PLATFORM_TYPE_NATIVE) != 0)
		return V3_NOT_IMPLEMENTED;
	if (!v->component->rack)
		return V3_NOT_INITIALIZED;

#if !defined ARCH_WIN
	// macOS: il segnaposto è una NSView con dentro una static text (vst3_mac.mm). Stessa
	// funzione della sua gemella Win32 — dire dov'è la UI e rimbalzare il focus — ma senza
	// classe da registrare, perché AppKit non ne ha bisogno.
	v->placeholder = metarack::vst3mac::createPlaceholder(
	                   parent,
	                   Tu8("MetaRack — the UI is the MetaRack window; Command+` switches back to the DAW",
	                       "MetaRack — la UI è la finestra MetaRack; Comando+` riporta alla DAW"),
	                   placeholderFocusThunk, v);
	// Portiamo il pannello in primo piano (era nascosto finché l'host non apre l'editor).
	rackhost::guiShow(v->component->rack);
	return V3_OK;
#else

	// Il segnaposto nell'editor dell'host: non è la UI, è solo ciò che l'host si aspetta di
	// poter incorporare.
	registerPlaceholderClass();
	// Il testo della finestra è il nome MSAA che NVDA legge se il focus si ferma qui;
	// WS_TABSTOP la rende raggiungibile, e lpCreateParams le dà la view su cui rimbalzare.
	v->placeholder = CreateWindowExW(0, kPlaceholderClass,
	                                 T(L"MetaRack — the UI is the MetaRack window, reachable with Alt+Tab; F6 returns to the DAW",
	                                   L"MetaRack — la UI è la finestra MetaRack, raggiungibile con Alt+Tab; F6 riporta alla finestra della DAW"),
	                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
	                                 0, 0, kWidth, kHeight, (HWND) parent, nullptr,
	                                 GetModuleHandleW(nullptr), v);

	// Registra la finestra radice dell'host come bersaglio del ritorno con F6 (parent è una
	// finestra figlia dentro l'editor, quindi risaliamo alla radice). NON rende MetaRack
	// posseduta dall'host: la proprietà la faceva sparire da Alt+Tab dopo F6 (vedi rackhost).
	rackhost::guiSetTransient(v->component->rack, GetAncestor((HWND) parent, GA_ROOT));
	// Portiamo MetaRack in primo piano (era nascosta finché l'host non apre l'editor).
	rackhost::guiShow(v->component->rack);
	return V3_OK;
#endif
}

v3_result V3_API RackPlugView::removed(void* self) {
	RackPlugView* v = *static_cast<RackPlugView**>(self);
	if (v->component->rack)
		rackhost::guiHide(v->component->rack);
	if (v->placeholder) {
#if defined ARCH_WIN
		DestroyWindow(v->placeholder);
#else
		metarack::vst3mac::destroyPlaceholder(v->placeholder);
#endif
		v->placeholder = nullptr;
	}
	return V3_OK;
}

v3_result V3_API RackPlugView::onWheel(void* /*self*/, float /*distance*/) {
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackPlugView::onKeyDown(void* /*self*/, int16_t /*keyChar*/, int16_t /*keyCode*/,
    int16_t /*modifiers*/) {
	// I tasti li gestisce la finestra accessibile, che ha il focus: non ci interessano
	// quelli diretti all'editor dell'host.
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackPlugView::onKeyUp(void* /*self*/, int16_t /*keyChar*/, int16_t /*keyCode*/,
                                       int16_t /*modifiers*/) {
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackPlugView::getSize(void* /*self*/, v3_view_rect* rect) {
	// La dimensione del segnaposto, non della finestra accessibile: è quella che l'host
	// incorpora.
	rect->left = 0;
	rect->top = 0;
	rect->right = kWidth;
	rect->bottom = kHeight;
	return V3_OK;
}

v3_result V3_API RackPlugView::onSize(void* self, v3_view_rect* rect) {
	RackPlugView* v = *static_cast<RackPlugView**>(self);
	if (v->placeholder) {
#if defined ARCH_WIN
		MoveWindow(v->placeholder, rect->left, rect->top,
		           rect->right - rect->left, rect->bottom - rect->top, TRUE);
#else
		metarack::vst3mac::resizePlaceholder(v->placeholder, rect->left, rect->top,
		                                     rect->right - rect->left, rect->bottom - rect->top);
#endif
	}
	return V3_OK;
}

v3_result V3_API RackPlugView::onFocus(void* self, v3_bool state) {
	// Secondo canale del rimbalzo: alcuni host focalizzano l'editor senza mai dare il focus
	// Win32 al nostro segnaposto, quindi WM_SETFOCUS da solo non basta.
	if (state)
		bounceFocusToAccessibleWindow(*static_cast<RackPlugView**>(self));
	return V3_OK;
}

v3_result V3_API RackPlugView::setFrame(void* /*self*/, v3_plugin_frame** /*frame*/) {
	// Non chiediamo mai un resize all'host, quindi non ci serve tenere il frame.
	return V3_OK;
}

v3_result V3_API RackPlugView::canResize(void* /*self*/) {
	return V3_FALSE;
}

v3_result V3_API RackPlugView::checkSizeConstraint(void* /*self*/, v3_view_rect* rect) {
	rect->left = 0;
	rect->top = 0;
	rect->right = kWidth;
	rect->bottom = kHeight;
	return V3_OK;
}


// --- midi mapping: implementazione -----------------------------------------------------

RackMidiMapping::RackMidiMapping(RackComponent* c) : refcounter(1), component(c) {
	// v3_funknown
	query_interface = queryInterface;
	ref = refFn;
	unref = unrefFn;
	// v3_midi_mapping
	map.get_midi_controller_assignment = getMidiControllerAssignment;
}

v3_result V3_API RackMidiMapping::queryInterface(void* self, const v3_tuid iid, void** iface) {
	RackMidiMapping* m = *static_cast<RackMidiMapping**>(self);
	if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_midi_mapping_iid)) {
		++m->component->refcounter; // identità COM: la ref conta sul component
		*iface = self;
		return V3_OK;
	}
	*iface = nullptr;
	return V3_NO_INTERFACE;
}

// Tearoff: la vita è quella del component (identità COM). ref/unref NON toccano un contatore
// proprio ma inoltrano a quello del component, così il component non può morire mentre l'host
// tiene ancora questa interfaccia.
uint32_t V3_API RackMidiMapping::refFn(void* self) {
	RackMidiMapping* m = *static_cast<RackMidiMapping**>(self);
	return RackComponent::refFn(&m->component);
}

uint32_t V3_API RackMidiMapping::unrefFn(void* self) {
	RackMidiMapping* m = *static_cast<RackMidiMapping**>(self);
	return RackComponent::unrefFn(&m->component);
}

v3_result V3_API RackMidiMapping::getMidiControllerAssignment(void* /*self*/, int32_t bus,
    int16_t channel, int16_t cc, v3_param_id* id) {
	// Un solo event bus (bus 0). cc 0..129: 0..127 = CC, 128 = channel pressure, 129 = pitch-
	// bend. L'host chiama questo a init per costruire la mappa MIDI->parametri.
	if (bus != 0 || channel < 0 || channel >= 16 || cc < 0 || cc >= kMidiControllersPerChannel)
		return V3_FALSE;
	*id = (v3_param_id)(channel * kMidiControllersPerChannel + cc);
	return V3_TRUE;
}


// --- edit controller: implementazione --------------------------------------------------

RackEditController::~RackEditController() {
	delete midiMapping;
	midiMapping = nullptr;
}

RackEditController::RackEditController(RackComponent* c) : refcounter(1), component(c) {
	// v3_funknown
	query_interface = queryInterface;
	ref = refFn;
	unref = unrefFn;
	// v3_plugin_base
	base.initialize = RackEditController::initialize;
	base.terminate = RackEditController::terminate;
	// v3_edit_controller
	ctrl.set_component_state = setComponentState;
	ctrl.set_state = RackEditController::setState;
	ctrl.get_state = RackEditController::getState;
	ctrl.get_parameter_count = getParameterCount;
	ctrl.get_parameter_info = getParameterInfo;
	ctrl.get_parameter_string_for_value = getParameterStringForValue;
	ctrl.get_parameter_value_for_string = getParameterValueForString;
	ctrl.normalised_parameter_to_plain = normalisedParameterToPlain;
	ctrl.plain_parameter_to_normalised = plainParameterToNormalised;
	ctrl.get_parameter_normalised = getParameterNormalised;
	ctrl.set_parameter_normalised = setParameterNormalised;
	ctrl.set_component_handler = setComponentHandler;
	ctrl.create_view = createView;
}

v3_result V3_API RackEditController::queryInterface(void* self, const v3_tuid iid, void** iface) {
	RackEditController* c = *static_cast<RackEditController**>(self);
	if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_base_iid)
	    || v3_tuid_match(iid, v3_edit_controller_iid)) {
		++c->component->refcounter; // identità COM: la ref conta sul component
		*iface = self;
		return V3_OK;
	}
	if (v3_tuid_match(iid, v3_midi_mapping_iid)) {
		// Il membro puntatore è esso stesso il T** da consegnare (vedi l'idioma in testa).
		if (!c->midiMapping)
			c->midiMapping = new RackMidiMapping(c->component);
		++c->component->refcounter; // identità COM: anche IMidiMapping conta sul component
		*iface = &c->midiMapping;
		return V3_OK;
	}
	*iface = nullptr;
	return V3_NO_INTERFACE;
}

// Tearoff: la vita è quella del component (identità COM). Inoltriamo al suo refcounter, così
// l'host può rilasciare IComponent/IEditController/IAudioProcessor in qualsiasi ordine senza
// che il component (e questo controller, che ne è membro) venga distrutto troppo presto.
uint32_t V3_API RackEditController::refFn(void* self) {
	RackEditController* c = *static_cast<RackEditController**>(self);
	return RackComponent::refFn(&c->component);
}

uint32_t V3_API RackEditController::unrefFn(void* self) {
	RackEditController* c = *static_cast<RackEditController**>(self);
	return RackComponent::unrefFn(&c->component);
}

v3_result V3_API RackEditController::initialize(void* /*self*/, v3_funknown** /*context*/) {
	// Il vero init è quello del component (single component effect): qui niente da fare.
	return V3_OK;
}

v3_result V3_API RackEditController::terminate(void* /*self*/) {
	return V3_OK;
}

v3_result V3_API RackEditController::setComponentState(void* /*self*/, v3_bstream** /*stream*/) {
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackEditController::setState(void* /*self*/, v3_bstream** /*stream*/) {
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackEditController::getState(void* /*self*/, v3_bstream** /*stream*/) {
	return V3_NOT_IMPLEMENTED;
}

int32_t V3_API RackEditController::getParameterCount(void* /*self*/) {
	// Gli UNICI parametri sono i controller MIDI (CC/pitch-bend/aftertouch): non manopole del
	// rack, ma il canale che l'host usa per instradarci quei controller (vedi IMidiMapping).
	return kMidiCCParamCount;
}

v3_result V3_API RackEditController::getParameterInfo(void* /*self*/, int32_t paramIdx,
    v3_param_info* info) {
	if (paramIdx < 0 || paramIdx >= kMidiCCParamCount)
		return V3_INVALID_ARG;
	std::memset(info, 0, sizeof(*info));
	const int32_t channel = paramIdx / kMidiControllersPerChannel;
	const int32_t cc = paramIdx % kMidiControllersPerChannel;
	info->param_id = (v3_param_id) paramIdx;
	// HIDDEN: servono solo perché l'host instradi qui il MIDI via IMidiMapping, non per
	// l'automazione manuale — il flag li tiene fuori dalla lista parametri mostrata all'utente.
	info->flags = V3_PARAM_CAN_AUTOMATE | V3_PARAM_IS_HIDDEN;
	info->step_count = 127;
	// Pitch-bend a riposo = centro (0.5); CC e aftertouch a riposo = 0.
	info->default_normalised_value = (cc == 129) ? 0.5 : 0.0;
	char name[32];
	if (cc == 128)
		std::snprintf(name, sizeof(name), "MIDI Ch.%d Pressure", channel + 1);
	else if (cc == 129)
		std::snprintf(name, sizeof(name), "MIDI Ch.%d PitchBend", channel + 1);
	else
		std::snprintf(name, sizeof(name), "MIDI Ch.%d CC%d", channel + 1, cc);
	copyUtf16(info->title, name, sizeof(info->title) / sizeof(info->title[0]));
	copyUtf16(info->short_title, name, sizeof(info->short_title) / sizeof(info->short_title[0]));
	return V3_OK;
}

v3_result V3_API RackEditController::getParameterStringForValue(void* /*self*/, v3_param_id id,
    double /*normalised*/, v3_str_128 output) {
	// Parametri MIDI nascosti: nessuna stringa utile, ma rispondiamo OK con stringa vuota per
	// gli id validi così l'host non tratta la chiamata come errore.
	if (id >= (v3_param_id) kMidiCCParamCount)
		return V3_INVALID_ARG;
	output[0] = 0;
	return V3_OK;
}

v3_result V3_API RackEditController::getParameterValueForString(void* /*self*/, v3_param_id /*id*/,
    int16_t* /*input*/, double* /*output*/) {
	return V3_INVALID_ARG;
}

double V3_API RackEditController::normalisedParameterToPlain(void* /*self*/, v3_param_id /*id*/,
    double normalised) {
	return normalised;
}

double V3_API RackEditController::plainParameterToNormalised(void* /*self*/, v3_param_id /*id*/,
    double plain) {
	return plain;
}

double V3_API RackEditController::getParameterNormalised(void* /*self*/, v3_param_id id) {
	// Non teniamo una cache: i valori MIDI li consumiamo dal process data (input_params), non
	// da qui. Rispondiamo con il valore a riposo — centro per il pitch-bend, 0 per gli altri.
	if (id < (v3_param_id) kMidiCCParamCount && (id % kMidiControllersPerChannel) == 129)
		return 0.5;
	return 0.0;
}

v3_result V3_API RackEditController::setParameterNormalised(void* /*self*/, v3_param_id id,
    double /*normalised*/) {
	// Accettiamo la notifica dell'host (edit-controller sync) ma non la usiamo: l'audio legge i
	// controller da input_params in process(). Rispondere V3_OK evita che l'host la creda fallita.
	return (id < (v3_param_id) kMidiCCParamCount) ? V3_OK : V3_INVALID_ARG;
}

v3_result V3_API RackEditController::setComponentHandler(void* /*self*/,
    v3_component_handler** /*handler*/) {
	// Serve solo per notificare modifiche ai parametri: non ne abbiamo.
	return V3_OK;
}

v3_plugin_view** V3_API RackEditController::createView(void* self, const char* name) {
	RackEditController* c = *static_cast<RackEditController**>(self);
	if (name && std::strcmp(name, "editor") != 0)
		return nullptr;
	// La view si possiede da sé: vedi l'idioma in testa per la coppia oggetto/puntatore.
	RackPlugView** viewptr = new RackPlugView*;
	*viewptr = new RackPlugView(c->component);
	return (v3_plugin_view**) viewptr;
}


// --- audio processor: implementazione -------------------------------------------------

RackAudioProcessor::RackAudioProcessor(RackComponent* c)
	: refcounter(1), component(c) {
	// v3_funknown
	query_interface = queryInterface;
	ref = refFn;
	unref = unrefFn;
	// v3_audio_processor
	proc.set_bus_arrangements = setBusArrangements;
	proc.get_bus_arrangement = getBusArrangement;
	proc.can_process_sample_size = canProcessSampleSize;
	proc.get_latency_samples = getLatencySamples;
	proc.setup_processing = setupProcessing;
	proc.set_processing = setProcessing;
	proc.process = RackAudioProcessor::process;
	proc.get_tail_samples = getTailSamples;
}

v3_result V3_API RackAudioProcessor::queryInterface(void* self, const v3_tuid iid, void** iface) {
	RackAudioProcessor* p = *static_cast<RackAudioProcessor**>(self);
	if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_audio_processor_iid)) {
		++p->component->refcounter; // identità COM: la ref conta sul component
		*iface = self;
		return V3_OK;
	}
	*iface = nullptr;
	return V3_NO_INTERFACE;
}

// Tearoff: la vita è quella del component (identità COM). Inoltriamo al suo refcounter — è
// proprio la ref del processor che, tenuta dall'host oltre il rilascio di IComponent, causava
// l'use-after-free alla chiusura: ora il component non muore finché anche questa è viva.
uint32_t V3_API RackAudioProcessor::refFn(void* self) {
	RackAudioProcessor* p = *static_cast<RackAudioProcessor**>(self);
	return RackComponent::refFn(&p->component);
}

uint32_t V3_API RackAudioProcessor::unrefFn(void* self) {
	RackAudioProcessor* p = *static_cast<RackAudioProcessor**>(self);
	return RackComponent::unrefFn(&p->component);
}

v3_result V3_API RackAudioProcessor::setBusArrangements(void* /*self*/,
    v3_speaker_arrangement* inputs, int32_t numInputs,
    v3_speaker_arrangement* outputs, int32_t numOutputs) {
	// Accettiamo esattamente kStereoBuses bus stereo per direzione: è il formato del ponte
	// verso il modulo Core Audio-16 (ogni bus = una coppia di canali del device DAW).
	const v3_speaker_arrangement stereo = V3_SPEAKER_L | V3_SPEAKER_R;
	if (numInputs != kStereoBuses || numOutputs != kStereoBuses)
		return V3_FALSE;
	for (int32_t i = 0; i < numInputs; i++)
		if (inputs[i] != stereo)
			return V3_FALSE;
	for (int32_t i = 0; i < numOutputs; i++)
		if (outputs[i] != stereo)
			return V3_FALSE;
	return V3_OK;
}

v3_result V3_API RackAudioProcessor::getBusArrangement(void* /*self*/, int32_t /*busDirection*/,
    int32_t idx, v3_speaker_arrangement* arr) {
	if (idx < 0 || idx >= kStereoBuses)
		return V3_INVALID_ARG;
	*arr = V3_SPEAKER_L | V3_SPEAKER_R;
	return V3_OK;
}

v3_result V3_API RackAudioProcessor::canProcessSampleSize(void* /*self*/, int32_t symbolicSampleSize) {
	// Solo 32 bit: l'engine di Rack e audio::Device::processBuffer lavorano in float.
	return (symbolicSampleSize == V3_SAMPLE_32) ? V3_OK : V3_FALSE;
}

uint32_t V3_API RackAudioProcessor::getLatencySamples(void* /*self*/) {
	return 0;
}

v3_result V3_API RackAudioProcessor::setupProcessing(void* self, v3_process_setup* setup) {
	RackAudioProcessor* p = *static_cast<RackAudioProcessor**>(self);
	if (setup->symbolic_sample_size != V3_SAMPLE_32)
		return V3_FALSE;
	p->component->sampleRate = setup->sample_rate;
	p->component->maxBlockSize = (uint32_t) setup->max_block_size;
	return V3_OK;
}

v3_result V3_API RackAudioProcessor::setProcessing(void* /*self*/, v3_bool /*state*/) {
	return V3_OK;
}

// Converte un evento VST3 in messaggio(i) MIDI grezzi e lo spinge nel driver "DAW".
// VST3 non trasporta MIDI grezzo: astrae le note in eventi strutturati (note_on/off,
// poly_pressure) e i dati binari in V3_EVENT_DATA (SysEx). Qui li ri-serializziamo nei byte
// MIDI che i moduli Core di Rack capiscono.
//
// LIMITE NOTO: CC, pitch-bend e mod-wheel NON arrivano come eventi in VST3 — passano da
// IMidiMapping (MIDI CC -> parametri) e da input_params, non ancora implementati. Per ora il
// ponte copre note e aftertouch polifonico: sufficiente a SUONARE il rack dalla tastiera del
// DAW. CC/PB sono una fase successiva.
void pushVst3Event(rackhost::Instance* rack, const v3_event& ev) {
	// velocità/pressione VST3 sono float 0..1; MIDI vuole 0..127. Arrotonda e satura.
	auto to7bit = [](float v) -> uint8_t {
		int x = (int)(v * 127.f + 0.5f);
		return (uint8_t)(x < 0 ? 0 : (x > 127 ? 127 : x));
	};
	uint8_t msg[3];
	switch (ev.type) {
		case V3_EVENT_NOTE_ON:
			msg[0] = (uint8_t)(0x90 | (ev.note_on.channel & 0x0f));
			msg[1] = (uint8_t)(ev.note_on.pitch & 0x7f);
			msg[2] = to7bit(ev.note_on.velocity);
			rackhost::pushMidiMessage(rack, msg, 3, ev.sample_offset);
			break;
		case V3_EVENT_NOTE_OFF:
			msg[0] = (uint8_t)(0x80 | (ev.note_off.channel & 0x0f));
			msg[1] = (uint8_t)(ev.note_off.pitch & 0x7f);
			msg[2] = to7bit(ev.note_off.velocity);
			rackhost::pushMidiMessage(rack, msg, 3, ev.sample_offset);
			break;
		case V3_EVENT_POLY_PRESSURE:
			msg[0] = (uint8_t)(0xA0 | (ev.poly_pressure.channel & 0x0f));
			msg[1] = (uint8_t)(ev.poly_pressure.pitch & 0x7f);
			msg[2] = to7bit(ev.poly_pressure.pressure);
			rackhost::pushMidiMessage(rack, msg, 3, ev.sample_offset);
			break;
		case V3_EVENT_DATA:
			// SysEx e simili: inoltra i byte grezzi così come sono.
			if (ev.data.bytes && ev.data.size > 0)
				rackhost::pushMidiMessage(rack, ev.data.bytes, (int) ev.data.size, ev.sample_offset);
			break;
		default:
			// chord, scale, note-expression: non hanno un equivalente MIDI diretto, ignorati.
			break;
	}
}

// Converte un parameter change (MIDI CC/pitch-bend/aftertouch instradato dall'host via
// IMidiMapping) nel messaggio MIDI grezzo corrispondente e lo spinge nel driver "DAW". `id` è
// l'id parametro `canale*130 + controller`; `normalized` è 0..1. Inversa di DPF appendCC.
void pushVst3ParamChange(rackhost::Instance* rack, uint32_t id, int32_t sampleOffset,
                         double normalized) {
	if (id >= (uint32_t) kMidiCCParamCount)
		return;
	const int channel = (int) id / kMidiControllersPerChannel;
	const int cc = (int) id % kMidiControllersPerChannel;
	if (normalized < 0.0)
		normalized = 0.0;
	if (normalized > 1.0)
		normalized = 1.0;

	uint8_t msg[3];
	if (cc < 128) {
		// Control change: [Bn, cc, valore 7-bit].
		msg[0] = (uint8_t)(0xB0 | channel);
		msg[1] = (uint8_t) cc;
		msg[2] = (uint8_t)(int)(normalized * 127.0 + 0.5);
		rackhost::pushMidiMessage(rack, msg, 3, sampleOffset);
	}
	else if (cc == 128) {
		// Channel pressure (aftertouch di canale): messaggio a DUE byte [Dn, valore 7-bit].
		msg[0] = (uint8_t)(0xD0 | channel);
		msg[1] = (uint8_t)(int)(normalized * 127.0 + 0.5);
		rackhost::pushMidiMessage(rack, msg, 2, sampleOffset);
	}
	else {
		// Pitch-bend: valore a 14 bit, LSB poi MSB. [En, LSB, MSB]. 0.5 -> 8192 = centro.
		int val14 = (int)(normalized * 16384.0);
		if (val14 < 0)
			val14 = 0;
		if (val14 > 16383)
			val14 = 16383; // 16384 traboccherebbe l'MSB a 128 (bit alto = byte MIDI non valido)
		msg[0] = (uint8_t)(0xE0 | channel);
		msg[1] = (uint8_t)(val14 & 0x7f);
		msg[2] = (uint8_t)((val14 >> 7) & 0x7f);
		rackhost::pushMidiMessage(rack, msg, 3, sampleOffset);
	}
}

v3_result V3_API RackAudioProcessor::process(void* self, v3_process_data* data) {
	RackAudioProcessor* p = *static_cast<RackAudioProcessor**>(self);
	rackhost::Instance* rack = p->component->rack;
	if (!rack)
		return V3_NOT_INITIALIZED;
	if (data->symbolic_sample_size != V3_SAMPLE_32)
		return V3_FALSE;

	// MIDI PRIMA dell'audio: pushMidiMessage timestampa con engine->getFrame() (= frame
	// d'inizio blocco finché stepBlock non gira), quindi va spinto prima di processPlanar, che
	// avvia lo stepBlock che consuma i messaggi in questo stesso blocco. L'event list è un
	// oggetto dell'host: si chiama con l'idioma travesty v3_cpp_obj(handle)->m(handle).
	if (data->input_events) {
		v3_event_list** elist = data->input_events;
		const uint32_t count = v3_cpp_obj(elist)->get_event_count(elist);
		for (uint32_t i = 0; i < count; i++) {
			v3_event ev;
			if (v3_cpp_obj(elist)->get_event(elist, (int32_t) i, &ev) == V3_OK)
				pushVst3Event(rack, ev);
		}
	}

	// CC / pitch-bend / channel-pressure arrivano QUI, non come eventi: l'host li manda come
	// parameter change sugli id mappati da IMidiMapping. Ogni coda di parametro può avere più
	// punti (valori a offset di campione diversi nel blocco): li spingiamo tutti. La InputQueue
	// del modulo Core MIDI riordina per frame, quindi non serve fondere con le note.
	if (data->input_params) {
		v3_param_changes** pchanges = data->input_params;
		const int32_t pcount = v3_cpp_obj(pchanges)->get_param_count(pchanges);
		for (int32_t i = 0; i < pcount; i++) {
			v3_param_value_queue** queue = v3_cpp_obj(pchanges)->get_param_data(pchanges, i);
			if (!queue)
				continue;
			const v3_param_id id = v3_cpp_obj(queue)->get_param_id(queue);
			if (id >= (v3_param_id) kMidiCCParamCount)
				continue; // non è uno dei nostri parametri MIDI
			const int32_t points = v3_cpp_obj(queue)->get_point_count(queue);
			for (int32_t j = 0; j < points; j++) {
				int32_t offset = 0;
				double normalized = 0.0;
				if (v3_cpp_obj(queue)->get_point(queue, j, &offset, &normalized) != V3_OK)
					break;
				pushVst3ParamChange(rack, id, offset, normalized);
			}
		}
	}

	// VST3 usa buffer PLANARI (channel_buffers_32[canale][frame]) e un array per ogni bus.
	// rackhost::processPlanar vuole invece UN array piatto di kNumChannels puntatori-per-
	// canale, quindi srotoliamo qui gli 8 bus stereo in ingresso e in uscita. Gli array
	// nascono azzerati ({}), così un bus non presentato dall'host (bypass, o coppia non
	// allocata) resta silenzio: tutto NULL-safe.
	const float* inChans[rackhost::kNumChannels] = {};
	float* outChans[rackhost::kNumChannels] = {};

	for (int32_t b = 0; data->inputs && b < data->num_input_buses && b < kStereoBuses; b++) {
		const v3_audio_bus_buffers& bus = data->inputs[b];
		for (int32_t c = 0; c < bus.num_channels && c < 2; c++)
			inChans[b * 2 + c] = bus.channel_buffers_32 ? bus.channel_buffers_32[c] : nullptr;
	}
	for (int32_t b = 0; data->outputs && b < data->num_output_buses && b < kStereoBuses; b++) {
		const v3_audio_bus_buffers& bus = data->outputs[b];
		for (int32_t c = 0; c < bus.num_channels && c < 2; c++)
			outChans[b * 2 + c] = bus.channel_buffers_32 ? bus.channel_buffers_32[c] : nullptr;
	}

	rackhost::processPlanar(rack, inChans, rackhost::kNumChannels,
	                        outChans, rackhost::kNumChannels, (uint32_t) data->nframes);
	return V3_OK;
}

uint32_t V3_API RackAudioProcessor::getTailSamples(void* /*self*/) {
	// Un rack può suonare all'infinito (è anche uno strumento): coda infinita.
	return 0xFFFFFFFF;
}


// --- component: implementazione --------------------------------------------------------

RackComponent::RackComponent() : refcounter(1) {
	// v3_funknown
	query_interface = queryInterface;
	ref = refFn;
	unref = unrefFn;
	// v3_plugin_base
	base.initialize = RackComponent::initialize;
	base.terminate = RackComponent::terminate;
	// v3_component
	comp.get_controller_class_id = getControllerClassId;
	comp.set_io_mode = setIoMode;
	comp.get_bus_count = getBusCount;
	comp.get_bus_info = getBusInfo;
	comp.get_routing_info = getRoutingInfo;
	comp.activate_bus = activateBus;
	comp.set_active = setActive;
	comp.set_state = RackComponent::setState;
	comp.get_state = RackComponent::getState;
}

RackComponent::~RackComponent() {
	delete processor;
	processor = nullptr;
	delete controller;
	controller = nullptr;
}

v3_result V3_API RackComponent::queryInterface(void* self, const v3_tuid iid, void** iface) {
	RackComponent* c = *static_cast<RackComponent**>(self);

	if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_base_iid)
	    || v3_tuid_match(iid, v3_component_iid)) {
		++c->refcounter;
		*iface = self;
		return V3_OK;
	}

	if (v3_tuid_match(iid, v3_audio_processor_iid)) {
		// Il membro puntatore è esso stesso il T** da consegnare (vedi l'idioma in testa).
		// L'oggetto tearoff si crea pigramente, ma la REF conta sempre sul component (identità
		// COM): è l'unico refcounter che governa la vita di tutto.
		if (!c->processor)
			c->processor = new RackAudioProcessor(c);
		++c->refcounter;
		*iface = &c->processor;
		return V3_OK;
	}

	if (v3_tuid_match(iid, v3_edit_controller_iid)) {
		// Single component effect: il controller è nostro, non una classe separata.
		if (!c->controller)
			c->controller = new RackEditController(c);
		++c->refcounter;
		*iface = &c->controller;
		return V3_OK;
	}

	*iface = nullptr;
	return V3_NO_INTERFACE;
}

uint32_t V3_API RackComponent::refFn(void* self) {
	return ++(*static_cast<RackComponent**>(self))->refcounter;
}

uint32_t V3_API RackComponent::unrefFn(void* self) {
	RackComponent* c = *static_cast<RackComponent**>(self);
	const int refcount = --c->refcounter;
	if (refcount > 0)
		return (uint32_t) refcount;

	// IDENTITÀ COM: questo refcounter conta TUTTE le interfacce che l'host ha ottenuto sullo
	// stesso oggetto logico — IComponent, IAudioProcessor, IEditController, IMidiMapping — che
	// inoltrano tutte qui (vedi i tearoff). Arrivati a zero l'host non ne tiene più nessuna,
	// quindi è ora sicuro distruggere l'oggetto (e con esso i membri processor/controller).
	//
	// Liberiamo SEMPRE selfPtr, il RackComponent** che create_instance ha dato all'host: `self`
	// qui potrebbe essere l'indirizzo di un membro (&component->processor) se l'ultimo rilascio
	// è arrivato da un handle tearoff, e delete su quello sarebbe un disastro.
	RackComponent** cptr = c->selfPtr;
	delete c;
	delete cptr;
	return 0;
}

v3_result V3_API RackComponent::initialize(void* self, v3_funknown** /*context*/) {
	RackComponent* c = *static_cast<RackComponent**>(self);
	if (c->rack)
		return V3_OK; // già inizializzato: l'host non dovrebbe, ma non è un errore fatale

	// La spec VST3 chiama initialize() sul main thread (UI thread): è qui che rackhost può
	// inizializzare GLFW, non nel caricamento della DLL.
	c->rack = rackhost::createInstance();
	if (!c->rack)
		return V3_INTERNAL_ERR;

	// Auto-show: la finestra MetaRack c'è appena istanzi il plugin, senza aspettare che il
	// DAW apra l'editor (che con IPlugView potrebbe non aprire mai). attached() la riporta
	// poi in primo piano.
	rackhost::guiShow(c->rack);
	return V3_OK;
}

v3_result V3_API RackComponent::terminate(void* self) {
	RackComponent* c = *static_cast<RackComponent**>(self);
	if (c->rack) {
		rackhost::destroyInstance(c->rack);
		c->rack = nullptr;
	}
	return V3_OK;
}

v3_result V3_API RackComponent::getControllerClassId(void* /*self*/, v3_tuid /*classId*/) {
	// Single component effect: nessuna classe controller separata, l'host interroga noi.
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackComponent::setIoMode(void* /*self*/, int32_t /*ioMode*/) {
	return V3_NOT_IMPLEMENTED;
}

int32_t V3_API RackComponent::getBusCount(void* /*self*/, int32_t mediaType, int32_t busDirection) {
	// kStereoBuses bus audio stereo per direzione. Più UN bus di eventi in INGRESSO: il MIDI
	// del DAW verso il rack. Nessun bus di eventi in uscita (Rack -> DAW MIDI non è nel task).
	if (mediaType == V3_AUDIO)
		return kStereoBuses;
	if (mediaType == V3_EVENT)
		return (busDirection == V3_INPUT) ? 1 : 0;
	return 0;
}

v3_result V3_API RackComponent::getBusInfo(void* /*self*/, int32_t mediaType, int32_t busDirection,
    int32_t busIdx, v3_bus_info* info) {
	// Bus di eventi: un solo bus MIDI in ingresso, 16 canali (i canali MIDI del device "DAW").
	if (mediaType == V3_EVENT) {
		if (busDirection != V3_INPUT || busIdx != 0)
			return V3_INVALID_ARG;
		std::memset(info, 0, sizeof(*info));
		info->media_type = V3_EVENT;
		info->direction = V3_INPUT;
		info->channel_count = 16;
		copyUtf16(info->bus_name, "MIDI In", sizeof(info->bus_name) / sizeof(info->bus_name[0]));
		info->bus_type = V3_MAIN;
		info->flags = V3_DEFAULT_ACTIVE;
		return V3_OK;
	}
	if (mediaType != V3_AUDIO || busIdx < 0 || busIdx >= kStereoBuses)
		return V3_INVALID_ARG;
	std::memset(info, 0, sizeof(*info));
	info->media_type = V3_AUDIO;
	info->direction = busDirection;
	info->channel_count = 2;
	// Nome 1-based come lo conta un DAW: bus 0 -> "1/2", bus 1 -> "3/4", ...
	char name[32];
	std::snprintf(name, sizeof(name), "%s %d/%d",
	              (busDirection == V3_INPUT) ? "In" : "Out", busIdx * 2 + 1, busIdx * 2 + 2);
	copyUtf16(info->bus_name, name, sizeof(info->bus_name) / sizeof(info->bus_name[0]));
	// Bus 0 = principale, gli altri ausiliari. Tutti default-attivi, così l'host li alloca e
	// li presenta a process() senza che l'utente debba abilitarli a mano.
	info->bus_type = (busIdx == 0) ? V3_MAIN : V3_AUX;
	info->flags = V3_DEFAULT_ACTIVE;
	return V3_OK;
}

v3_result V3_API RackComponent::getRoutingInfo(void* /*self*/, v3_routing_info* /*input*/,
    v3_routing_info* /*output*/) {
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackComponent::activateBus(void* /*self*/, int32_t /*mediaType*/,
    int32_t /*busDirection*/, int32_t /*busIdx*/, v3_bool /*state*/) {
	// I nostri bus sono sempre attivi: accettiamo senza fare nulla.
	return V3_OK;
}

v3_result V3_API RackComponent::setActive(void* self, v3_bool state) {
	RackComponent* c = *static_cast<RackComponent**>(self);
	if (!c->rack)
		return V3_NOT_INITIALIZED;
	if (state)
		return rackhost::activate(c->rack, c->sampleRate, c->maxBlockSize) ? V3_OK : V3_INTERNAL_ERR;
	rackhost::deactivate(c->rack);
	return V3_OK;
}

v3_result V3_API RackComponent::setState(void* /*self*/, v3_bstream** /*stream*/) {
	// v1: la patch non si salva nel progetto del DAW (vedi il piano, "Rischi noti").
	return V3_NOT_IMPLEMENTED;
}

v3_result V3_API RackComponent::getState(void* /*self*/, v3_bstream** /*stream*/) {
	return V3_NOT_IMPLEMENTED;
}


// =====================================================================================
// Factory
// =====================================================================================

struct RackFactory : v3_plugin_factory_cpp {
	RackFactory() {
		// v3_funknown — la factory è un singleton statico: ref/unref non contano nulla.
		query_interface = queryInterface;
		ref = refFn;
		unref = unrefFn;
		// v3_plugin_factory
		v1.get_factory_info = getFactoryInfo;
		v1.num_classes = numClasses;
		v1.get_class_info = getClassInfo;
		v1.create_instance = createInstance;
		// v3_plugin_factory_2
		v2.get_class_info_2 = getClassInfo2;
		// v3_plugin_factory_3
		v3.get_class_info_utf16 = getClassInfoUtf16;
		v3.set_host_context = setHostContext;
	}

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface) {
		if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_factory_iid)
		    || v3_tuid_match(iid, v3_plugin_factory_2_iid) || v3_tuid_match(iid, v3_plugin_factory_3_iid)) {
			*iface = self;
			return V3_OK;
		}
		*iface = nullptr;
		return V3_NO_INTERFACE;
	}
	static uint32_t V3_API refFn(void*) {
		return 1;
	}
	static uint32_t V3_API unrefFn(void*) {
		return 1;
	}

	static v3_result V3_API getFactoryInfo(void*, v3_factory_info* info) {
		std::memset(info, 0, sizeof(*info));
		copyAscii(info->vendor, kVendor, sizeof(info->vendor));
		info->flags = 0x10; // unicode
		return V3_OK;
	}

	static int32_t V3_API numClasses(void*) {
		// Single component effect: una sola classe.
		return 1;
	}

	static v3_result V3_API getClassInfo(void*, int32_t idx, v3_class_info* info) {
		if (idx != 0)
			return V3_INVALID_ARG;
		std::memset(info, 0, sizeof(*info));
		std::memcpy(info->class_id, g_classTuid, sizeof(v3_tuid));
		info->cardinality = 0x7FFFFFFF;
		copyAscii(info->category, "Audio Module Class", sizeof(info->category));
		copyAscii(info->name, kPluginName, sizeof(info->name));
		return V3_OK;
	}

	static v3_result V3_API getClassInfo2(void*, int32_t idx, v3_class_info_2* info) {
		if (idx != 0)
			return V3_INVALID_ARG;
		std::memset(info, 0, sizeof(*info));
		std::memcpy(info->class_id, g_classTuid, sizeof(v3_tuid));
		info->cardinality = 0x7FFFFFFF;
		copyAscii(info->category, "Audio Module Class", sizeof(info->category));
		copyAscii(info->name, kPluginName, sizeof(info->name));
		copyAscii(info->sub_categories, kSubCategories, sizeof(info->sub_categories));
		copyAscii(info->vendor, kVendor, sizeof(info->vendor));
		copyAscii(info->version, kVersion, sizeof(info->version));
		copyAscii(info->sdk_version, "VST 3.7.4", sizeof(info->sdk_version));
		return V3_OK;
	}

	static v3_result V3_API getClassInfoUtf16(void*, int32_t idx, v3_class_info_3* info) {
		if (idx != 0)
			return V3_INVALID_ARG;
		std::memset(info, 0, sizeof(*info));
		std::memcpy(info->class_id, g_classTuid, sizeof(v3_tuid));
		info->cardinality = 0x7FFFFFFF;
		copyAscii(info->category, "Audio Module Class", sizeof(info->category));
		copyUtf16(info->name, kPluginName, sizeof(info->name) / sizeof(info->name[0]));
		copyAscii(info->sub_categories, kSubCategories, sizeof(info->sub_categories));
		copyUtf16(info->vendor, kVendor, sizeof(info->vendor) / sizeof(info->vendor[0]));
		copyUtf16(info->version, kVersion, sizeof(info->version) / sizeof(info->version[0]));
		copyUtf16(info->sdk_version, "VST 3.7.4",
		          sizeof(info->sdk_version) / sizeof(info->sdk_version[0]));
		return V3_OK;
	}

	static v3_result V3_API setHostContext(void*, v3_funknown** /*host*/) {
		// Non ci serve nulla dall'host (niente parametri, niente messaggi).
		return V3_NOT_IMPLEMENTED;
	}

	static v3_result V3_API createInstance(void* /*self*/, const v3_tuid classId,
	                                       const v3_tuid iid, void** instance) {
		if (!v3_tuid_match(classId, g_classTuid))
			return V3_NO_INTERFACE;
		if (!v3_tuid_match(iid, v3_component_iid) && !v3_tuid_match(iid, v3_funknown_iid))
			return V3_NO_INTERFACE;

		// Vedi l'idioma in testa: all'host va un puntatore a un puntatore all'oggetto,
		// quindi il puntatore stesso è allocato sullo heap e vive quanto l'oggetto.
		RackComponent** componentptr = new RackComponent*;
		*componentptr = new RackComponent;
		// unrefFn libererà questo stesso puntatore quando l'ultima interfaccia sarà rilasciata,
		// da qualunque handle arrivi il rilascio finale (vedi l'identità COM lì).
		(*componentptr)->selfPtr = componentptr;
		*instance = static_cast<void*>(componentptr);
		return V3_OK;
	}
};


RackFactory g_factory;
RackFactory* g_factoryPtr = &g_factory;


} // anonymous namespace


// =====================================================================================
// Entry-point del modulo
// =====================================================================================
//
// Gli host VST3 su Windows chiamano InitDll() dopo il LoadLibrary e ExitDll() prima dello
// scarico; su macOS il bundle espone invece bundleEntry()/bundleExit(), che il loader chiama
// con il CFBundleRef del nostro .vst3 (non ci serve: le risorse le troviamo da dladdr, vedi
// rackhost::getModuleDir). GetPluginFactory() sta in mezzo su entrambe le piattaforme.
// rackhost::retainProcess/releaseProcess sono refcounted, quindi né una coppia sbilanciata né
// i bundleEntry ripetuti che la spec ammette fanno danni.

#if defined ARCH_WIN
	#define METARACK_VST3_EXPORT __declspec(dllexport)
#else
	#define METARACK_VST3_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

#if defined ARCH_WIN

	METARACK_VST3_EXPORT bool InitDll(void) {
		return rackhost::retainProcess("vst3-log.txt");
	}

	METARACK_VST3_EXPORT bool ExitDll(void) {
		rackhost::releaseProcess();
		return true;
	}

#else

	METARACK_VST3_EXPORT bool bundleEntry(void* /*CFBundleRef*/) {
		return rackhost::retainProcess("vst3-log.txt");
	}

	METARACK_VST3_EXPORT bool bundleExit(void) {
		rackhost::releaseProcess();
		return true;
	}

#endif

	METARACK_VST3_EXPORT const void* GetPluginFactory(void) {
		// L'host riceve sempre un T**: qui il puntatore al singleton statico.
		return &g_factoryPtr;
	}

	// Solo per l'harness di test (vst3test): quanti messaggi MIDI il driver "DAW" ha ricevuto
	// dall'host. NON fa parte dell'ABI VST3 — un DAW non chiama mai questo simbolo.
	METARACK_VST3_EXPORT uint64_t MetarackDebugMidiCount(void) {
		return rackhost::debugMidiMessageCount();
	}

} // extern "C"
