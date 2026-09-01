RACK_DIR ?= .
RACK_EDITION := Free
RACK_VERSION_MAJOR := 2
RACK_VERSION ?= $(patsubst v%,%,$(shell git describe --tags --match "v$(RACK_VERSION_MAJOR).*" 2>/dev/null))
# This fork carries no v2.* git tags, so `git describe` returns nothing and RACK_VERSION
# is empty. An empty APP_VERSION makes the built-in Core plugin fail to load with "No
# plugin version" (Plugin::fromJson), which silently drops the entire VCV brand — and all
# its modules — from the library. Fall back to a concrete version (matching the bundled
# Fundamental) whenever no tag describes the current commit.
ifeq ($(strip $(RACK_VERSION)),)
RACK_VERSION := 2.6.4
endif

FLAGS += -Iinclude -Idep/include

include arch.mk

# Sources and build flags

SOURCES += dep/nanovg/src/nanovg.c
SOURCES += dep/osdialog/osdialog.c
SOURCES += dep/oui-blendish/blendish.c
SOURCES += dep/pffft/pffft.c dep/pffft/fftpack.c
SOURCES += dep/tinyexpr/tinyexpr.c
SOURCES += $(wildcard src/*.c src/*/*.c)
SOURCES += $(wildcard src/*.cpp src/*/*.cpp)

build/src/common.cpp.o: FLAGS += -D_RACK_VERSION=$(RACK_VERSION)
build/dep/tinyexpr/tinyexpr.c.o: FLAGS += -DTE_POW_FROM_RIGHT -DTE_NAT_LOG

FLAGS += -fPIC
LDFLAGS += -shared

ifdef ARCH_LIN
	SED := sed -i
	TARGET := libRack.so

	SOURCES += dep/osdialog/osdialog_zenity.c

	# This prevents static variables in the DSO (dynamic shared object) from being preserved after dlclose().
	# I don't really understand the side effects (see GCC manual), but so far tests are positive.
	FLAGS += -fno-gnu-unique

	LDFLAGS += -Wl,--whole-archive
	LDFLAGS += -static-libstdc++ -static-libgcc
	LDFLAGS += dep/lib/libGLEW.a dep/lib/libglfw3.a dep/lib/libjansson.a dep/lib/libcurl.a dep/lib/libssl.a dep/lib/libcrypto.a dep/lib/libarchive.a dep/lib/libzstd.a dep/lib/libspeexdsp.a dep/lib/libsamplerate.a dep/lib/librtmidi.a dep/lib/librtaudio.a
	LDFLAGS += -Wl,--no-whole-archive
	LDFLAGS += -lpthread -lGL -ldl -lX11 -lasound -ljack -lpulse -lpulse-simple
endif

ifdef ARCH_MAC
	SED := sed -i ''
	TARGET := libRack.dylib

	SOURCES += $(wildcard src/*.m src/*/*.m)
	SOURCES += $(wildcard src/*.mm src/*/*.mm)
	SOURCES += dep/osdialog/osdialog_mac.m
	LDFLAGS += -lpthread -ldl
	LDFLAGS += -framework SystemConfiguration -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo -framework CoreAudio -framework CoreMIDI -framework AVFoundation
	LDFLAGS += -Wl,-all_load
	LDFLAGS += dep/lib/libGLEW.a dep/lib/libglfw3.a dep/lib/libjansson.a dep/lib/libcurl.a dep/lib/libssl.a dep/lib/libcrypto.a -Wl,-load_hidden,dep/lib/libarchive.a -Wl,-load_hidden,dep/lib/libzstd.a dep/lib/libspeexdsp.a dep/lib/libsamplerate.a -Wl,-load_hidden,dep/lib/librtmidi.a -Wl,-load_hidden,dep/lib/librtaudio.a
endif

ifdef ARCH_WIN
	SED := sed -i
	TARGET := libRack.dll

	SOURCES += dep/osdialog/osdialog_win.c
	LDFLAGS += -municode
	LDFLAGS += -Wl,--export-all-symbols
	LDFLAGS += -Wl,--out-implib,$(TARGET).a
	LDFLAGS += -Wl,-Bstatic -Wl,--whole-archive
	LDFLAGS += dep/lib/libglew32.a dep/lib/libglfw3.a dep/lib/libjansson.a dep/lib/libspeexdsp.a dep/lib/libsamplerate.a dep/lib/libarchive.a dep/lib/libzstd.a dep/lib/libcurl.a dep/lib/libssl.a dep/lib/libcrypto.a dep/lib/librtaudio.a dep/lib/librtmidi.a
	LDFLAGS += -Wl,-Bdynamic -Wl,--no-whole-archive
	LDFLAGS += -lpthread -lopengl32 -lgdi32 -lws2_32 -lcomdlg32 -lole32 -ldsound -lwinmm -lksuser -lshlwapi -lmfplat -lmfuuid -lwmcodecdspuuid -ldbghelp -lcrypt32 -lbcrypt
endif

# Some libraries aren't needed by plugins and might conflict with DAWs that load libRack, so make their symbols local to libRack instead of global (default).
# --exclude-libs is unavailable on Apple ld
ifndef ARCH_MAC
	LDFLAGS += -Wl,--exclude-libs,libzstd.a
	LDFLAGS += -Wl,--exclude-libs,libarchive.a
	LDFLAGS += -Wl,--exclude-libs,librtmidi.a
	LDFLAGS += -Wl,--exclude-libs,librtaudio.a
endif

include compile.mk

# Standalone adapter

STANDALONE_SOURCES += adapters/standalone.cpp

ifdef ARCH_LIN
	STANDALONE_TARGET := Rack
	STANDALONE_LDFLAGS += -static-libstdc++ -static-libgcc
	STANDALONE_LDFLAGS += -Wl,-rpath=.
endif
ifdef ARCH_MAC
	STANDALONE_TARGET := Rack
	STANDALONE_LDFLAGS += -stdlib=libc++
endif
ifdef ARCH_WIN
	STANDALONE_TARGET := Rack.exe
	STANDALONE_LDFLAGS += -mwindows
	# 1MiB stack size to match MSVC
	STANDALONE_LDFLAGS += -Wl,--stack,0x100000
	STANDALONE_OBJECTS += build/Rack.res
endif

STANDALONE_OBJECTS += $(TARGET)

$(STANDALONE_TARGET): $(STANDALONE_SOURCES) $(STANDALONE_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(STANDALONE_LDFLAGS)

# VST3 adapter scritto a mano: nessun SDK Steinberg e nessun wrapper: adapters/vst3.cpp
# implementa l'ABI VST3 con gli header travesty (C puro), quindi si costruisce con lo stesso
# compilatore che produce libRack, invece che con MSVC (Windows) o l'SDK Steinberg. Un .vst3
# è un BUNDLE su entrambe le piattaforme, ma con layout e problemi diversi — vedi i due rami.
#
# WINDOWS: il bundle è una DLL in Contents/x86_64-win e deve portarsi dietro OGNI dipendenza.
# Windows non cerca le dipendenze di una DLL nella cartella della DLL stessa: il search path
# parte dall'EXE host, che qui è il DAW (reaper.exe), non noi. Servono:
#   - libRack.dll
#   - il runtime MinGW: su Windows libRack NON è linkata staticamente a libstdc++/libgcc
#     (il -static-libstdc++ del Makefile è solo per ARCH_LIN), quindi le tre DLL vanno
#     spedite, esattamente come fa il target `dist` per lo standalone.
#   - nvdaControllerClient.dll, caricata a runtime dalla finestra accessibile.
# Le prime le chiediamo al compilatore, così non incastriamo a mano la versione del toolchain.
# Il modulo che l'host carica è uno STUB senza dipendenze (adapters/vst3stub.c), che carica
# l'adapter vero per percorso assoluto. Senza lo stub, un host con ricerca DLL ristretta
# (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) non riesce a risolvere libRack.dll accanto al modulo e
# il plugin sparisce senza errori: è ciò che facevano Reaper e Ableton. Vedi vst3stub.c.
#
# macOS: niente stub. Il binario sta in Contents/MacOS senza estensione, e dyld risolve le
# dipendenze per @loader_path, cioè relativamente al binario stesso: copiare libRack.dylib
# accanto e riscrivere l'install name basta a rendere il bundle autosufficiente ovunque.
VST3_SOURCES += adapters/vst3.cpp adapters/rackhost.cpp
VST3_STUB_SOURCES += adapters/vst3stub.c
ifdef ARCH_WIN
	VST3_BUNDLE := MetaRack.vst3
	VST3_BUNDLE_DIR := $(VST3_BUNDLE)/Contents/x86_64-win
	# Su Windows il modulo dentro Contents/x86_64-win DEVE chiamarsi come il bundle.
	VST3_TARGET := $(VST3_BUNDLE_DIR)/MetaRack.vst3
	VST3_ADAPTER := $(VST3_BUNDLE_DIR)/RackVst3Adapter.dll
	VST3_LDFLAGS += -shared
	# Lo stub non deve dipendere da NESSUNA DLL affiancata, o il problema si riproporrebbe su
	# di lui: -static-libgcc elimina libgcc_s_seh-1.dll. Restano solo kernel32 e msvcrt, che
	# stanno in System32 e sono risolvibili con qualunque politica di ricerca dell'host.
	VST3_STUB_LDFLAGS += -shared -static-libgcc
	VST3_RUNTIME_DLLS := $(shell $(CXX) -print-file-name=libstdc++-6.dll) \
	                     $(shell $(CXX) -print-file-name=libgcc_s_seh-1.dll) \
	                     $(shell $(CXX) -print-file-name=libwinpthread-1.dll)
endif
ifdef ARCH_MAC
	VST3_BUNDLE := MetaRack.vst3
	VST3_BUNDLE_DIR := $(VST3_BUNDLE)/Contents/MacOS
	# Il binario del bundle non ha estensione e deve combaciare con CFBundleExecutable
	# dell'Info.plist, altrimenti CFBundle non lo trova e l'host scarta il plugin in silenzio.
	VST3_TARGET := $(VST3_BUNDLE_DIR)/MetaRack
	# Le due parti Cocoa dell'adapter: la protezione attorno a glfwInit (che dentro una DAW
	# si prenderebbe il delegate di NSApp) e il segnaposto NSView dell'editor.
	VST3_SOURCES += adapters/rackhost_mac.mm adapters/vst3_mac.mm
	VST3_LDFLAGS += -bundle -stdlib=libc++ -framework Cocoa
	# Il linker con -g fa girare dsymutil, che scrive il .dSYM ACCANTO all'output, cioè
	# dentro il bundle. Va tolto di lì (vedi la regola), ma tenuto: è ciò che simbolica un
	# crash dentro il DAW.
	VST3_DSYM := $(VST3_BUNDLE).dSYM
endif
VST3_OBJECTS += $(TARGET)

ifdef ARCH_WIN
$(VST3_ADAPTER): $(VST3_SOURCES) $(VST3_OBJECTS)
	mkdir -p $(VST3_BUNDLE_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(VST3_LDFLAGS)
	cp $(TARGET) $(VST3_RUNTIME_DLLS) $(VST3_BUNDLE_DIR)/
	cp nvdaControllerClient.dll $(VST3_BUNDLE_DIR)/ 2>/dev/null || echo "NB: nvdaControllerClient.dll assente, NVDA non parlerà dal plugin"
	# -MMD scrive il .d accanto all'output: nel bundle, che spediamo, non ci va.
	rm -f $(VST3_BUNDLE_DIR)/*.d

$(VST3_TARGET): $(VST3_STUB_SOURCES) $(VST3_ADAPTER)
	$(CC) $(CFLAGS) -o $@ $(VST3_STUB_SOURCES) $(VST3_STUB_LDFLAGS)
	rm -f $(VST3_BUNDLE_DIR)/*.d
endif

ifdef ARCH_MAC
$(VST3_TARGET): $(VST3_SOURCES) $(VST3_OBJECTS) adapters/vst3/Info.plist
	mkdir -p $(VST3_BUNDLE_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(VST3_SOURCES) $(VST3_OBJECTS) $(VST3_LDFLAGS)
	# libRack.dylib registra un install name RELATIVO ("libRack.dylib"), che dyld non risolve
	# accanto al modulo. La copiamo nel bundle e riscriviamo la dipendenza come @loader_path,
	# cioè "la cartella del binario che mi carica": così il .vst3 si carica in qualunque host
	# e da qualunque cartella, senza DYLD_LIBRARY_PATH.
	cp $(TARGET) $(VST3_BUNDLE_DIR)/
	install_name_tool -change $(TARGET) @loader_path/$(TARGET) $@
	# E ora il rovescio della medaglia. I plugin di terze parti sono compilati dalla build
	# farm VCV dentro /tmp/Rack2, quindi dichiarano la dipendenza ASSOLUTA
	# /tmp/Rack2/libRack.dylib (vedi anche mac-librack-link, che per `make run` risolve la
	# stessa cosa con un symlink). Dentro un DAW quel symlink non c'è, e senza il rimedio
	# NESSUN plugin dell'utente si carica: il rack resta al solo Core.
	# Rimedio senza toccare il filesystem: diamo alla COPIA nel bundle proprio quell'install
	# name. Quando poi plugin.dylib chiede /tmp/Rack2/libRack.dylib, dyld trova un'immagine
	# GIÀ CARICATA con quel nome e la riusa, senza cercare il file. Il nostro binario non ne
	# risente: la sua dipendenza è stata riscritta a @loader_path, che è un percorso reale.
	install_name_tool -id /tmp/Rack2/libRack.dylib $(VST3_BUNDLE_DIR)/$(TARGET)
	# Senza Info.plist (e senza il CFBundleExecutable giusto) CFBundle non riconosce la
	# cartella come bundle: l'host la salta senza un errore.
	mkdir -p $(VST3_BUNDLE)/Contents
	cp adapters/vst3/Info.plist $(VST3_BUNDLE)/Contents/
	$(SED) 's/{RACK_VERSION}/$(RACK_VERSION)/g' $(VST3_BUNDLE)/Contents/Info.plist
	printf 'BNDL????' > $(VST3_BUNDLE)/Contents/PkgInfo
	# -MMD scrive il .d accanto all'output: nel bundle, che spediamo, non ci va.
	rm -f $(VST3_BUNDLE_DIR)/*.d
	# Idem per il .dSYM prodotto da dsymutil: dentro il bundle finirebbe sigillato dalla
	# firma e copiato da vst3dist. Lo spostiamo accanto al bundle, dove resta utilizzabile
	# per simbolicare un crash (atos/lldb lo cercano anche lì).
	rm -rf $(VST3_DSYM)
	mv $(VST3_BUNDLE_DIR)/MetaRack.dSYM $(VST3_DSYM)
	# Su Apple Silicon un binario senza firma NON viene caricato, e install_name_tool ha
	# appena invalidato la firma ad-hoc che il linker aveva applicato: rifirmiamo ad-hoc il
	# binario e poi il bundle intero (che sigilla anche l'Info.plist).
	codesign --force --sign - $(VST3_BUNDLE_DIR)/$(TARGET)
	codesign --force --sign - $(VST3_BUNDLE)
endif

vst3: $(VST3_TARGET)

# Dev harness: mini-host VST3 da console. Con un ABI scritto a mano è l'unico modo di
# distinguere "il DAW non lo vede" da "l'ABI è sbagliato".
VST3TEST_SOURCES += adapters/vst3test.cpp
ifdef ARCH_WIN
	VST3TEST_TARGET := RackVst3Test.exe
	# Linkato staticamente di proposito: l'harness non deve dipendere da DLL accanto a sé,
	# così può girare da una cartella qualsiasi e testare ONESTAMENTE se è il BUNDLE a essere
	# autosufficiente. Girando da C:\Rack (dove stanno libRack.dll e il runtime) il test
	# passerebbe anche con un bundle incompleto: le DLL verrebbero risolte dalla cartella
	# dell'eseguibile, non dal bundle.
	VST3TEST_LDFLAGS += -static-libstdc++ -static-libgcc
endif
ifdef ARCH_MAC
	VST3TEST_TARGET := RackVst3Test
	# Stessa logica: l'harness NON si linka a libRack. Carica il bundle con dlopen e basta,
	# quindi se il bundle non risolve le proprie dipendenze il test fallisce come farebbe la
	# DAW, invece di essere salvato dalla libRack.dylib che sta nella cartella corrente.
	# Objective-C++ perché il test di attached() vuole una NSView vera, come quella che
	# passa un host: -x deve precedere i sorgenti, quindi sta in una variabile a sé.
	VST3TEST_FLAGS += -x objective-c++
	VST3TEST_LDFLAGS += -stdlib=libc++ -framework Cocoa
endif

$(VST3TEST_TARGET): $(VST3TEST_SOURCES) $(VST3_TARGET)
	$(CXX) $(CXXFLAGS) $(VST3TEST_FLAGS) -o $@ $(VST3TEST_SOURCES) $(VST3TEST_LDFLAGS)

vst3test: $(VST3TEST_TARGET)

# Packaging: bundle VST3 AUTOSUFFICIENTE e RILOCABILE, pronto da installare in una qualunque
# cartella VST3 (su macOS ~/Library/Audio/Plug-Ins/VST3). Differenza da `make vst3` (che
# lascia il bundle in-tree e fa risalire systemDir alla radice del repo): qui copiamo la
# res/ + Core.json &co. DENTRO Contents/Resources. A runtime findPackagedResources()
# (rackhost.cpp) li trova nel bundle e punta userDir alla libreria per-utente condivisa con
# lo standalone. Output in dist/, così il bundle di sviluppo resta pulito e leggero.
VST3_DIST_BUNDLE := dist/$(VST3_BUNDLE)
ifdef ARCH_WIN
	VST3_DIST_ARCH := $(VST3_DIST_BUNDLE)/Contents/x86_64-win
endif
ifdef ARCH_MAC
	VST3_DIST_ARCH := $(VST3_DIST_BUNDLE)/Contents/MacOS
endif
VST3_DIST_RES := $(VST3_DIST_BUNDLE)/Contents/Resources

vst3dist: vst3
	rm -rf "$(VST3_DIST_BUNDLE)"
	mkdir -p "$(VST3_DIST_ARCH)" "$(VST3_DIST_RES)"
	# Binari, già assemblati da `make vst3` (Windows: stub + adapter + libRack + runtime
	# MinGW + nvdaControllerClient; macOS: binario del bundle + libRack.dylib).
	cp $(VST3_BUNDLE_DIR)/* "$(VST3_DIST_ARCH)/"
	# systemDir del bundle: tutto ciò che lo standalone tiene nella radice Rack.
	cp -R res translations "$(VST3_DIST_RES)/"
	cp Core.json template.vcv cacert.pem "$(VST3_DIST_RES)/"
ifdef ARCH_MAC
	cp $(VST3_BUNDLE)/Contents/Info.plist $(VST3_BUNDLE)/Contents/PkgInfo "$(VST3_DIST_BUNDLE)/Contents/"
	# Copiare i binari ha invalidato la firma del bundle: rifirmiamo la copia distribuita.
	codesign --force --sign - "$(VST3_DIST_ARCH)/$(notdir $(TARGET))"
	codesign --force --sign - "$(VST3_DIST_BUNDLE)"
endif
	@echo "Bundle pacchettizzato pronto: $(VST3_DIST_BUNDLE)"

# Convenience targets

all: $(TARGET) $(STANDALONE_TARGET)

dep:
	$(MAKE) -C dep

cleandep:
	$(MAKE) -C dep clean

# On macOS the executable records libRack.dylib with a bare relative install name,
# which dyld does not resolve against the current directory. Point dyld at the repo
# root so the dev build runs without bundling (make dist). No-op on Linux/Windows.
ifdef ARCH_MAC
	RUN_ENV := DYLD_LIBRARY_PATH="$(CURDIR)"
endif

# VCV-prebuilt plugins record their libRack.dylib dependency as the absolute path
# /tmp/Rack2/libRack.dylib (the VCV build farm's build directory). DYLD_LIBRARY_PATH
# overrides this in a plain shell, but not through make's SIP-protected shell, so under
# `make run` the plugins fail to load (Library not loaded: /tmp/Rack2/libRack.dylib) and
# the module browser comes up empty. Point that absolute path at our local libRack.dylib.
# /tmp is volatile, so recreate the link on every run. No-op off macOS.
mac-librack-link:
ifdef ARCH_MAC
	@mkdir -p /tmp/Rack2
	@ln -sf "$(CURDIR)/libRack.dylib" /tmp/Rack2/libRack.dylib
endif

run: $(STANDALONE_TARGET) mac-librack-link
	$(RUN_ENV) ./$< -d

runr: $(STANDALONE_TARGET) mac-librack-link
	$(RUN_ENV) ./$<

debug: $(STANDALONE_TARGET) mac-librack-link
ifdef ARCH_MAC
	$(RUN_ENV) lldb -- ./$< -d
endif
ifdef ARCH_WIN
	gdb --args ./$< -d
endif
ifdef ARCH_LIN
	gdb --args ./$< -d
endif

perf: $(STANDALONE_TARGET)
	# Requires perf
	perf record --call-graph dwarf -o perf.data ./$< -d
	# Analyze with hotspot (https://github.com/KDAB/hotspot) for example
	hotspot perf.data
	rm perf.data

VALGRIND_FLAGS += --gen-suppressions=all
VALGRIND_FLAGS += --suppressions=valgrind.supp
VALGRIND_FLAGS += --leak-check=full
VALGRIND_FLAGS += --track-origins=yes
VALGRIND_FLAGS += --exit-on-first-error=yes
valgrind: $(STANDALONE_TARGET)
	valgrind $(VALGRIND_FLAGS) ./$< -d

clean:
	rm -rfv build dist $(TARGET) $(STANDALONE_TARGET) $(VST3_BUNDLE) $(VST3_DSYM) $(VST3TEST_TARGET) *.dSYM *.a

# Windows resources
build/%.res: %.rc
ifdef ARCH_WIN
	windres $^ -O coff -o $@
endif


# Plugin helper
plugins:
ifdef CMD
	for f in plugins/*; do (cd "$$f" && $(CMD)); done
else
	for f in plugins/*; do $(MAKE) -C "$$f"; done
endif


# The following targets are not supported for public use

DIST_NAME = Rack$(RACK_EDITION)-$(RACK_VERSION)-$(ARCH_NAME)
ifdef ARCH_MAC
	DIST_BUNDLE := VCV Rack $(RACK_VERSION_MAJOR) $(RACK_EDITION).app
else
	DIST_DIR := Rack$(RACK_VERSION_MAJOR)$(RACK_EDITION)
endif
FUNDAMENTAL_VERSION ?= 2.6.4
FUNDAMENTAL_FILENAME := Fundamental-$(FUNDAMENTAL_VERSION)-$(ARCH_NAME).vcvplugin
DIST_MD := $(wildcard *.md)
DIST_HTML := $(patsubst %.md, build/%.html, $(DIST_MD))
DIST_RES := res cacert.pem Core.json template.vcv LICENSE-GPLv3.txt $(DIST_HTML) translations $(FUNDAMENTAL_FILENAME)
DIST_SDK_DIR := Rack-SDK
DIST_SDK = Rack-SDK-$(RACK_VERSION)-$(ARCH_NAME).zip


$(FUNDAMENTAL_FILENAME):
	curl -o "$(FUNDAMENTAL_FILENAME)" "https://api.vcvrack.com/download?slug=Fundamental&version=$(FUNDAMENTAL_VERSION)&arch=$(ARCH_NAME)"


dist: $(TARGET) $(STANDALONE_TARGET) $(DIST_HTML) $(FUNDAMENTAL_FILENAME)
	mkdir -p dist
ifdef ARCH_LIN
	mkdir -p dist/"$(DIST_DIR)"
	cp $(TARGET) dist/"$(DIST_DIR)"/
	cp $(STANDALONE_TARGET) dist/"$(DIST_DIR)"/
	$(STRIP) -s dist/"$(DIST_DIR)"/$(TARGET)
	$(STRIP) -s dist/"$(DIST_DIR)"/$(STANDALONE_TARGET)
	# Manually check that no nonstandard shared libraries are linked
	ldd dist/"$(DIST_DIR)"/$(TARGET)
	ldd dist/"$(DIST_DIR)"/$(STANDALONE_TARGET)
	# Copy resources
	cp -R $(DIST_RES) dist/"$(DIST_DIR)"/
endif
ifdef ARCH_MAC
	mkdir -p dist/"$(DIST_BUNDLE)"
	mkdir -p dist/"$(DIST_BUNDLE)"/Contents
	mkdir -p dist/"$(DIST_BUNDLE)"/Contents/Resources
	mkdir -p dist/"$(DIST_BUNDLE)"/Contents/MacOS
	cp $(TARGET) dist/"$(DIST_BUNDLE)"/Contents/Resources/
	cp $(STANDALONE_TARGET) dist/"$(DIST_BUNDLE)"/Contents/MacOS/
	$(STRIP) -S dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET)
	$(STRIP) -S dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET)
	install_name_tool -change $(TARGET) @executable_path/../Resources/$(TARGET) dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET)
	# Manually check that no nonstandard shared libraries are linked
	otool -L dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET)
	otool -L dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET)
	# Copy resources
	cp Info.plist dist/"$(DIST_BUNDLE)"/Contents/
	$(SED) 's/{RACK_VERSION}/$(RACK_VERSION)/g' dist/"$(DIST_BUNDLE)"/Contents/Info.plist
	cp -R icon.icns dist/"$(DIST_BUNDLE)"/Contents/Resources/
	cp -R $(DIST_RES) dist/"$(DIST_BUNDLE)"/Contents/Resources/
endif
ifdef ARCH_WIN
	mkdir -p dist/"$(DIST_DIR)"
	cp $(TARGET) dist/"$(DIST_DIR)"/
	cp $(STANDALONE_TARGET) dist/"$(DIST_DIR)"/
	$(STRIP) -s dist/"$(DIST_DIR)"/$(TARGET)
	$(STRIP) -s dist/"$(DIST_DIR)"/$(STANDALONE_TARGET)
	# Copy resources
	cp -R $(DIST_RES) dist/"$(DIST_DIR)"/
	cp /mingw64/bin/libwinpthread-1.dll dist/"$(DIST_DIR)"/
	cp /mingw64/bin/libstdc++-6.dll dist/"$(DIST_DIR)"/
	cp /mingw64/bin/libgcc_s_seh-1.dll dist/"$(DIST_DIR)"/
endif


sdk: $(DIST_HTML)
	mkdir -p dist/$(DIST_SDK_DIR)
	cp -R include *.mk helper.py $(DIST_HTML) dist/$(DIST_SDK_DIR)/
	mkdir -p dist/$(DIST_SDK_DIR)/dep
	cp -R dep/include dist/$(DIST_SDK_DIR)/dep/
ifdef ARCH_LIN
	cp $(TARGET) dist/$(DIST_SDK_DIR)/
	$(STRIP) -s dist/$(DIST_SDK_DIR)/$(TARGET)
endif
ifdef ARCH_MAC
	cp $(TARGET) dist/$(DIST_SDK_DIR)/
	$(STRIP) -S dist/$(DIST_SDK_DIR)/$(TARGET)
endif
ifdef ARCH_WIN
	cp $(TARGET).a dist/$(DIST_SDK_DIR)/
endif
	# SDK
	cd dist && zip -q -9 -r $(DIST_SDK) $(DIST_SDK_DIR)


package:
ifdef ARCH_LIN
	# Make ZIP
	cd dist && zip -q -9 -r $(DIST_NAME).zip "$(DIST_DIR)"
endif
ifdef ARCH_MAC
	# Clean up and sign bundle
	xattr -cr dist/"$(DIST_BUNDLE)"
	codesign --verbose --sign "Developer ID Application: Andrew Belt (V8SW9J626X)" --options runtime --entitlements Entitlements.plist --timestamp --deep dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET) dist/"$(DIST_BUNDLE)"
	codesign --verify --deep --strict --verbose=2 dist/"$(DIST_BUNDLE)"
	# Make standalone PKG
	mkdir -p dist/Component
	cp -R dist/"$(DIST_BUNDLE)" dist/Component/
	pkgbuild --identifier com.vcvrack.rack2 --component-plist Component.plist --root dist/Component --install-location /Applications dist/Component.pkg
	# Make PKG
	productbuild --distribution Distribution.xml --package-path dist dist/$(DIST_NAME).pkg
	productsign --sign "Developer ID Installer: Andrew Belt (V8SW9J626X)" dist/$(DIST_NAME).pkg dist/$(DIST_NAME)-signed.pkg
	mv dist/$(DIST_NAME)-signed.pkg dist/$(DIST_NAME).pkg
endif
ifdef ARCH_WIN
	# Make NSIS installer
	# pacman -S mingw-w64-x86_64-nsis
	makensis -DRACK_VERSION_MAJOR=$(RACK_VERSION_MAJOR) -DRACK_VERSION=$(RACK_VERSION) "-XOutFile dist/$(DIST_NAME).exe" installer.nsi
endif


lipo:
ifndef OTHER_RACK_DIR
	$(error OTHER_RACK_DIR not defined)
endif
ifdef ARCH_MAC
	# App bundle
	lipo -create -output dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET) dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET) $(OTHER_RACK_DIR)/dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET)
	lipo -create -output dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET) dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET) $(OTHER_RACK_DIR)/dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET)
	# Fundamental package
	cp $(OTHER_RACK_DIR)/dist/"$(DIST_BUNDLE)"/Contents/Resources/Fundamental-*.vcvplugin dist/"$(DIST_BUNDLE)"/Contents/Resources/
endif


notarize:
ifdef ARCH_MAC
	# Submit installer package to Apple
	xcrun notarytool submit --keychain-profile "VCV" --wait dist/$(DIST_NAME).pkg
	# Mark app as notarized
	xcrun stapler staple dist/$(DIST_NAME).pkg
	# Check notarization
	stapler validate dist/$(DIST_NAME).pkg
endif


install: uninstall
ifdef ARCH_MAC
	sudo installer -pkg dist/$(DIST_NAME).pkg -target /
endif


uninstall:
ifdef ARCH_MAC
	sudo rm -rf /Applications/"$(DIST_BUNDLE)"
endif


cleandist:
	rm -rfv dist


.DEFAULT_GOAL := all
.PHONY: all dep run runr debug clean plugins dist sdk package lipo notarize mac-librack-link vst3 vst3test vst3dist
