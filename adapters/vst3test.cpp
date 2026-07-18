// Dev harness — mini-host VST3 da console per testare adapters/vst3.cpp SENZA un DAW.
//
// Carica il bundle Rack.vst3, esegue l'intero ciclo di vita VST3 (InitDll -> factory ->
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
// Uso: RackVst3Test.exe ["C:\percorso\Rack.vst3"]
static const wchar_t* const kDefaultBundle = L"Rack.vst3";
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


int main() {
	// Su Windows il rack parte vuoto (il modulo Audio c'è, ma non è cablato a nulla: è
	// l'utente a instradare i moduli dalla finestra accessibile), quindi senza questo
	// l'uscita sarebbe silenziosa per costruzione e il test del ponte audio non direbbe
	// nulla. La variabile fa aggiungere a rackhost i cavi di loopback DAW in -> DAW out.
	// Va impostata PRIMA del LoadLibrary: la legge l'init dell'istanza.
	SetEnvironmentVariableA("METARACK_TEST_PASSTHROUGH", "1");

	const std::wstring bundle = g_bundleArg.empty() ? kDefaultBundle : g_bundleArg;
	const std::wstring modulePath = bundle + L"\\Contents\\x86_64-win\\Rack.vst3";
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

	// --- GUI: edit controller + plug view ---------------------------------------------
	// Single component effect: il controller si ottiene dal component, non da una classe
	// separata della factory.
	std::printf("query IEditController...\n");
	v3_edit_controller** controller = nullptr;
	v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller);
	CHECK(controller, "il component non espone IEditController");
	CHECK(v3_cpp_obj(controller)->get_parameter_count(controller) == 0,
	      "atteso nessun parametro automatizzabile");

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
	return passthrough ? 0 : 3;
}


#ifdef UNICODE
// -municode (arch.mk) seleziona l'entry wide.
int wmain(int argc, wchar_t** argv) {
	if (argc > 1)
		g_bundleArg = argv[1];
	return main();
}
#endif
