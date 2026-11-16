# Build term16.exe for 16-bit MS-DOS with Open Watcom and libc16.
#   make                         fetch the pinned libc16 release into
#                                build/libc16, then build term16.exe
#   make LIBC16_ROOT=../libc16   use a libc16 checkout built with its make
#   make test                    host unit tests, then DOSBox-X tests
#
# term16.exe is compiled and linked against libc16 only (its ABI is in
# libc16's docs/ABI.md), with the Open Watcom snapshot libc16 was built
# with: the libc16 tree's tools/fetch-watcom fetches it into build/tools.
# build/t16test.exe is the same program with the test hooks of
# src/screen.c (keyboard, screen dumps) and src/dosfile.c (a failing
# rename), built with -DTERM16_TEST, for tests/run.py.

LIBC16_ROOT ?= build/libc16
# A release tree, or a checkout, whose make stages the same layout in
# build/stage.
LIBC16_DIR  := $(if $(wildcard $(LIBC16_ROOT)/build/stage/VERSION),$(LIBC16_ROOT)/build/stage,$(LIBC16_ROOT))
LIBC16      := $(LIBC16_DIR)/lib/LIBC16.LIB
TOOLS_ENV   := build/tools.env
WATCOM_ENV   = . ./$(TOOLS_ENV) && PATH=$$WATCOM/binl64:$$PATH

VERSION := $(shell cat VERSION)

CC     = wcc
CFLAGS = -q -zq -zl -zld -x -ms -0 -os -s -bt=dos -wx -we \
         -I$(LIBC16_DIR)/include '-DVERSION="$(VERSION)"'
LDOPTS = option quiet, nodefaultlibs, stack=4096

HOSTCC     = cc
HOSTCFLAGS = -std=c89 -pedantic -Wall -Wextra -Werror -O1 -Isrc

NAMES   = main config uart uprobe screen term cp437 menu zmodem zmcrc dlname dosfile
HEADERS = src/term16.h
OBJS    = $(NAMES:%=build/obj/%.obj)
# Built again with -DTERM16_TEST for build/t16test.exe
T_NAMES = screen dosfile
T_OBJS  = $(filter-out $(T_NAMES:%=build/obj/%.obj),$(OBJS)) \
          $(T_NAMES:%=build/obj/%_t.obj)
PORTABLE = src/config.c src/term.c src/cp437.c src/zmcrc.c src/dlname.c \
           src/uprobe.c

all: term16.exe

term16.exe: $(OBJS) $(LIBC16) $(TOOLS_ENV)
	$(WATCOM_ENV) && wlink $(LDOPTS), map=build/term16.map \
	  format dos name $@ $(OBJS:%=file %) library $(LIBC16)

build/t16test.exe: $(T_OBJS) $(LIBC16) $(TOOLS_ENV)
	$(WATCOM_ENV) && wlink $(LDOPTS), map=build/t16test.map \
	  format dos name $@ $(T_OBJS:%=file %) library $(LIBC16)

build/obj/%.obj: src/%.c $(HEADERS) VERSION $(LIBC16) $(TOOLS_ENV)
	mkdir -p build/obj
	$(WATCOM_ENV) && $(CC) $(CFLAGS) -fo=$@ $<

build/obj/%_t.obj: src/%.c $(HEADERS) VERSION $(LIBC16) $(TOOLS_ENV)
	mkdir -p build/obj
	$(WATCOM_ENV) && $(CC) $(CFLAGS) -DTERM16_TEST -fo=$@ $<

# The pinned libc16 release (scripts/fetch-libc16). A LIBC16_ROOT given
# on the command line must already be built.
ifeq ($(LIBC16_ROOT),build/libc16)
$(LIBC16): scripts/fetch-libc16
	scripts/fetch-libc16 $(LIBC16_ROOT)
endif

# The Open Watcom snapshot libc16 names, as shell assignments.
$(TOOLS_ENV): $(LIBC16)
	mkdir -p build
	watcom=$$(sh $(LIBC16_DIR)/tools/fetch-watcom $(CURDIR)/build/tools) \
	  && printf 'WATCOM=%s\nexport WATCOM\n' "$$watcom" > $@

build/unit: tests/unit/unit.c $(PORTABLE) $(HEADERS)
	mkdir -p build
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tests/unit/unit.c $(PORTABLE)

unit: build/unit
	./build/unit

test: unit term16.exe build/t16test.exe
	sh tests/run.sh $(CURDIR)/term16.exe $(CURDIR)/build/t16test.exe

src/cp437.c: tools/gen-cp437.py
	python3 tools/gen-cp437.py > $@

clean:
	rm -rf *.o *.obj *.err *.map term16.exe build/obj build/unit \
	  build/t16test.exe build/*.map

.PHONY: all unit test clean
