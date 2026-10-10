# libusb341 - libusb-1.0.dll for CH341A on the WCH vendor driver
#
# Build from the MSYS2 MINGW64 shell (or MINGW32 for a 32-bit DLL):
#   make            -> build/libusb-1.0.dll
#   make test       -> builds a simulator-backed DLL and runs the tests
#                      (compiles unmodified sources from ../IMSProg and ../SFP-Master)

ifeq ($(origin CC),default)
CC       := gcc
endif
WINDRES  ?= windres
IMSPROG  ?= ../IMSProg
SFPMASTER ?= ../SFP-Master

CFLAGS   ?= -O2
CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
LDFLAGS  += -shared -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--kill-at
LIBS     := -lsetupapi

SRC      := src/core.c src/ch341.c src/engine.c src/descriptor.c src/orig.c
HDR      := src/libusb.h src/shim.h

.PHONY: all tools test clean

all: build/libusb-1.0.dll tools

tools: build/ch341probe.exe

build build/mock build/inc/libusb-1.0 build/v106:
	mkdir -p $@

build/version.o: src/version.rc | build
	$(WINDRES) $< -o $@

build/libusb-1.0.dll: $(SRC) $(HDR) src/libusb-1.0.def build/version.o | build
	$(CC) $(CFLAGS) $(LDFLAGS) -s -o $@ $(SRC) build/version.o src/libusb-1.0.def $(LIBS) \
		-Wl,--out-implib,build/libusb-1.0.dll.a

# read-only hardware check (works with the shim and with the original libusb)
build/ch341probe.exe: tools/ch341probe.c src/libusb.h build/libusb-1.0.dll
	$(CC) -O2 -std=gnu11 -Wall -Isrc -s -o $@ tools/ch341probe.c -Lbuild -lusb-1.0

# ---------------------------------------------------------------------------
# Tests: same DLL plus a simulated CH341 (selected with LIBUSB341_MOCK=1)

MOCKDIR  := build/mock
MOCKDLL  := $(MOCKDIR)/libusb-1.0.dll
TCFLAGS  := -O1 -g -std=gnu11 -Isrc -Wno-unused-result

$(MOCKDLL): $(SRC) $(HDR) test/mock_ch341.c src/libusb-1.0.def build/version.o | $(MOCKDIR)
	$(CC) $(CFLAGS) -g -DLIBUSB341_MOCK $(LDFLAGS) -o $@ $(SRC) test/mock_ch341.c build/version.o \
		src/libusb-1.0.def $(LIBS) -Wl,--out-implib,$(MOCKDIR)/libusb-1.0.dll.a

# IMSProg's CH341A SPI driver (async transfers), unmodified
$(MOCKDIR)/test_spi.exe: test/test_spi.c test/test_util.h $(IMSPROG)/IMSProg_programmer/ch341a_spi.c $(MOCKDLL)
	$(CC) $(TCFLAGS) -I$(IMSPROG)/IMSProg_programmer -o $@ test/test_spi.c \
		$(IMSPROG)/IMSProg_programmer/ch341a_spi.c $(IMSPROG)/IMSProg_programmer/ch341a_gpio.c \
		-L$(MOCKDIR) -lusb-1.0

# SFP-Master's current CH341 I2C code (sync transfers), unmodified
$(MOCKDIR)/test_i2c.exe: test/test_i2c.c test/test_util.h $(SFPMASTER)/ch34x_i2c.c $(MOCKDLL)
	$(CC) $(TCFLAGS) -I$(SFPMASTER) -o $@ test/test_i2c.c $(SFPMASTER)/ch34x_i2c.c -L$(MOCKDIR) -lusb-1.0

# The code inside the shipped SFP-Master.exe (tag v1.0.6-win64; async reads), unmodified
build/inc/libusb-1.0/libusb.h: | build/inc/libusb-1.0
	echo '#include "../../../src/libusb.h"' > $@

build/v106/ch341funcs.c build/v106/ch341eeprom.h: | build/v106
	git -C $(SFPMASTER) show v1.0.6-win64:ch341funcs.c > build/v106/ch341funcs.c
	git -C $(SFPMASTER) show v1.0.6-win64:ch341eeprom.h > build/v106/ch341eeprom.h

$(MOCKDIR)/test_v106.exe: test/test_v106.c test/test_util.h build/v106/ch341funcs.c build/v106/ch341eeprom.h \
		build/inc/libusb-1.0/libusb.h $(MOCKDLL)
	$(CC) $(TCFLAGS) -Ibuild/inc -Ibuild/v106 -o $@ test/test_v106.c build/v106/ch341funcs.c -L$(MOCKDIR) -lusb-1.0

$(MOCKDIR)/test_api.exe: test/test_api.c test/test_util.h $(MOCKDLL)
	$(CC) $(TCFLAGS) -o $@ test/test_api.c -L$(MOCKDIR) -lusb-1.0

TESTS := $(MOCKDIR)/test_api.exe $(MOCKDIR)/test_spi.exe $(MOCKDIR)/test_i2c.exe $(MOCKDIR)/test_v106.exe

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t"; LIBUSB341_MOCK=1 ./$$t || exit 1; done
	@echo "=== all tests passed"

clean:
	rm -rf build
