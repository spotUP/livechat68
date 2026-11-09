# LiveChat68 standalone build.
#
# This tree holds the door (examples/livechat) and the part of the AmiExpress
# web C door SDK it needs (src/, include/), nothing else. Two targets:
#
#   make            68K door binary, build/amiga/livechat (vbcc + vasm)
#   make test       host tests: chat protocol against a stub server, and the
#                   screen layout (any C99 compiler + OpenSSL 3)
#
# Point the variables below at your toolchain. Every path can be given on the
# command line, e.g.  make VBCC=/opt/vbcc NDK32=/opt/NDK3.2R4 AMISSL_SDK=/opt/AmiSSL
#
# vbcc:        http://sun.hasenbraten.de/vbcc/  (target m68k-amigaos)
# vasm:        http://sun.hasenbraten.de/vasm/  (vasmm68k_mot)
# NDK 3.2:     https://www.hyperion-entertainment.com/  (Include_H and
#              SANA+RoadshowTCP-IP/netinclude are used)
# AmiSSL SDK:  https://github.com/jens-maus/amissl/releases  (AmiSSL/Developer/include)

VBCC       ?= /opt/vbcc
NDK32      ?= /opt/NDK3.2R4
NDK        ?= $(NDK32)/Include_H
AMISSL_SDK ?= /opt/AmiSSL-SDK
VASM       ?= vasmm68k_mot
VC          = VBCC=$(VBCC) vc

CC         ?= cc
OPENSSL    ?= /opt/homebrew/opt/openssl@3
HOST_CFLAGS = -std=c99 -Wall -Wextra -Iinclude -D_DEFAULT_SOURCE -I$(OPENSSL)/include
TLS_LIBS    = -L$(OPENSSL)/lib -lssl -lcrypto

AMIGA_CFLAGS = +aos68k -c99 -O2 -DAMIGA -Iinclude -I"$(VBCC)/targets/m68k-amigaos/include" -I"$(NDK)"

# Board-independent code shared by both builds.
COMMON = ae_host ui_cells ae_chat ae_request ae_http ae_net ae_net_tls json_lite \
         ae_dos ae_session ae_session_bbs ae_board_screen ae_usernote ae_chatprefs \
         ui_ansi ui_chrome ui_key ui_input ui_list ui_profile ui_screen ui_theme

# The 68K door adds the AmiExpress transport (door message port) and AmigaDOS
# file access. The host build swaps those for a socket transport and POSIX files.
AMIGA_ONLY = ae_transport_amiga ae_dos_amiga ae_chunk ae_field_shape
HOST_ONLY  = ae_transport_native ae_dos_native

AMIGA_OBJS = $(patsubst %,build/amiga/%.o,$(COMMON) $(AMIGA_ONLY)) build/amiga/ui_cells_68k.o
HOST_SRCS  = $(patsubst %,src/%.c,$(COMMON) $(HOST_ONLY))

DOOR = examples/livechat/livechat.c examples/livechat/livechat_ui.c examples/livechat/livechat_ui.h

.PHONY: all amiga test clean
all: amiga
amiga: build/amiga/livechat

# vc reads a -I path with a space in it as a second input file, so the NDK
# network headers reach it through links under build/.
NET_SHIM    = build/amiga/ndk-shim
NET_INCLUDE = build/amiga/netinclude
$(NET_SHIM)/devices/timer.h:
	@mkdir -p $(NET_SHIM)/devices
	@ln -sf "$(NDK32)/Include_H/devices/timer.h" $@
	@ln -sfn "$(NDK32)/SANA+RoadshowTCP-IP/netinclude" $(NET_INCLUDE)

build/amiga/%.o: src/%.c
	@mkdir -p build/amiga
	$(VC) $(AMIGA_CFLAGS) -c $< -o $@

build/amiga/ae_net.o: src/ae_net.c $(NET_SHIM)/devices/timer.h
	$(VC) -I$(NET_SHIM) -I$(NET_INCLUDE) $(AMIGA_CFLAGS) -c $< -o $@

build/amiga/ae_net_tls.o: src/ae_net_tls.c $(NET_SHIM)/devices/timer.h
	$(VC) -I$(NET_SHIM) -I$(NET_INCLUDE) -I$(AMISSL_SDK)/AmiSSL/Developer/include $(AMIGA_CFLAGS) -c $< -o $@

build/amiga/%.o: src/%.s
	@mkdir -p build/amiga
	$(VASM) -quiet -Fhunk -m68000 -o $@ $<

# vbcc libraries are plain concatenated objects; the linker pulls only what
# the door calls. ae_host goes first.
build/amiga/libae.lib: $(AMIGA_OBJS)
	cat $(AMIGA_OBJS) > $@

build/amiga/livechat: $(DOOR) build/amiga/libae.lib
	$(VC) $(AMIGA_CFLAGS) -Iexamples/livechat examples/livechat/livechat.c examples/livechat/livechat_ui.c build/amiga/libae.lib -lauto -o $@

test: build/test_chat build/test_livechat_ui
	./build/test_chat
	./build/test_livechat_ui

build/test_chat: $(DOOR) $(HOST_SRCS) tests/test_chat.c tests/stub_server.c
	@mkdir -p build
	$(CC) $(HOST_CFLAGS) -Isrc -Iexamples/livechat examples/livechat/livechat_ui.c $(HOST_SRCS) tests/stub_server.c tests/test_chat.c $(TLS_LIBS) -o $@

build/test_livechat_ui: src/ui_cells.c src/ui_ansi.c src/ui_theme.c examples/livechat/livechat_ui.c tests/test_livechat_ui.c
	@mkdir -p build
	$(CC) $(HOST_CFLAGS) -Iexamples/livechat src/ui_cells.c src/ui_ansi.c src/ui_theme.c examples/livechat/livechat_ui.c tests/test_livechat_ui.c -o $@

clean:
	rm -rf build
