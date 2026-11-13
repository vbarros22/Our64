# PC tests for the player's portable parts. make -C tests
CC	?=	cc
CFLAGS	:=	-std=gnu11 -O1 -g -Wall -Wextra -I../player/source
BUILD	:=	build
TESTS	:=	test_saves test_states test_input test_prompt test_notices test_pause test_library test_setup test_tpak test_rsp_lle test_gb_tower test_usbfs test_covers test_lwp_heap test_gameopt test_speed

.PHONY: all clean shots
all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do echo "$$t:"; $(BUILD)/$$t || exit 1; done

# The JIT code heap (bench/source/lwp_heap.c), its coreinit calls on malloc (stub/).
$(BUILD)/test_lwp_heap: test_lwp_heap.c ../bench/source/lwp_heap.c stub/coreinit/memexpheap.h stub/wut_types.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Istub -I../bench/shim -o $@ test_lwp_heap.c

$(BUILD)/test_saves: test_saves.c ../player/source/saves.c ../player/source/saves.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_saves.c ../player/source/saves.c

$(BUILD)/test_states: test_states.c ../player/source/states.c ../player/source/states.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_states.c ../player/source/states.c -lz

$(BUILD)/test_input: test_input.c ../player/source/input.c ../player/source/input.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_input.c ../player/source/input.c -lm

$(BUILD)/test_prompt: test_prompt.c ../player/source/prompt.c ../player/source/prompt.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_prompt.c ../player/source/prompt.c

$(BUILD)/test_notices: test_notices.c ../player/source/notices.c ../player/source/notices.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_notices.c ../player/source/notices.c

$(BUILD)/test_pause: test_pause.c ../player/source/pause.c ../player/source/pause.h ../player/source/prompt.c ../player/source/states.h ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pause.c ../player/source/pause.c ../player/source/prompt.c ../player/source/theme.c -lm

$(BUILD)/test_library: test_library.c ../player/source/library.c ../player/source/library.h ../player/source/prompt.c ../player/source/saves.c ../player/source/gamedb.c ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_library.c ../player/source/library.c ../player/source/prompt.c ../player/source/saves.c ../player/source/gamedb.c ../player/source/theme.c -lm

$(BUILD)/test_setup: test_setup.c ../player/source/setup.c ../player/source/setup.h ../player/source/input.c ../player/source/prompt.c ../player/source/pause.c ../player/source/theme.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_setup.c ../player/source/setup.c ../player/source/input.c ../player/source/prompt.c ../player/source/pause.c ../player/source/theme.c -lm

$(BUILD)/test_tpak: test_tpak.c ../player/source/tpak.c ../player/source/tpak.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../third_party/minilzo -o $@ test_tpak.c ../player/source/tpak.c ../third_party/minilzo/minilzo.c

$(BUILD)/test_covers: test_covers.c ../player/source/covers.c ../player/source/covers.h ../player/source/covertitles.c ../player/source/covertitles_find.c ../player/source/covertitles.h ../player/source/covercodes.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_covers.c ../player/source/covers.c ../player/source/covertitles.c ../player/source/covertitles_find.c ../player/source/covercodes.c

$(BUILD)/test_gameopt: test_gameopt.c ../player/source/gameopt.c ../player/source/gameopt.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_gameopt.c ../player/source/gameopt.c

$(BUILD)/test_speed: test_speed.c ../player/source/speed.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_speed.c

# cxd4 (vendored) at -O2, as in the player; its own warnings off.
CXD4	:=	../third_party/cxd4
CXD4_SRC :=	su.c vu/vu.c vu/multiply.c vu/add.c vu/select.c vu/logical.c vu/divide.c
$(BUILD)/test_rsp_lle: test_rsp_lle.c ../player/source/rsp_lle.c ../player/source/rsp_lle.h $(addprefix $(CXD4)/,$(CXD4_SRC))
	@mkdir -p $(BUILD)/cxd4/vu
	@for f in $(CXD4_SRC); do $(CC) -std=gnu11 -O2 -g -w -c $(CXD4)/$$f -o $(BUILD)/cxd4/$${f%.c}.o || exit 1; done
	$(CC) $(CFLAGS) -O2 -I$(CXD4) -o $@ test_rsp_lle.c ../player/source/rsp_lle.c $(addprefix $(BUILD)/cxd4/,$(CXD4_SRC:.c=.o))

# The GB Tower's program in C against cxd4 (built by the rule above).
$(BUILD)/test_gb_tower: test_gb_tower.c ../player/source/gb_tower_hle.c ../player/source/gb_tower_hle.h $(BUILD)/test_rsp_lle
	$(CC) $(CFLAGS) -O2 -I$(CXD4) -o $@ test_gb_tower.c ../player/source/gb_tower_hle.c ../player/source/rsp_lle.c \
		$(addprefix $(BUILD)/cxd4/,$(CXD4_SRC:.c=.o))

# USB drives (usbfs.c) on disk images. test_usbfs and FatFs get ASan and UBSan:
# the images include broken volumes. make_disk makes the images with a
# read-write FatFs: its own copies of the sources beside tests/fatfs_rw/ffconf.h
# (ff.h includes "ffconf.h" from its own folder first, so -I cannot swap it).
FATFS	:=	../third_party/fatfs
FATFS_SRC :=	ff.c ff.h diskio.h ffunicode.c
SAN	:=	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
USBFS_IMGS :=	$(BUILD)/usbfs-images
$(BUILD)/fatfs_ro/%.o: $(FATFS)/%.c $(addprefix $(FATFS)/,$(FATFS_SRC) ffconf.h)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O1 -g -w $(SAN) -c $< -o $@

$(BUILD)/make_disk: make_disk.c usbfs_diskio_file.c fatfs_rw/ffconf.h $(addprefix $(FATFS)/,$(FATFS_SRC))
	@mkdir -p $(BUILD)/fatfs_rw
	cp $(addprefix $(FATFS)/,$(FATFS_SRC)) fatfs_rw/ffconf.h $(BUILD)/fatfs_rw/
	$(CC) -std=gnu11 -O1 -g -w -c $(BUILD)/fatfs_rw/ff.c -o $(BUILD)/fatfs_rw/ff.o
	$(CC) -std=gnu11 -O1 -g -w -c $(BUILD)/fatfs_rw/ffunicode.c -o $(BUILD)/fatfs_rw/ffunicode.o
	$(CC) $(CFLAGS) -I$(BUILD)/fatfs_rw -o $@ make_disk.c usbfs_diskio_file.c $(BUILD)/fatfs_rw/ff.o $(BUILD)/fatfs_rw/ffunicode.o

$(USBFS_IMGS): $(BUILD)/make_disk
	$(BUILD)/make_disk $(BUILD)
	@touch $@

$(BUILD)/test_usbfs: test_usbfs.c usbfs_diskio_file.c ../player/source/usbfs.c ../player/source/usbfs.h \
		$(BUILD)/fatfs_ro/ff.o $(BUILD)/fatfs_ro/ffunicode.o $(USBFS_IMGS)
	$(CC) $(CFLAGS) $(SAN) -I$(FATFS) -o $@ test_usbfs.c usbfs_diskio_file.c ../player/source/usbfs.c \
		$(BUILD)/fatfs_ro/ff.o $(BUILD)/fatfs_ro/ffunicode.o

# Every screen drawn on the PC with the console's drawing code and font: results/shots.
LIBGUI	:=	../third_party/libgui/source
SHOTS_GUI :=	libgui/GuiGeometry libgui/GuiPaint libgui/GuiTextRenderer libgui/GuiMeshCache drivers/Mutex
SHOTS_C	:=	pause.c prompt.c setup.c input.c library.c notices.c saves.c gamedb.c states.c theme.c covers.c covertitles.c covertitles_find.c covercodes.c
SHOTS_FLAGS :=	-O1 -g -I../player/source -I$(LIBGUI) -I$(LIBGUI)/libgui $(shell pkg-config --cflags freetype2 libpng)
$(BUILD)/ui_shots: ui_shots.cpp ../player/source/ui_draw.cpp ../player/source/ui_draw.h ../player/source/ui.h \
		$(addprefix ../player/source/,$(SHOTS_C)) $(addprefix $(LIBGUI)/,$(addsuffix .cpp,$(SHOTS_GUI)))
	@mkdir -p $(BUILD)/shots
	@for f in $(SHOTS_C); do $(CC) -std=gnu11 $(SHOTS_FLAGS) -w -c ../player/source/$$f -o $(BUILD)/shots/$${f%.c}.o || exit 1; done
	@for f in $(SHOTS_GUI); do o=$(BUILD)/shots/$$(basename $$f).o; [ $$o -nt $(LIBGUI)/$$f.cpp ] || \
		$(CXX) -std=gnu++17 $(SHOTS_FLAGS) -w -c $(LIBGUI)/$$f.cpp -o $$o || exit 1; done
	$(CXX) -std=gnu++17 $(SHOTS_FLAGS) -Wall -o $@ ui_shots.cpp ../player/source/ui_draw.cpp $(BUILD)/shots/*.o \
		$(shell pkg-config --libs freetype2 libpng libturbojpeg) -lz -lm

shots: $(BUILD)/ui_shots
	$(BUILD)/ui_shots
	$(BUILD)/ui_shots --theme comrade --out ../results/shots/comrade

clean:
	rm -rf $(BUILD)
