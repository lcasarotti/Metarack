include $(RACK_DIR)/arch.mk

# The install location for `make install`
DEP_LOCAL ?= dep
$(shell mkdir -p $(DEP_LOCAL))
DEP_PATH := $(abspath $(DEP_LOCAL))

DEP_FLAGS += -g -O3
# This is needed for Rack for DAWs.
# Static libs don't usually compiled with -fPIC, but since we're including them in a shared library, it's needed.
DEP_FLAGS += -fPIC

# (No --target flag needed: GCC cross-compilers encode the target in the binary name)

ifdef ARCH_X64
	DEP_FLAGS += -march=nehalem
endif
ifdef ARCH_ARM64
	DEP_FLAGS += -march=armv8-a+fp+simd
endif

ifdef ARCH_MAC
	DEP_MAC_SDK_FLAGS := -mmacosx-version-min=10.9
	DEP_FLAGS += $(DEP_MAC_SDK_FLAGS)
	DEP_CXXFLAGS += -stdlib=libc++
	DEP_LDFLAGS += -stdlib=libc++
endif

DEP_CFLAGS += $(DEP_FLAGS)
DEP_CXXFLAGS += $(DEP_FLAGS)
DEP_LDFLAGS += $(DEP_FLAGS)

# Commands
WGET := wget -c
UNTAR := tar xf
UNZIP := unzip -o
CONFIGURE := ./configure --prefix="$(DEP_PATH)"
ifdef CROSS_COMPILE
	CONFIGURE += --host=$(CROSS_COMPILE)
endif

CMAKE := cmake
ifdef ARCH_WIN
	CMAKE += -DCMAKE_SYSTEM_NAME=Windows
endif
ifdef CROSS_COMPILE
	CMAKE += -DCMAKE_C_COMPILER=$(CROSS_COMPILE)-gcc
	CMAKE += -DCMAKE_CXX_COMPILER=$(CROSS_COMPILE)-g++
	CMAKE += -DCMAKE_RC_COMPILER=$(CROSS_COMPILE)-windres
endif
# We must specify the MSYS generator if in an MSYS shell
ifdef MSYSTEM
	CMAKE += -G "MSYS Makefiles"
endif
ifdef ARCH_MAC
	CMAKE += -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9
endif
ifdef ARCH_LIN
	CMAKE += -DCMAKE_SYSTEM_NAME=Linux
endif
CMAKE += -DCMAKE_INSTALL_PREFIX="$(DEP_PATH)"
# Some platforms try to install to lib64
CMAKE += -DCMAKE_INSTALL_LIBDIR=lib

ifdef ARCH_MAC
	SHA256SUM := shasum -a 256
	SED := sed -i ''
else
	SHA256SUM := sha256sum
	SED := sed -i
endif
SHA256 := sha256check() { echo "$$2  $$1" | $(SHA256SUM) -c; }; sha256check


# Export environment for all dependency targets
$(DEPS): export CFLAGS = $(DEP_CFLAGS)
$(DEPS): export CXXFLAGS = $(DEP_CXXFLAGS)
$(DEPS): export LDFLAGS = $(DEP_LDFLAGS)
ifdef CROSS_COMPILE
$(DEPS): export CC     = $(CROSS_COMPILE)-gcc
$(DEPS): export CXX    = $(CROSS_COMPILE)-g++
$(DEPS): export AR     = $(CROSS_COMPILE)-ar
$(DEPS): export RANLIB = $(CROSS_COMPILE)-ranlib
$(DEPS): export STRIP  = $(CROSS_COMPILE)-strip
endif

dep: $(DEPS)

cleandep:
ifeq ($(DEP_LOCAL), .)
	$(error Refusing to clean cwd)
endif
	rm -rfv $(DEP_LOCAL)

.PHONY: dep
