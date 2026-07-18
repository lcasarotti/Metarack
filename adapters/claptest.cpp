// Dev harness — mini-host CLAP da console per testare adapters/clap.cpp SENZA un DAW.
//
// Carica Rack.clap, esegue l'intero ciclo di vita CLAP (init -> factory -> create ->
// activate -> alcuni process() -> teardown) e stampa i log INFO di Rack. Serve come
// loop di sviluppo rapido per le Fasi 1-3; non fa parte del prodotto.
//
// Build: target Makefile `claptest` (console, niente -mwindows). Esegui dalla root
// C:\Rack così trova Rack.clap e libRack.dll accanto.

#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include <windows.h>


static const void* CLAP_ABI hostGetExtension(const clap_host_t*, const char*) {
	return nullptr;
}
static void CLAP_ABI hostRequestRestart(const clap_host_t*) {}
static void CLAP_ABI hostRequestProcess(const clap_host_t*) {}
static void CLAP_ABI hostRequestCallback(const clap_host_t*) {}

static const clap_host_t g_host = {
	CLAP_VERSION_INIT,
	nullptr,
	"RackClapTest",
	"Metarack",
	"",
	"1.0",
	hostGetExtension,
	hostRequestRestart,
	hostRequestProcess,
	hostRequestCallback,
};


#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FALLITO: %s\n", msg); return 1; } } while (0)


int main() {
	// Su Windows il rack parte vuoto (il modulo Audio c'è, ma non è cablato a nulla: è
	// l'utente a instradare i moduli dalla finestra accessibile), quindi senza questo
	// l'uscita sarebbe silenziosa per costruzione e il test del ponte audio non direbbe
	// nulla. La variabile fa aggiungere a rackhost i cavi di loopback DAW in -> DAW out.
	// Va impostata PRIMA del LoadLibrary: la legge l'init dell'istanza.
	SetEnvironmentVariableA("METARACK_TEST_PASSTHROUGH", "1");

	std::printf("== RackClapTest: carico Rack.clap ==\n");
	HMODULE dll = LoadLibraryW(L"Rack.clap");
	CHECK(dll, "LoadLibrary(Rack.clap)");

	auto* entry = (const clap_plugin_entry_t*) GetProcAddress(dll, "clap_entry");
	CHECK(entry, "GetProcAddress(clap_entry)");
	CHECK(clap_version_is_compatible(entry->clap_version), "versione CLAP incompatibile");

	std::printf("entry->init()...\n");
	CHECK(entry->init("Rack.clap"), "entry->init");

	auto* factory = (const clap_plugin_factory_t*) entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	CHECK(factory, "get_factory");
	uint32_t n = factory->get_plugin_count(factory);
	std::printf("plugin disponibili: %u\n", n);
	CHECK(n >= 1, "nessun plugin");

	const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
	CHECK(desc, "get_plugin_descriptor");
	std::printf("  [0] id=%s name=%s vendor=%s version=%s\n",
	            desc->id, desc->name, desc->vendor, desc->version);

	const clap_plugin_t* plugin = factory->create_plugin(factory, &g_host, desc->id);
	CHECK(plugin, "create_plugin");

	std::printf("plugin->init()...\n");
	CHECK(plugin->init(plugin), "plugin->init");

	// Interroga audio-ports
	auto* ports = (const clap_plugin_audio_ports_t*) plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
	if (ports) {
		for (int isIn = 1; isIn >= 0; isIn--) {
			uint32_t pc = ports->count(plugin, isIn);
			for (uint32_t i = 0; i < pc; i++) {
				clap_audio_port_info_t info;
				std::memset(&info, 0, sizeof(info));
				if (ports->get(plugin, i, isIn, &info)) {
					std::printf("  %s port %u: '%s' ch=%u type=%s\n",
					            isIn ? "IN " : "OUT", i, info.name, info.channel_count,
					            info.port_type ? info.port_type : "(none)");
				}
			}
		}
	}

	const double sampleRate = 48000.0;
	const uint32_t frames = 512;
	std::printf("plugin->activate(%g, 1, %u)...\n", sampleRate, frames);
	CHECK(plugin->activate(plugin, sampleRate, 1, frames), "activate");
	CHECK(plugin->start_processing(plugin), "start_processing");

	// Buffer stereo. In ingresso un seno a 440 Hz (ampiezza 0.5) con fase continua tra
	// i blocchi; in uscita ci aspettiamo lo stesso segnale, ritardato dal buffering della
	// catena audio di Rack. Verifichiamo che l'RMS d'uscita diventi non-nullo (pass-through).
	std::vector<float> inL(frames), inR(frames), outL(frames, 0.f), outR(frames, 0.f);
	float* inChans[2] = { inL.data(), inR.data() };
	float* outChans[2] = { outL.data(), outR.data() };
	clap_audio_buffer_t inBuf = { inChans, nullptr, 2, 0, 0 };
	clap_audio_buffer_t outBuf = { outChans, nullptr, 2, 0, 0 };

	clap_process_t proc;
	std::memset(&proc, 0, sizeof(proc));
	proc.frames_count = frames;
	proc.audio_inputs = &inBuf;
	proc.audio_outputs = &outBuf;
	proc.audio_inputs_count = 1;
	proc.audio_outputs_count = 1;
	proc.steady_time = 0;

	const double freq = 440.0;
	const double twoPi = 6.283185307179586;
	double phase = 0.0;
	double maxOutRms = 0.0;

	std::printf("process() con seno 440Hz in ingresso (attendo il pass-through)...\n");
	for (int b = 0; b < 24; b++) {
		// Genera il blocco di ingresso (fase continua).
		for (uint32_t i = 0; i < frames; i++) {
			float s = (float)(0.5 * std::sin(phase));
			inL[i] = s;
			inR[i] = s;
			phase += twoPi * freq / sampleRate;
			if (phase > twoPi)
				phase -= twoPi;
		}

		clap_process_status st = plugin->process(plugin, &proc);
		proc.steady_time += frames;

		// RMS del blocco d'uscita (canale L).
		double sum = 0.0;
		for (uint32_t i = 0; i < frames; i++)
			sum += (double) outL[i] * outL[i];
		double rms = std::sqrt(sum / frames);
		if (rms > maxOutRms)
			maxOutRms = rms;
		if (b < 6 || rms > 0.0)
			std::printf("  block[%02d] status=%d outRMS=%.6f\n", b, st, rms);
	}
	std::printf("RMS d'uscita massimo osservato: %.6f (atteso ~0.35 per un seno 0.5)\n", maxOutRms);
	if (maxOutRms > 0.01)
		std::printf("PASS-THROUGH OK: l'engine ha instradato l'audio dal DAW in -> DAW out.\n");
	else
		std::printf("ATTENZIONE: uscita silenziosa, il pass-through non ha prodotto segnale.\n");

	std::printf("teardown...\n");
	plugin->stop_processing(plugin);
	plugin->deactivate(plugin);
	plugin->destroy(plugin);
	entry->deinit();

	FreeLibrary(dll);
	std::printf("== OK: ciclo di vita CLAP completato senza crash ==\n");
	return 0;
}


#ifdef UNICODE
// -municode (arch.mk) seleziona l'entry wide. Non ci servono gli argomenti.
int wmain(int /*argc*/, wchar_t** /*argv*/) {
	return main();
}
#endif
