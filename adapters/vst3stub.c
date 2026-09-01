// Stub di caricamento del VST3 — il modulo che l'host carica davvero.
//
// PERCHÉ ESISTE
// Il vero adapter (RackVst3Adapter.dll) importa libRack.dll, che vive accanto a lui nel
// bundle. Ma Windows non cerca le dipendenze di una DLL nella cartella della DLL stessa: il
// search path parte dall'EXE host. Alcuni host caricano i plugin con la ricerca RISTRETTA
// (SetDefaultDllDirectories / LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = solo cartella dell'app,
// System32 e le user dirs), che NON include la cartella del modulo caricato: lì libRack.dll
// è invisibile, il load fallisce con ERROR_MOD_NOT_FOUND e il plugin sparisce senza un
// errore comprensibile. È esattamente ciò che facevano Reaper e Ableton: trovavano il
// bundle, registravano il file, e non riuscivano a caricarlo.
//
// Questo stub non ha NESSUNA dipendenza oltre kernel32, quindi si carica sempre, con
// qualunque politica di ricerca. Poi carica l'adapter per PERCORSO ASSOLUTO con
// LOAD_WITH_ALTERED_SEARCH_PATH, che per quella catena di load mette la nostra cartella in
// testa alla ricerca: da lì libRack.dll e il runtime MinGW si risolvono sempre.
//
// Espone i tre simboli del modulo VST3 e li inoltra. Non c'è altro da inoltrare: la factory
// è ABI C (struct di puntatori a funzione), quindi tutte le chiamate successive entrano
// dritte nell'adapter senza passare da qui.
//
// Scritto in C puro e linkato senza libstdc++/libgcc di proposito: se lo stub avesse
// dipendenze, il problema si riproporrebbe su se stesso.

#include <windows.h>
#include <stdbool.h>

// Il nome dell'adapter dentro il bundle, accanto a questo stub.
#define ADAPTER_DLL L"RackVst3Adapter.dll"

// Le firme devono combaciare ESATTAMENTE con quelle dell'adapter e con ciò che l'host si
// aspetta: lo spec VST3 dichiara `bool InitDll()`. Un `bool` torna in AL (1 byte); leggerlo
// come BOOL (4 byte) prenderebbe byte alti non inizializzati.
typedef bool (*InitDllFn)(void);
typedef bool (*ExitDllFn)(void);
typedef const void* (*GetPluginFactoryFn)(void);

static HMODULE g_adapter = NULL;
static InitDllFn g_initDll = NULL;
static ExitDllFn g_exitDll = NULL;
static GetPluginFactoryFn g_getFactory = NULL;


// Carica l'adapter una volta sola. Restituisce false in caso di fallimento.
static bool loadAdapter(void) {
	wchar_t path[MAX_PATH];
	HMODULE self = NULL;
	DWORD len;
	wchar_t* slash;

	if (g_adapter)
		return true;

	// La cartella di QUESTO modulo: GetModuleFileNameW(NULL) darebbe l'EXE dell'host.
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR) &loadAdapter, &self))
		return false;
	len = GetModuleFileNameW(self, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return false;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return false;
	slash[1] = L'\0';
	if (wcslen(path) + wcslen(ADAPTER_DLL) >= MAX_PATH)
		return false;
	wcscat(path, ADAPTER_DLL);

	// Percorso assoluto + LOAD_WITH_ALTERED_SEARCH_PATH: la nostra cartella va in testa alla
	// ricerca per questa catena di load, così libRack.dll e il runtime accanto si risolvono
	// qualunque cosa faccia l'host.
	g_adapter = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_adapter)
		return false;

	g_initDll = (InitDllFn) (void*) GetProcAddress(g_adapter, "InitDll");
	g_exitDll = (ExitDllFn) (void*) GetProcAddress(g_adapter, "ExitDll");
	g_getFactory = (GetPluginFactoryFn) (void*) GetProcAddress(g_adapter, "GetPluginFactory");
	return g_getFactory != NULL;
}


__declspec(dllexport) bool InitDll(void) {
	if (!loadAdapter())
		return false;
	return g_initDll ? g_initDll() : true;
}

__declspec(dllexport) bool ExitDll(void) {
	// Non facciamo FreeLibrary dell'adapter: Rack non è pensato per essere scaricato e
	// ricaricato nello stesso processo, e l'host sta comunque per lasciarci andare.
	if (g_exitDll)
		g_exitDll();
	return true;
}

__declspec(dllexport) const void* GetPluginFactory(void) {
	// Un host può chiamare GetPluginFactory senza aver chiamato InitDll: carichiamo pigramente.
	if (!loadAdapter())
		return NULL;
	return g_getFactory();
}
