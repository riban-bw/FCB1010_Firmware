# One ROM plugin that lets the FCB1010 firmware switch firmware images.
#   make ORA=<one-rom checkout>/firmware/ora   build build/plugin_user.bin
#   make test                                  host + simulator tests (sdcc, s51, cc)
# Building the plugin needs arm-none-eabi-gcc (One ROM's plugin.mk); TOOLCHAIN=
# is passed through for a non-default one.

ORA ?=

.PHONY: all test clean

all:
ifeq ($(ORA),)
	$(error set ORA to the firmware/ora directory of a One ROM checkout)
endif
	$(MAKE) -f $(ORA)/plugin.mk SRC=$(CURDIR)/plugin_main.c BUILD_DIR=$(CURDIR)/build \
		ORA_INCLUDE=$(ORA) EXTRA_C_FLAGS=-I$(CURDIR) $(if $(TOOLCHAIN),TOOLCHAIN=$(TOOLCHAIN),)

test:
	sh test/run.sh

clean:
	rm -rf build
