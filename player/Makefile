#-------------------------------------------------------------------------------
# Our64 player: Not64's CPU core (as in bench/) with GLideN64 on the GX2
# backend (as in replay/, GFX=gx2). One emulation thread on the codegen core
# runs the CPU and decodes each graphics task when the game sends it; audio
# goes out through AX.
#-------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_NAME	:= Our64
APP_SHORTNAME	:= Our64
APP_AUTHOR	:= Our64

# The public build's icon is the grey N64 controller (tools/make-icon.sh
# our64); the developer's own build keeps the cat (Zach, 2026-10-30).
ifeq ($(RELEASE),1)
APP_ICON	:=	$(CURDIR)/../meta/icon.png
else
APP_ICON	:=	$(CURDIR)/../replay/meta/icon-gx2.png
endif
include $(DEVKITPRO)/wut/share/wut_rules

GFX		:=	gx2
include ../replay/gliden64.mk

# Not64, vendored (third_party/not64/VENDORED.md); NOT64= builds another tree.
NOT64		?=	$(CURDIR)/../third_party/not64
BENCH		:=	$(CURDIR)/../bench
REPLAY		:=	$(CURDIR)/../replay
CAFEGLSL	:=	$(CURDIR)/../third_party/cafeglsl
# libgui (dborth), vendored: Our64's screens draw with its GX2 renderers.
LIBGUI		:=	$(CURDIR)/../third_party/libgui/source
LIBGUI_SRC	:=	$(patsubst $(LIBGUI)/%,%,$(wildcard $(LIBGUI)/libgui/*.cpp $(LIBGUI)/drivers/*.cpp \
			$(LIBGUI)/drivers/wut/*.cpp $(LIBGUI)/drivers/wut/shaders/*.cpp))
LIBGUI_A	=	$(BUILD)/libgui.a
# minilzo (LZO 2.09, GPL), vendored: Goomba Color compresses its Game Boy saves with it.
MINILZO		:=	$(CURDIR)/../third_party/minilzo
# cxd4's RSP interpreter (CC0), vendored: the GB Tower's RSP program (rsp_lle.c).
CXD4		:=	$(CURDIR)/../third_party/cxd4
CXD4_SRC	:=	su.c vu/vu.c vu/multiply.c vu/add.c vu/select.c vu/logical.c vu/divide.c
# FatFs R0.16 (BSD-style), vendored read-only: FAT32 and exFAT on USB drives. An
# archive, so its objects link only once something calls them.
FATFS		:=	$(CURDIR)/../third_party/fatfs
FATFS_A		=	$(BUILD)/fatfs.a
# make RELEASE=1: the public build, into release/our64.wuhb. It leaves out what
# only the developer's own card uses: Delete in Our ROMs and the graphics
# capture (minus with ZL and ZR, and the capture switches), and has its own
# icon (APP_ICON, above wut_rules, which reads it when included).
RELEASE		?=	0
ifeq ($(RELEASE),1)
TARGET		:=	release/our64
BUILD		:=	build-release
REL_DEFINES	:=	-DOUR64_RELEASE
else
TARGET		:=	our64
BUILD		:=	build
REL_DEFINES	:=
endif

# The same Not64 core as the bench, minus nothing.
include $(BENCH)/not64.mk

BENCH_SRC	:=	our64_jit.c lwp_heap.c report.c romcache.c capture.c

OFILES		:=	$(addprefix $(BUILD)/not64/,$(NOT64_SRC:.c=.o)) \
			$(addprefix $(BUILD)/bench/,$(BENCH_SRC:.c=.o)) \
			$(BUILD)/source/main.o $(BUILD)/source/plugins.o $(BUILD)/source/audio.o $(BUILD)/source/video.o \
			$(BUILD)/source/alloc_stats.o $(BUILD)/source/saves.o $(BUILD)/source/saves_wiiu.o $(BUILD)/source/states.o $(BUILD)/source/states_wiiu.o $(BUILD)/source/writer.o $(BUILD)/source/input.o $(BUILD)/source/input_wiiu.o \
			$(BUILD)/source/prompt.o $(BUILD)/source/notices.o $(BUILD)/source/ui_wiiu.o $(BUILD)/source/tv_pass.o $(BUILD)/source/ui_draw.o $(BUILD)/source/theme.o $(BUILD)/source/pause.o $(BUILD)/source/pause_wiiu.o \
			$(BUILD)/source/library.o $(BUILD)/source/library_wiiu.o $(BUILD)/source/setup.o $(BUILD)/source/setup_wiiu.o $(BUILD)/source/gamedb.o $(BUILD)/source/covertitles.o $(BUILD)/source/covertitles_find.o $(BUILD)/source/covercodes.o $(BUILD)/source/covers.o $(BUILD)/source/covers_wiiu.o $(BUILD)/source/gameopt.o $(BUILD)/source/keyboard_wiiu.o \
			$(BUILD)/source/tpak.o $(BUILD)/source/tpak_wiiu.o $(BUILD)/source/mocha_wiiu.o $(BUILD)/source/usbfs.o $(BUILD)/source/usbfs_wiiu.o $(BUILD)/source/cpuprof.o $(BUILD)/minilzo.o $(BUILD)/cxd4.o $(BUILD)/source/gb_tower_hle.o \
			$(BUILD)/replay/core_wiiu.o \
			$(addprefix $(BUILD)/gliden64/,$(GLIDEN64_CORE:.cpp=.o) $(GLIDEN64_C:.c=.o))

# Not64: the bench's switches. GLideN64: the replay's, without per-command timing.
NOT64_DEFINES	:=	-D__WIIU__ -D__WUT__ -DHW_RVL -DNOASM -DUSE_EXPANSION \
			-DUSE_RECOMP_CACHE -DPPC_DYNAREC -DFASTMEM -DRELEASE -DPROFILE
# Diagnostic builds: send instruction groups to the interpreter, for example
# make clean-core && make NOT64_EXTRA=-DINTERPRET_FP (see Not64's ppc/Interpreter.h).
NOT64_DEFINES	+=	$(NOT64_EXTRA)
NOT64_INCLUDE	:=	-I$(BENCH)/shim -I$(BENCH)/source -I$(NOT64) -I$(MINILZO) -I$(FATFS) \
			$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-I$(dir)/include)
CFLAGS		:=	-g -O2 -std=gnu11 -fgnu89-inline -ffunction-sections -fno-strict-aliasing \
			-Wall -Wno-unused-variable -Wno-unused-but-set-variable \
			-Wno-unused-function -Wno-pointer-sign -Wno-parentheses \
			-Wno-int-conversion -Wno-incompatible-pointer-types \
			-Wno-error=implicit-function-declaration \
			$(MACHDEP) $(NOT64_DEFINES) $(NOT64_INCLUDE) $(REL_DEFINES)

GDEFINES	:=	-D__WIIU__ -D__WUT__ -DOS_LINUX $(filter-out -DOUR64_CMD_TIMING,$(GLIDEN64_DEFINES)) \
			-DREPORT_NO_CONSOLE
GINCLUDE	:=	$(GLIDEN64_INCLUDE) -I$(CAFEGLSL)/include -I$(BENCH)/source \
			$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-I$(dir)/include)
# -ffp-contract=off: no fused multiply-add, so float results match the PC build.
GCOMMON		:=	-g -O2 -ffunction-sections -ffp-contract=off $(MACHDEP) $(GDEFINES) $(GINCLUDE)
GCFLAGS		:=	$(GCOMMON) -std=gnu11 -w
CXXFLAGS	:=	$(GCOMMON) -std=gnu++17 -w

LDFLAGS		:=	-g $(MACHDEP) $(RPXSPECS) -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--wrap=doRspCycles -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
LIBPATHS	:=	-L$(CAFEGLSL)/lib $(foreach dir,$(PORTLIBS) $(WUT_ROOT),-L$(dir)/lib)
LIBS		:=	-lcafeglsl -lturbojpeg -lfreetype -lpng16 -lbz2 -lz -lwut -lm -lstdc++
LIBGUI_FLAGS	:=	-g -O2 -ffunction-sections $(MACHDEP) -D__WIIU__ -D__WUT__ -DOUR64 -std=gnu++17 -w \
			-I$(LIBGUI) -I$(LIBGUI)/libgui -I$(DEVKITPRO)/portlibs/ppc/include/freetype2 \
			$(foreach dir,$(PORTLIBS) $(WUT_ROOT),-I$(dir)/include)
# Our own screens get warnings (libgui's -w hid a function that called itself forever).
UI_FLAGS	:=	$(filter-out -w,$(LIBGUI_FLAGS)) -Wall -Wno-unused-function -Werror=infinite-recursion $(REL_DEFINES)
export LD	:=	$(CXX)

.PHONY: all clean clean-core
all: $(TARGET).wuhb

$(TARGET).wuhb: $(TARGET).rpx
$(TARGET).rpx: $(BUILD)/$(TARGET).elf
	@mkdir -p $(dir $@)
	@elf2rpl $< $@
	@echo built ... $@

$(BUILD)/$(TARGET).elf: $(OFILES) $(LIBGUI_A) $(FATFS_A)
	@mkdir -p $(dir $@)
	@echo linking ... $(notdir $@)
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBGUI_A) $(FATFS_A) $(LIBPATHS) $(LIBS) -o $@

$(LIBGUI_A): $(addprefix $(BUILD)/libgui/,$(LIBGUI_SRC:.cpp=.o))
	@rm -f $@
	@$(AR) rcs $@ $^

$(BUILD)/libgui/%.o: $(LIBGUI)/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(LIBGUI_FLAGS) -c $< -o $@

# Our64's screens: libgui's headers, GLideN64's GX2 backend (restoreState), the portrait.
$(BUILD)/source/ui_wiiu.o: source/ui_wiiu.cpp data/cat1.jpg data/cat2.jpg data/n64-controller.png
	@mkdir -p $(dir $@)
	@echo ui_wiiu.cpp
	@$(CXX) -MMD -MP $(UI_FLAGS) -I$(BENCH)/source -DOUR64_DATA='"$(CURDIR)/data"' -c $< -o $@

# The system keyboard (nn::swkbd), drawn through the UI's video driver.
$(BUILD)/source/keyboard_wiiu.o: source/keyboard_wiiu.cpp
	@mkdir -p $(dir $@)
	@echo keyboard_wiiu.cpp
	@$(CXX) -MMD -MP $(UI_FLAGS) -I$(BENCH)/source -c $< -o $@

# The screens as drawing (tests/ui_shots.cpp draws them on a PC too).
$(BUILD)/source/ui_draw.o: source/ui_draw.cpp
	@mkdir -p $(dir $@)
	@echo ui_draw.cpp
	@$(CXX) -MMD -MP $(UI_FLAGS) -c $< -o $@

$(BUILD)/not64/%.o: $(NOT64)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

# wut's headers need C23; Not64's old-style prototypes need gnu11.
$(BUILD)/bench/%.o: $(BENCH)/source/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -std=gnu23 -DREPORT_NO_CONSOLE -DREPORT_FILE='"player-log.txt"' -c $< -o $@
# main.c's log header names the build (__DATE__ __TIME__): it is compiled
# again whenever any other part changed, so the header matches what runs.
$(BUILD)/source/main.o: $(filter-out $(BUILD)/source/main.o,$(OFILES)) $(LIBGUI_A)

$(FATFS_A): $(BUILD)/fatfs/ff.o $(BUILD)/fatfs/ffunicode.o
	@rm -f $@
	@$(AR) rcs $@ $^

$(BUILD)/fatfs/%.o: $(FATFS)/%.c
	@mkdir -p $(dir $@)
	@echo fatfs/$*.c
	@$(CC) -MMD -MP -g -O2 -std=gnu11 -ffunction-sections $(MACHDEP) -w -c $< -o $@

$(BUILD)/minilzo.o: $(MINILZO)/minilzo.c
	@mkdir -p $(dir $@)
	@echo minilzo.c
	@$(CC) -MMD -MP -g -O2 -ffunction-sections $(MACHDEP) -w -c $< -o $@

# cxd4 and rsp_lle.c, linked into one object whose only global symbols are
# rsp_lle_*: cxd4's SR, CR, DMEM, IMEM, message and the rest stay out of the
# way of rsp_hle's and GLideN64's names. gnu11, not gnu23: cxd4 predates C23.
CXD4_FLAGS	:=	-g -O2 -std=gnu11 -ffunction-sections -fno-strict-aliasing $(MACHDEP)
$(BUILD)/cxd4/%.o: $(CXD4)/%.c
	@mkdir -p $(dir $@)
	@echo cxd4/$*.c
	@$(CC) -MMD -MP $(CXD4_FLAGS) -w -c $< -o $@

$(BUILD)/cxd4/rsp_lle.o: source/rsp_lle.c
	@mkdir -p $(dir $@)
	@echo rsp_lle.c
	@$(CC) -MMD -MP $(CXD4_FLAGS) -Wall -I$(CXD4) -c $< -o $@

$(BUILD)/cxd4.o: $(BUILD)/cxd4/rsp_lle.o $(addprefix $(BUILD)/cxd4/,$(CXD4_SRC:.c=.o))
	@echo linking ... $(notdir $@)
	@$(CC) $(MACHDEP) -r -nostdlib $^ -o $@.tmp
	@$(OBJCOPY) -w --keep-global-symbol='rsp_lle_*' $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/source/%.o: source/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(CFLAGS) -std=gnu23 -c $< -o $@

$(BUILD)/source/%.o: source/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -Wall -c $< -o $@
$(BUILD)/replay/%.o: $(REPLAY)/source/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/gliden64/%.o: $(G)/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/gliden64/%.o: $(G)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) -MMD -MP $(GCFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) $(TARGET).wuhb $(TARGET).rpx

clean-core:
	@rm -rf $(BUILD)/not64 $(TARGET).wuhb $(TARGET).rpx

-include $(OFILES:.o=.d)
-include $(wildcard $(BUILD)/cxd4/*.d $(BUILD)/cxd4/vu/*.d $(BUILD)/fatfs/*.d)
