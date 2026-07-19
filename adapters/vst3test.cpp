// Dev harness — mini-host VST3 da console per testare adapters/vst3.cpp SENZA un DAW.
//
// Carica il bundle MetaRack.vst3, esegue l'intero ciclo di vita VST3 (InitDll -> factory ->
// create_instance -> initialize -> setup_processing -> set_active -> alcuni process() ->
// teardown) e stampa i log INFO di Rack.
//
// Con un ABI VST3 scritto a mano (travesty) questo harness non è un lusso: è l'unico modo di
// distinguere "il DAW non lo vede" da "l'ABI è sbagliato". Non fa parte del prodotto.
//
// Build: target Makefile `vst3test` (console, niente -mwindows). Esegui dalla root C:\Rack.

#include "vst3/travesty/audio_processor.h"
#include "vst3/travesty/component.h"
#include "vst3/travesty/edit_controller.h"
#include "vst3/travesty/factory.h"
#include "vst3/travesty/view.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>


#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FALLITO: %s\n", msg); return 1; } } while (0)

// Deve combaciare con g_classTuid in adapters/vst3.cpp.
static constexpr const v3_tuid kClassTuid = V3_ID(0x4D657461, 0x5261636B, 0x436F6D70, 0x6F6E656E);

// Percorso del modulo dentro il bundle. Di default quello buildato in-tree; si può passare
// come argomento la cartella .vst3 di un bundle INSTALLATO, che è l'unico test onesto della
// risoluzione delle dipendenze: questo eseguibile è linkato staticamente e non ha bisogno di
// nulla accanto a sé, quindi se il bundle non è autosufficiente il caricamento fallisce come
// fallirebbe in un DAW.
//
// Uso: RackVst3Test.exe ["C:\percorso\MetaRack.vst3"]
static const wchar_t* const kDefaultBundle = L"MetaRack.vst3";
static std::wstring g_bundleArg;

typedef bool (*InitDllFn)(void);
typedef bool (*ExitDllFn)(void);
typedef const void* (*GetPluginFactoryFn)(void);


static void printAscii(const char* label, const char* s) {
	std::printf("%s%s\n", label, s);
}

// I campi "unicode" della factory VST3 sono UTF-16 in int16_t: per i nostri nomi ASCII
// basta prendere il byte basso.
static void printUtf16(const char* label, const int16_t* s) {
	std::printf("%s", label);
	for (int i = 0; i < 128 && s[i]; i++)
		std::putchar((char) s[i]);
	std::putchar('\n');
}


// Finto IEventList dell'host, per testare il ponte MIDI. Mima l'event list che un DAW passa
// in process_data.input_events: espone un piccolo array di v3_event. Layout ABI come gli
// oggetti dell'adapter — [funknown][metodi] — consegnato al plugin come T** (vedi l'idioma
// in testa a vst3.cpp).
struct TestEventList : v3_event_list_cpp {
	const std::vector<v3_event>* events;

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface) {
		if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_event_list_iid)) {
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
	static uint32_t V3_API getEventCount(void* self) {
		TestEventList* o = *static_cast<TestEventList**>(self);
		return (uint32_t) o->events->size();
	}
	static v3_result V3_API getEvent(void* self, int32_t idx, v3_event* event) {
		TestEventList* o = *static_cast<TestEventList**>(self);
		if (idx < 0 || (size_t) idx >= o->events->size())
			return V3_INVALID_ARG;
		*event = (*o->events)[idx];
		return V3_OK;
	}
	static v3_result V3_API addEvent(void*, v3_event*) {
		return V3_NOT_IMPLEMENTED;
	}

	explicit TestEventList(const std::vector<v3_event>* evs) : events(evs) {
		query_interface = queryInterface;
		ref = refFn;
		unref = unrefFn;
		list.get_event_count = getEventCount;
		list.get_event = getEvent;
		list.add_event = addEvent;
	}
};


// Finta coda di valori di UN parametro (IParamValueQueue): un id + una lista di punti
// (sample offset, valore normalizzato). Serve a simulare CC/pitch-bend che l'host manda come
// parameter change. Stessa idea ABI di TestEventList.
struct TestParamQueue : v3_param_value_queue_cpp {
	v3_param_id id;
	std::vector<std::pair<int32_t, double>> points;

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface) {
		if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_value_queue_iid)) {
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
	static v3_param_id V3_API getParamId(void* self) {
		return (*static_cast<TestParamQueue**>(self))->id;
	}
	static int32_t V3_API getPointCount(void* self) {
		return (int32_t)(*static_cast<TestParamQueue**>(self))->points.size();
	}
	static v3_result V3_API getPoint(void* self, int32_t idx, int32_t* sampleOffset, double* value) {
		TestParamQueue* o = *static_cast<TestParamQueue**>(self);
		if (idx < 0 || (size_t) idx >= o->points.size())
			return V3_INVALID_ARG;
		*sampleOffset = o->points[idx].first;
		*value = o->points[idx].second;
		return V3_OK;
	}
	static v3_result V3_API addPoint(void*, int32_t, double, int32_t*) {
		return V3_NOT_IMPLEMENTED;
	}

	TestParamQueue(v3_param_id pid, std::vector<std::pair<int32_t, double>> pts)
		: id(pid), points(std::move(pts)) {
		query_interface = queryInterface;
		ref = refFn;
		unref = unrefFn;
		queue.get_param_id = getParamId;
		queue.get_point_count = getPointCount;
		queue.get_point = getPoint;
		queue.add_point = addPoint;
	}
};


// Finto IParameterChanges che espone UNA sola coda di parametro (basta per il test).
struct TestParamChanges : v3_param_changes_cpp {
	TestParamQueue* queue; // consegnato all'host come (v3_param_value_queue**) &queue

	static v3_result V3_API queryInterface(void* self, const v3_tuid iid, void** iface) {
		if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_changes_iid)) {
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
	static int32_t V3_API getParamCount(void*) {
		return 1;
	}
	static v3_param_value_queue** V3_API getParamData(void* self, int32_t idx) {
		TestParamChanges* o = *static_cast<TestParamChanges**>(self);
		if (idx != 0)
			return nullptr;
		return (v3_param_value_queue**) &o->queue;
	}
	static v3_param_value_queue** V3_API addParamData(void*, const v3_param_id*, int32_t*) {
		return nullptr;
	}

	explicit TestParamChanges(TestParamQueue* q) : queue(q) {
		query_interface = queryInterface;
		ref = refFn;
		unref = unrefFn;
		changes.get_param_count = getParamCount;
		changes.get_param_data = getParamData;
		changes.add_param_data = addParamData;
	}
};


int main() {
	// Su Windows il rack parte vuoto (il modulo Audio c'è, ma non è cablato a nulla: è
	// l'utente a instradare i moduli dalla finestra accessibile), quindi senza questo
	// l'uscita sarebbe silenziosa per costruzione e il test del ponte audio non direbbe
	// nulla. La variabile fa aggiungere a rackhost i cavi di loopback DAW in -> DAW out.
	// Va impostata PRIMA del LoadLibrary: la legge l'init dell'istanza.
	SetEnvironmentVariableA("METARACK_TEST_PASSTHROUGH", "1");

	const std::wstring bundle = g_bundleArg.empty() ? kDefaultBundle : g_bundleArg;
	const std::wstring modulePath = bundle + L"\\Contents\\x86_64-win\\MetaRack.vst3";
	std::printf("== RackVst3Test: carico %ls ==\n", modulePath.c_str());

	// Carichiamo con LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, cioè la politica di ricerca DLL più
	// RESTRITTIVA che un host reale possa usare: cartella dell'applicazione + System32 + user
	// dirs, e NON la cartella del modulo caricato.
	//
	// È deliberato ed è il cuore del test. Con LOAD_WITH_ALTERED_SEARCH_PATH (più permissivo)
	// il test passava mentre né Reaper né Ableton riuscivano a caricare il plugin: un bundle
	// che dipende da DLL affiancate si carica solo se l'host è generoso. Testando col caso
	// peggiore, il gate verifica che il bundle sia davvero autosufficiente — che è il lavoro
	// dello stub (adapters/vst3stub.c).
	HMODULE dll = LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!dll) {
		std::printf("FALLITO: LoadLibraryExW del bundle, GetLastError=%lu\n", GetLastError());
		std::printf("  (126 = ERROR_MOD_NOT_FOUND: il modulo NON è autosufficiente — dipende da\n");
		std::printf("   DLL affiancate che un host con ricerca ristretta non vede. È esattamente\n");
		std::printf("   il motivo per cui i DAW non vedevano il plugin.)\n");
		return 1;
	}

	InitDllFn initDll = (InitDllFn) GetProcAddress(dll, "InitDll");
	ExitDllFn exitDll = (ExitDllFn) GetProcAddress(dll, "ExitDll");
	GetPluginFactoryFn getFactory = (GetPluginFactoryFn) GetProcAddress(dll, "GetPluginFactory");
	CHECK(getFactory, "simbolo GetPluginFactory non trovato");

	if (initDll) {
		std::printf("InitDll()...\n");
		CHECK(initDll(), "InitDll ha fallito");
	}

	v3_plugin_factory** factory = (v3_plugin_factory**) getFactory();
	CHECK(factory, "GetPluginFactory ha restituito NULL");

	v3_factory_info finfo = {};
	v3_cpp_obj(factory)->get_factory_info(factory, &finfo);
	printAscii("vendor: ", finfo.vendor);

	const int32_t numClasses = v3_cpp_obj(factory)->num_classes(factory);
	std::printf("classi disponibili: %d\n", (int) numClasses);
	CHECK(numClasses == 1, "attesa esattamente 1 classe (single component effect)");

	// La factory v2 dà le informazioni interessanti (sub-categorie, versione).
	v3_plugin_factory_2** factory2 = nullptr;
	v3_cpp_obj_query_interface(factory, v3_plugin_factory_2_iid, &factory2);
	CHECK(factory2, "la factory non espone l'interfaccia v2");

	v3_class_info_2 cinfo = {};
	CHECK(v3_cpp_obj(factory2)->get_class_info_2(factory2, 0, &cinfo) == V3_OK, "get_class_info_2 fallita");
	printAscii("  name: ", cinfo.name);
	printAscii("  category: ", cinfo.category);
	printAscii("  sub_categories: ", cinfo.sub_categories);
	printAscii("  version: ", cinfo.version);
	CHECK(v3_tuid_match(cinfo.class_id, kClassTuid), "il class_id non combacia con quello atteso");

	// La factory v3 espone gli stessi dati in UTF-16: verifichiamo che non siano vuoti,
	// perché è ciò che molti host mostrano davvero all'utente.
	v3_plugin_factory_3** factory3 = nullptr;
	v3_cpp_obj_query_interface(factory, v3_plugin_factory_3_iid, &factory3);
	if (factory3) {
		v3_class_info_3 cinfo3 = {};
		if (v3_cpp_obj(factory3)->get_class_info_utf16(factory3, 0, &cinfo3) == V3_OK)
			printUtf16("  name (utf16): ", cinfo3.name);
	}

	std::printf("create_instance()...\n");
	v3_component** component = nullptr;
	CHECK(v3_cpp_obj(factory)->create_instance(factory, kClassTuid, v3_component_iid,
	      (void**) &component) == V3_OK && component,
	      "create_instance fallita");

	std::printf("initialize()...\n");
	CHECK(v3_cpp_obj_initialize(component, nullptr) == V3_OK, "initialize fallita");

	// Bus dichiarati. Deve combaciare con l'adapter: rackhost::kNumChannels/2 bus stereo per
	// direzione (16 canali → 8 bus). Costante locale per non tirarci dentro rackhost.hpp.
	const int32_t kStereoBuses = 8;
	const int32_t numIn = v3_cpp_obj(component)->get_bus_count(component, V3_AUDIO, V3_INPUT);
	const int32_t numOut = v3_cpp_obj(component)->get_bus_count(component, V3_AUDIO, V3_OUTPUT);
	std::printf("bus audio: %d in, %d out\n", (int) numIn, (int) numOut);
	CHECK(numIn == kStereoBuses && numOut == kStereoBuses, "attesi kStereoBuses bus audio in e out");

	for (int dir = 0; dir < 2; dir++) {
		for (int32_t b = 0; b < kStereoBuses; b++) {
			v3_bus_info binfo = {};
			CHECK(v3_cpp_obj(component)->get_bus_info(component, V3_AUDIO, dir, b, &binfo) == V3_OK,
			      "get_bus_info fallita");
			char label[32];
			std::snprintf(label, sizeof(label),
			              dir == V3_INPUT ? "  IN  bus %d: " : "  OUT bus %d: ", (int) b);
			printUtf16(label, binfo.bus_name);
			CHECK(binfo.channel_count == 2, "atteso un bus stereo");
		}
	}

	// L'audio processor si ottiene interrogando il component (single component effect).
	v3_audio_processor** processor = nullptr;
	v3_cpp_obj_query_interface(component, v3_audio_processor_iid, &processor);
	CHECK(processor, "il component non espone IAudioProcessor");

	CHECK(v3_cpp_obj(processor)->can_process_sample_size(processor, V3_SAMPLE_32) == V3_OK,
	      "il processor rifiuta i sample a 32 bit");
	CHECK(v3_cpp_obj(processor)->can_process_sample_size(processor, V3_SAMPLE_64) != V3_OK,
	      "il processor accetta i 64 bit, ma non li implementa");

	std::vector<v3_speaker_arrangement> arr(kStereoBuses, V3_SPEAKER_L | V3_SPEAKER_R);
	CHECK(v3_cpp_obj(processor)->set_bus_arrangements(
	        processor, arr.data(), kStereoBuses, arr.data(), kStereoBuses) == V3_OK,
	      "set_bus_arrangements 8x stereo rifiutata");

	const int32_t kBlock = 512;
	const double kSampleRate = 48000.0;

	std::printf("setup_processing(%g, %d)...\n", kSampleRate, (int) kBlock);
	v3_process_setup setup = {};
	setup.process_mode = V3_REALTIME;
	setup.symbolic_sample_size = V3_SAMPLE_32;
	setup.max_block_size = kBlock;
	setup.sample_rate = kSampleRate;
	CHECK(v3_cpp_obj(processor)->setup_processing(processor, &setup) == V3_OK, "setup_processing fallita");

	std::printf("set_active(true)...\n");
	CHECK(v3_cpp_obj(component)->set_active(component, true) == V3_OK, "set_active fallita");
	v3_cpp_obj(processor)->set_processing(processor, true);

	std::printf("process() con seno 440Hz in ingresso (attendo il pass-through)...\n");

	// Un buffer L/R per ciascuno degli 8 bus, in ingresso e in uscita: presentiamo all'host
	// TUTTI i bus (come farebbe un DAW multi-out reale). Solo il bus 0 riceve il seno; gli
	// altri restano a zero. Il pass-through si legge dall'uscita del bus 0.
	std::vector<float> inCh[kStereoBuses * 2];
	std::vector<float> outCh[kStereoBuses * 2];
	for (int c = 0; c < kStereoBuses * 2; c++) {
		inCh[c].assign(kBlock, 0.f);
		outCh[c].assign(kBlock, 0.f);
	}

	float* inPtrs[kStereoBuses][2];
	float* outPtrs[kStereoBuses][2];
	v3_audio_bus_buffers inBuses[kStereoBuses] = {};
	v3_audio_bus_buffers outBuses[kStereoBuses] = {};
	for (int b = 0; b < kStereoBuses; b++) {
		inPtrs[b][0] = inCh[b * 2 + 0].data();
		inPtrs[b][1] = inCh[b * 2 + 1].data();
		outPtrs[b][0] = outCh[b * 2 + 0].data();
		outPtrs[b][1] = outCh[b * 2 + 1].data();
		inBuses[b].num_channels = 2;
		inBuses[b].channel_buffers_32 = inPtrs[b];
		outBuses[b].num_channels = 2;
		outBuses[b].channel_buffers_32 = outPtrs[b];
	}

	double phase = 0.0;
	const double phaseInc = 2.0 * 3.14159265358979323846 * 440.0 / kSampleRate;
	double maxRms = 0.0;

	for (int block = 0; block < 24; block++) {
		// Seno continuo di ampiezza 0.5 sui due canali del bus 0.
		for (int i = 0; i < kBlock; i++) {
			const float s = (float)(0.5 * std::sin(phase));
			inCh[0][i] = s;
			inCh[1][i] = s;
			phase += phaseInc;
		}
		for (int c = 0; c < kStereoBuses * 2; c++)
			std::fill(outCh[c].begin(), outCh[c].end(), 0.f);

		v3_process_data data = {};
		data.process_mode = V3_REALTIME;
		data.symbolic_sample_size = V3_SAMPLE_32;
		data.nframes = kBlock;
		data.num_input_buses = kStereoBuses;
		data.num_output_buses = kStereoBuses;
		data.inputs = inBuses;
		data.outputs = outBuses;

		const v3_result res = v3_cpp_obj(processor)->process(processor, &data);

		double sum = 0.0;
		for (int i = 0; i < kBlock; i++)
			sum += (double) outCh[0][i] * outCh[0][i];
		const double rms = std::sqrt(sum / kBlock);
		if (rms > maxRms)
			maxRms = rms;
		if (block < 6 || rms > 0.01)
			std::printf("  block[%02d] res=%d outRMS=%f\n", block, (int) res, rms);
	}

	std::printf("RMS d'uscita massimo osservato: %f (atteso ~0.35 per un seno 0.5)\n", maxRms);
	const bool passthrough = maxRms > 0.01;
	if (passthrough)
		std::printf("PASS-THROUGH OK: l'engine ha instradato l'audio dal DAW in -> DAW out.\n");
	else
		std::printf("ATTENZIONE: uscita silenziosa, il pass-through non ha prodotto segnale.\n");

	// --- ponte MIDI: eventi VST3 -> driver "DAW" --------------------------------------
	// L'adapter dichiara un event input bus e traduce gli eventi in messaggi MIDI grezzi che
	// spinge nel driver "DAW". Qui simuliamo un DAW che manda note-on/note-off e verifichiamo,
	// via il simbolo di debug esportato, che siano arrivati al driver. Nessun modulo MIDI è
	// sottoscritto (è l'utente a instradarli): il contatore sale comunque a monte del device.
	std::printf("MIDI: invio note-on/note-off via IEventList...\n");
	// Il simbolo di debug vive nell'ADAPTER (RackVst3Adapter.dll), non nello stub che abbiamo
	// caricato: InitDll ha già caricato l'adapter nel processo, quindi ne prendiamo l'handle
	// per nome (GetModuleHandle non incrementa il refcount, ma il modulo resta vivo).
	typedef uint64_t (*MidiCountFn)(void);
	HMODULE adapter = GetModuleHandleW(L"RackVst3Adapter.dll");
	CHECK(adapter, "RackVst3Adapter.dll non risulta caricato");
	MidiCountFn midiCount = (MidiCountFn)(void*) GetProcAddress(adapter, "MetarackDebugMidiCount");
	CHECK(midiCount, "simbolo MetarackDebugMidiCount non trovato");

	std::vector<v3_event> events;
	v3_event noteOn = {};
	noteOn.type = V3_EVENT_NOTE_ON;
	noteOn.sample_offset = 0;
	noteOn.note_on.channel = 0;
	noteOn.note_on.pitch = 60;      // C4
	noteOn.note_on.velocity = 0.8f;
	events.push_back(noteOn);
	v3_event noteOff = {};
	noteOff.type = V3_EVENT_NOTE_OFF;
	noteOff.sample_offset = 128;    // a metà blocco: verifica il timestamp sample-accurate
	noteOff.note_off.channel = 0;
	noteOff.note_off.pitch = 60;
	noteOff.note_off.velocity = 0.f;
	events.push_back(noteOff);

	TestEventList evList(&events);
	TestEventList* evListPtr = &evList;

	const uint64_t midiBefore = midiCount();

	for (int c = 0; c < kStereoBuses * 2; c++)
		std::fill(outCh[c].begin(), outCh[c].end(), 0.f);
	v3_process_data mdata = {};
	mdata.process_mode = V3_REALTIME;
	mdata.symbolic_sample_size = V3_SAMPLE_32;
	mdata.nframes = kBlock;
	mdata.num_input_buses = kStereoBuses;
	mdata.num_output_buses = kStereoBuses;
	mdata.inputs = inBuses;
	mdata.outputs = outBuses;
	mdata.input_events = (v3_event_list**) &evListPtr;
	const v3_result midiRes = v3_cpp_obj(processor)->process(processor, &mdata);

	const uint64_t midiDelta = midiCount() - midiBefore;
	std::printf("  process res=%d, messaggi MIDI ricevuti dal driver: %llu (attesi 2)\n",
	            (int) midiRes, (unsigned long long) midiDelta);
	const bool midiOk = (midiDelta == 2);
	if (midiOk)
		std::printf("MIDI OK: gli eventi VST3 sono arrivati al driver \"DAW\".\n");
	else
		std::printf("ATTENZIONE: il ponte MIDI non ha consegnato gli eventi attesi.\n");

	// --- pitch-bend: parameter change via IMidiMapping --------------------------------
	// CC/pitch-bend NON arrivano come eventi: l'host li manda come parameter change sugli id
	// che il plugin dichiara via IMidiMapping. Verifichiamo la mappatura e che un pitch-bend
	// (param change) arrivi al driver.
	std::printf("MIDI: pitch-bend come parameter change (IMidiMapping)...\n");
	v3_edit_controller** midiCtrl = nullptr;
	v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &midiCtrl);
	CHECK(midiCtrl, "il component non espone IEditController per il MIDI mapping");
	CHECK(v3_cpp_obj(midiCtrl)->get_parameter_count(midiCtrl) == 130 * 16,
	      "atteso 130*16 parametri MIDI (CC/pitch-bend/aftertouch)");

	v3_midi_mapping** mapping = nullptr;
	v3_cpp_obj_query_interface(midiCtrl, v3_midi_mapping_iid, &mapping);
	CHECK(mapping, "il controller non espone IMidiMapping");
	// Pitch-bend sul canale 0: controller 129 -> id parametro 0*130+129 = 129.
	v3_param_id pbId = 0xffffffff;
	CHECK(v3_cpp_obj(mapping)->get_midi_controller_assignment(mapping, 0, 0, 129, &pbId) == V3_TRUE
	      && pbId == 129, "get_midi_controller_assignment del pitch-bend non torna l'id atteso");

	// 0.75 normalizzato -> bend verso l'alto. Una coda di un solo punto a offset 0.
	TestParamQueue pbQueue(pbId, { { 0, 0.75 } });
	TestParamQueue* pbQueuePtr = &pbQueue;
	(void) pbQueuePtr; // il puntatore vive dentro TestParamChanges via &queue
	TestParamChanges pbChanges(&pbQueue);
	TestParamChanges* pbChangesPtr = &pbChanges;

	const uint64_t pbBefore = midiCount();
	for (int c = 0; c < kStereoBuses * 2; c++)
		std::fill(outCh[c].begin(), outCh[c].end(), 0.f);
	v3_process_data pbData = {};
	pbData.process_mode = V3_REALTIME;
	pbData.symbolic_sample_size = V3_SAMPLE_32;
	pbData.nframes = kBlock;
	pbData.num_input_buses = kStereoBuses;
	pbData.num_output_buses = kStereoBuses;
	pbData.inputs = inBuses;
	pbData.outputs = outBuses;
	pbData.input_params = (v3_param_changes**) &pbChangesPtr;
	const v3_result pbRes = v3_cpp_obj(processor)->process(processor, &pbData);

	const uint64_t pbDelta = midiCount() - pbBefore;
	std::printf("  process res=%d, messaggi da parameter change: %llu (atteso 1: il pitch-bend)\n",
	            (int) pbRes, (unsigned long long) pbDelta);
	const bool pbOk = (pbDelta == 1);
	if (pbOk)
		std::printf("PITCH-BEND OK: il parameter change è arrivato al driver \"DAW\".\n");
	else
		std::printf("ATTENZIONE: il pitch-bend non ha raggiunto il driver.\n");

	v3_cpp_obj_unref(mapping);
	v3_cpp_obj_unref(midiCtrl);

	// --- GUI: edit controller + plug view ---------------------------------------------
	// Single component effect: il controller si ottiene dal component, non da una classe
	// separata della factory.
	std::printf("query IEditController...\n");
	v3_edit_controller** controller = nullptr;
	v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller);
	CHECK(controller, "il component non espone IEditController");
	// Gli unici parametri sono i controller MIDI nascosti (CC/pitch-bend/aftertouch): 130*16.
	CHECK(v3_cpp_obj(controller)->get_parameter_count(controller) == 130 * 16,
	      "attesi 130*16 parametri MIDI (nessuna manopola del rack)");

	std::printf("create_view(\"editor\")...\n");
	v3_plugin_view** view = v3_cpp_obj(controller)->create_view(controller, "editor");
	CHECK(view, "create_view ha restituito NULL");
	CHECK(v3_cpp_obj(view)->is_platform_type_supported(view, V3_VIEW_PLATFORM_TYPE_HWND) == V3_OK,
	      "la view rifiuta il tipo HWND");

	v3_view_rect rect = {};
	CHECK(v3_cpp_obj(view)->get_size(view, &rect) == V3_OK, "get_size fallita");
	std::printf("  size del segnaposto: %dx%d\n", (int)(rect.right - rect.left),
	            (int)(rect.bottom - rect.top));

	// Finta finestra editor dell'host in cui incorporare il segnaposto, come farebbe un DAW.
	HWND hostWnd = CreateWindowExW(0, L"STATIC", L"Finto editor DAW", WS_OVERLAPPEDWINDOW,
	                               CW_USEDEFAULT, CW_USEDEFAULT, 400, 200,
	                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	CHECK(hostWnd, "creazione della finta finestra host fallita");

	std::printf("attached() -> deve comparire la finestra accessibile MetaRack...\n");
	CHECK(v3_cpp_obj(view)->attached(view, hostWnd, V3_VIEW_PLATFORM_TYPE_HWND) == V3_OK,
	      "attached fallita");
	// Il segnaposto deve essere davvero figlio della finestra host.
	CHECK(FindWindowExW(hostWnd, nullptr, nullptr, nullptr) != nullptr,
	      "attached non ha creato la finestra segnaposto figlia");

	std::printf("removed()...\n");
	CHECK(v3_cpp_obj(view)->removed(view) == V3_OK, "removed fallita");
	CHECK(FindWindowExW(hostWnd, nullptr, nullptr, nullptr) == nullptr,
	      "removed non ha distrutto la finestra segnaposto");

	v3_cpp_obj_unref(view);
	v3_cpp_obj_unref(controller);
	DestroyWindow(hostWnd);

	std::printf("teardown...\n");
	v3_cpp_obj(processor)->set_processing(processor, false);
	v3_cpp_obj(component)->set_active(component, false);
	v3_cpp_obj_unref(processor);
	v3_cpp_obj_terminate(component);
	v3_cpp_obj_unref(component);

	if (exitDll)
		exitDll();

	std::printf("== OK: ciclo di vita VST3 completato senza crash ==\n");
	return (passthrough && midiOk && pbOk) ? 0 : 3;
}


#ifdef UNICODE
// -municode (arch.mk) seleziona l'entry wide.
int wmain(int argc, wchar_t** argv) {
	if (argc > 1)
		g_bundleArg = argv[1];
	return main();
}
#endif
