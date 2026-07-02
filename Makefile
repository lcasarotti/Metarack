RACK_DIR ?= .
RACK_EDITION := Free
RACK_VERSION_MAJOR := 2
# MetaRack's own product version, independent of the underlying Rack base version
# (RACK_VERSION, e.g. 2.6.x). Mirrors METARACK_VERSION in installer.nsi on Windows.
METARACK_VERSION := 1.0
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
	LDFLAGS += -lpthread -lopengl32 -lgdi32 -lws2_32 -lcomdlg32 -lole32 -ldsound -lwinmm -lksuser -lshlwapi -lmfplat -lmfuuid -lwmcodecdspuuid -ldbghelp -lcrypt32 -lbcrypt -lcomctl32
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

# Windowing-embed spike (Fase 0 feasibility test): Rack rendered as a child of a
# host Win32 window, driven by Window::step() from an external loop.
SPIKE_SOURCES += adapters/spike.cpp
ifdef ARCH_WIN
	SPIKE_TARGET := RackSpike.exe
	# Note: intentionally NO -mwindows, so we keep a console for the INFO logs.
	SPIKE_LDFLAGS += -Wl,--stack,0x100000
endif
SPIKE_OBJECTS += $(TARGET)

$(SPIKE_TARGET): $(SPIKE_SOURCES) $(SPIKE_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SPIKE_LDFLAGS)

spike: $(SPIKE_TARGET)

# CLAP plugin adapter (Fase 1): libRack linkato in una DSO .clap che il DAW carica.
# Su Windows un .clap è semplicemente una DLL che esporta il simbolo `clap_entry`.
CLAP_SOURCES += adapters/clap.cpp
ifdef ARCH_WIN
	CLAP_TARGET := Rack.clap
	CLAP_LDFLAGS += -shared
endif
ifdef ARCH_LIN
	CLAP_TARGET := Rack.clap
	CLAP_LDFLAGS += -shared
	CLAP_LDFLAGS += -static-libstdc++ -static-libgcc
	CLAP_LDFLAGS += -Wl,-rpath=.
endif
ifdef ARCH_MAC
	CLAP_TARGET := Rack.clap
	CLAP_LDFLAGS += -shared
	CLAP_LDFLAGS += -stdlib=libc++
endif
CLAP_OBJECTS += $(TARGET)

$(CLAP_TARGET): $(CLAP_SOURCES) $(CLAP_OBJECTS)
	$(CXX) $(CXXFLAGS) -Idep/clap/include -o $@ $^ $(CLAP_LDFLAGS)

clap: $(CLAP_TARGET)

# Dev harness: mini-host CLAP da console che carica Rack.clap e ne esegue il ciclo
# di vita, per testare l'adapter senza un DAW. Console subsystem (niente -mwindows).
CLAPTEST_SOURCES += adapters/claptest.cpp
ifdef ARCH_WIN
	CLAPTEST_TARGET := RackClapTest.exe
endif

$(CLAPTEST_TARGET): $(CLAPTEST_SOURCES) $(CLAP_TARGET)
	$(CXX) $(CXXFLAGS) -Idep/clap/include -o $@ $(CLAPTEST_SOURCES)

claptest: $(CLAPTEST_TARGET)

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
	rm -rfv build dist $(TARGET) $(STANDALONE_TARGET) $(SPIKE_TARGET) $(CLAP_TARGET) $(CLAPTEST_TARGET) *.a

# Windows resources
WINDRES ?= windres
ifdef CROSS_COMPILE
	WINDRES = $(CROSS_COMPILE)-windres
endif

build/%.res: %.rc
ifdef ARCH_WIN
	$(WINDRES) $^ -O coff -o $@
endif


# Plugin helper
plugins:
ifdef CMD
	for f in plugins/*; do (cd "$$f" && $(CMD)); done
else
	for f in plugins/*; do $(MAKE) -C "$$f"; done
endif


# The following targets are not supported for public use

DIST_NAME = MetaRack-$(METARACK_VERSION)-$(ARCH_NAME)
ifdef ARCH_MAC
	DIST_BUNDLE := MetaRack.app
else
	DIST_DIR := MetaRack$(RACK_VERSION_MAJOR)
endif
# Code-signing identity for the macOS bundle. Defaults to "-" (ad-hoc), which is
# enough to launch locally and is mandatory on Apple Silicon. Override with a real
# "Developer ID Application: ..." identity to produce a distributable, notarizable app.
CODESIGN_IDENTITY ?= -
FUNDAMENTAL_VERSION ?= 2.6.4
FUNDAMENTAL_FILENAME := Fundamental-$(FUNDAMENTAL_VERSION)-$(ARCH_NAME).vcvplugin
# Bundle the user-facing docs as HTML, but never the CLAUDE*.md dev/instruction
# files (CLAUDE.local.md in particular is private and not committed).
DIST_MD := $(filter-out CLAUDE.md CLAUDE.local.md, $(wildcard *.md))
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
	# VCV-library plugins record their libRack dependency as the absolute path
	# /tmp/Rack2/libRack.dylib (the VCV build farm's build dir). Give our bundled
	# libRack that exact install name so dyld matches it as the already-loaded image
	# (by install name, not file path) and does NOT load a second copy. A duplicate
	# copy splits GLFW's monitor state -> glfwGetPrimaryMonitor() returns NULL ->
	# assertion abort at startup. This works on end-user machines with no /tmp/Rack2.
	install_name_tool -id /tmp/Rack2/$(TARGET) dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET)
	# Manually check that no nonstandard shared libraries are linked
	otool -L dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET)
	otool -L dist/"$(DIST_BUNDLE)"/Contents/MacOS/$(STANDALONE_TARGET)
	# Copy resources
	cp Info.plist dist/"$(DIST_BUNDLE)"/Contents/
	$(SED) 's/{RACK_VERSION}/$(RACK_VERSION)/g' dist/"$(DIST_BUNDLE)"/Contents/Info.plist
	$(SED) 's/{METARACK_VERSION}/$(METARACK_VERSION)/g' dist/"$(DIST_BUNDLE)"/Contents/Info.plist
	cp -R icon.icns dist/"$(DIST_BUNDLE)"/Contents/Resources/
	cp -R $(DIST_RES) dist/"$(DIST_BUNDLE)"/Contents/Resources/
	# Ad-hoc sign so the bundle launches at all (mandatory on Apple Silicon). The
	# secure-timestamp / hardened-runtime flags need a real identity, so they live
	# in the package target; override CODESIGN_IDENTITY for a Developer ID build.
	xattr -cr dist/"$(DIST_BUNDLE)"
	codesign --force --sign "$(CODESIGN_IDENTITY)" --entitlements Entitlements.plist --deep dist/"$(DIST_BUNDLE)"
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
ifneq ($(CODESIGN_IDENTITY),-)
	# Real Developer ID build: re-sign with hardened runtime + secure timestamp so
	# the app can be notarized. Set CODESIGN_IDENTITY="Developer ID Application: ..."
	# and CODESIGN_IDENTITY_INSTALLER="Developer ID Installer: ...".
	# Uses Entitlements-release.plist (no get-task-allow): the notary service
	# rejects binaries signed with the debug entitlement from Entitlements.plist.
	xattr -cr dist/"$(DIST_BUNDLE)"
	codesign --force --verbose --sign "$(CODESIGN_IDENTITY)" --options runtime --entitlements Entitlements-release.plist --timestamp --deep dist/"$(DIST_BUNDLE)"/Contents/Resources/$(TARGET) dist/"$(DIST_BUNDLE)"
	codesign --verify --deep --strict --verbose=2 dist/"$(DIST_BUNDLE)"
endif
	# Distributable ZIP of the app bundle. Works without a Developer ID: the bundle
	# is already ad-hoc signed by `dist`, so it launches locally; recipients without
	# a notarized build clear quarantine on first run (xattr -dr com.apple.quarantine).
	cd dist && zip -q -9 -r --symlinks "$(DIST_NAME).zip" "$(DIST_BUNDLE)"
	# Installer PKG (unsigned unless an installer identity is supplied below).
	mkdir -p dist/Component
	cp -R dist/"$(DIST_BUNDLE)" dist/Component/
	pkgbuild --identifier com.lcasarotti.metarack --component-plist Component.plist --root dist/Component --install-location /Applications dist/Component.pkg
	productbuild --distribution Distribution.xml --resources installer-resources --package-path dist dist/$(DIST_NAME).pkg
ifdef CODESIGN_IDENTITY_INSTALLER
	productsign --sign "$(CODESIGN_IDENTITY_INSTALLER)" dist/$(DIST_NAME).pkg dist/$(DIST_NAME)-signed.pkg
	mv dist/$(DIST_NAME)-signed.pkg dist/$(DIST_NAME).pkg
endif
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
	# Submit installer package to Apple. Register credentials once with:
	# xcrun notarytool store-credentials "MetaRack" --apple-id <email> --team-id <TEAMID> --password <app-specific-password>
	xcrun notarytool submit --keychain-profile "MetaRack" --wait dist/$(DIST_NAME).pkg
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
.PHONY: all dep run runr debug clean plugins dist sdk package lipo notarize mac-librack-link spike clap
