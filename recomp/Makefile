# Compile the recompiled game code.
#   make gen              # regenerate gen/ from analysis/dlc_game.exe
#   make TARGET=a64 -j8   # Switch NRO: build/a64/pvzdlc.nro (devkitA64 + libnx; software renderer for now)
#   make TARGET=a32 -j8   # AArch32 objects (clang; runtime needs the libnx32 docker image)
#   make TARGET=host -j8  # macOS test build: build/host/pvzdlc_host (-O1, call trace on)
#   make TARGET=play -j8  # macOS build to play: build/play/pvzdlc_host (-O2, no call trace); the Mac app: ../tools/package_mac.sh
#   make TARGET=android -j8  # Android arm64: build/android/libmain.so (NDK r27d; ../tools/build_android.sh makes the APK)
#   make TARGET=linux -j8    # Linux x86_64 / TARGET=linux-arm64: build/linux*/pvzdlc (in Docker: ../tools/build_linux.sh)
TARGET ?= a64
PY ?= python3
# the downloaded toolchains (NDK dmg, SDL2 2.32.10 source + its mingw files, llvm-mingw): any folder, e.g. make TOOLCHAIN=...
TOOLCHAIN ?= $(HOME)/sixtyfour/toolchain
GAME_EXE ?= ../analysis/dlc_game.exe
BUILD := build/$(TARGET)
# libopenmpt 0.8.9 (BSD-3) plays the MO3 music; its own makefile builds it (bundled stb_vorbis/minimp3/miniz)
OMPT := third_party/libopenmpt
OMPT_FLAGS := STATIC_LIB=1 SHARED_LIB=0 EXAMPLES=0 OPENMPT123=0 TEST=0 NO_ZLIB=1 NO_MPG123=1 NO_OGG=1 NO_VORBIS=1 \
              NO_VORBISFILE=1 NO_PORTAUDIO=1 NO_PORTAUDIOCPP=1 NO_PULSEAUDIO=1 NO_SDL2=1 NO_FLAC=1 NO_SNDFILE=1

GEN_SRC := $(sort $(wildcard gen/code_*.c)) $(sort $(wildcard gen/big_*.c)) gen/dispatch.c gen/imports.c gen/conts.c
RT_SRC := runtime/rt_core.c runtime/rt_mem.c runtime/rt_sys.c runtime/rt_loader.c runtime/rt_hle.c runtime/rt_com.c runtime/rt_eh.c runtime/rt_audio.c runtime/rt_vorbis.c runtime/rt_pool.c runtime/rt_prof.c \
          $(sort $(wildcard runtime/hle_*.c))
COMMON := -O2 -fno-strict-aliasing -Iruntime -Igen -Wall -Wno-unused-label -Wno-unused-variable \
          -Wno-unused-but-set-variable -ffunction-sections -MMD -MP

ifneq ($(filter host play count,$(TARGET)),)
CC := gcc-15
ifeq ($(TARGET),host)
CFLAGS := -DRT_TRACE $(COMMON) -O1 -g
else ifeq ($(TARGET),count)
CFLAGS := $(COMMON) -g1 -DRT_PROF -DRT_COUNT
else
CFLAGS := $(COMMON) -g1 -DRT_PROF
endif
SRC := $(GEN_SRC) $(RT_SRC) host/main_posix.c host/evb_unpack.c host/dlc_setup.c host/vcdiff.c host/platform_sdl.c host/gpu_gl.c host/pad_input.c host/game_patches.c host/overlay.c host/wide_art.c host/port_art.c
# libstdc++ (libopenmpt is C++) linked in, so the Mac app needs no Homebrew GCC
STDCXX := $(shell $(CC) -print-file-name=libstdc++.a)
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
OMPT_LIB := $(OMPT)/bin/libopenmpt.a
else ifeq ($(TARGET),a64)
DKA := /opt/devkitpro/devkitA64/bin/aarch64-none-elf-
LIBNX := /opt/devkitpro/libnx
CC := $(DKA)gcc
CXX := $(DKA)g++
AR := $(DKA)gcc-ar
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := $(ARCH) -D__SWITCH__ -I$(LIBNX)/include $(COMMON) -g -DRT_PROF
SRC := $(GEN_SRC) $(RT_SRC) host/main_switch.c host/evb_unpack.c host/dlc_setup.c host/vcdiff.c host/platform_switch.c host/pad_input.c host/game_patches.c host/overlay.c host/wide_art.c host/port_art.c host/gpu_dk.c host/saltynx.c
# libopenmpt from its own source list (its makefile assumes a desktop OS); flags as its makefile uses
OMPT_LIB := $(BUILD)/libopenmpt.a
OMPT_SRC := $(shell cat third_party/libopenmpt_sources.txt)
OMPT_OBJ := $(patsubst %,$(BUILD)/ompt/%.o,$(OMPT_SRC))
OMPT_FLAGS := $(ARCH) -D__SWITCH__ -I$(LIBNX)/include -DLIBOPENMPT_BUILD -DMPT_WITH_MINIMP3 -DMPT_WITH_MINIZ \
              -DMPT_WITH_STBVORBIS -DSTB_VORBIS_NO_PULLDATA_API -DSTB_VORBIS_NO_STDIO -I$(OMPT) -I$(OMPT)/common \
              -I$(OMPT)/include -I$(OMPT)/src -O2 -ffunction-sections -fdata-sections -w
else ifeq ($(TARGET),android)
# NDK r27d, mounted from its dmg (~/sixtyfour/toolchain) by ../tools/build_android.sh; SDL2's headers from its source
NDK ?= /tmp/pvz-ndk-r27d/AndroidNDK13750724.app/Contents/NDK
NDKBIN := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin
CC := $(NDKBIN)/aarch64-linux-android29-clang
CXX := $(NDKBIN)/aarch64-linux-android29-clang++
AR := $(NDKBIN)/llvm-ar
SDL2_SRC ?= $(TOOLCHAIN)/SDL2-2.32.10
CFLAGS := -fPIC $(COMMON) -g1
SRC := $(GEN_SRC) $(RT_SRC) host/main_posix.c host/evb_unpack.c host/dlc_setup.c host/vcdiff.c host/android_jni.c host/platform_sdl.c host/gpu_gl.c host/pad_input.c host/game_patches.c host/overlay.c host/wide_art.c host/port_art.c
SDL_CFLAGS := -I$(SDL2_SRC)/include
OMPT_LIB := $(BUILD)/libopenmpt.a
OMPT_SRC := $(shell cat third_party/libopenmpt_sources.txt)
OMPT_OBJ := $(patsubst %,$(BUILD)/ompt/%.o,$(OMPT_SRC))
OMPT_FLAGS := -fPIC -DLIBOPENMPT_BUILD -DMPT_WITH_MINIMP3 -DMPT_WITH_MINIZ \
              -DMPT_WITH_STBVORBIS -DSTB_VORBIS_NO_PULLDATA_API -DSTB_VORBIS_NO_STDIO -I$(OMPT) -I$(OMPT)/common \
              -I$(OMPT)/include -I$(OMPT)/src -O2 -ffunction-sections -fdata-sections -w
else ifeq ($(TARGET),win64)
# Windows x64: llvm-mingw (UCRT) and SDL2's mingw build, both in ~/sixtyfour/toolchain; ../tools/package_windows.sh
MINGW ?= $(TOOLCHAIN)/llvm-mingw-20261104-ucrt-macos-universal
CC := $(MINGW)/bin/x86_64-w64-mingw32-clang
CXX := $(MINGW)/bin/x86_64-w64-mingw32-clang++
AR := $(MINGW)/bin/x86_64-w64-mingw32-ar
WINDRES := $(MINGW)/bin/x86_64-w64-mingw32-windres
SDL2_WIN ?= $(TOOLCHAIN)/SDL2-2.32.10/x86_64-w64-mingw32
CFLAGS := $(COMMON) -g1 -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN
SRC := $(GEN_SRC) $(RT_SRC) host/main_posix.c host/evb_unpack.c host/dlc_setup.c host/vcdiff.c host/platform_sdl.c host/gpu_gl.c host/pad_input.c host/game_patches.c host/overlay.c host/wide_art.c host/port_art.c
SDL_CFLAGS := -I$(SDL2_WIN)/include/SDL2
OMPT_LIB := $(BUILD)/libopenmpt.a
OMPT_SRC := $(shell cat third_party/libopenmpt_sources.txt)
OMPT_OBJ := $(patsubst %,$(BUILD)/ompt/%.o,$(OMPT_SRC))
OMPT_FLAGS := -DLIBOPENMPT_BUILD -DMPT_WITH_MINIMP3 -DMPT_WITH_MINIZ \
              -DMPT_WITH_STBVORBIS -DSTB_VORBIS_NO_PULLDATA_API -DSTB_VORBIS_NO_STDIO -I$(OMPT) -I$(OMPT)/common \
              -I$(OMPT)/include -I$(OMPT)/src -O2 -ffunction-sections -fdata-sections -w
else ifneq ($(filter linux linux-arm64,$(TARGET)),)
# Linux, built in the Docker image of ../tools/linux (Debian 11: glibc 2.31, as Steam's own runtime; GCC 10):
# x86_64 by its cross GCC (desktop OpenGL 3.2, as the Mac), arm64 by its own GCC with OpenGL ES 3.0 (RT_GLES: ARM
# Linux devices mostly have only that), SDL2 2.32.10 built from source beside it (../tools/build_linux.sh)
ifeq ($(TARGET),linux)
XP := x86_64-linux-gnu-
GL_LIBS := -lGL
GL_DEFS :=
else
XP :=
GL_LIBS := -lGLESv2
GL_DEFS := -DRT_GLES
endif
CC := $(XP)gcc
CXX := $(XP)g++
AR := $(XP)ar
SDLP ?= $(BUILD)/sdl/prefix
CFLAGS := $(COMMON) -g1 -D_GNU_SOURCE $(GL_DEFS)
SRC := $(GEN_SRC) $(RT_SRC) host/main_posix.c host/evb_unpack.c host/dlc_setup.c host/vcdiff.c host/platform_sdl.c host/gpu_gl.c host/pad_input.c host/game_patches.c host/overlay.c host/wide_art.c host/port_art.c
SDL_CFLAGS := -I$(SDLP)/include/SDL2 -D_REENTRANT
OMPT_LIB := $(BUILD)/libopenmpt.a
OMPT_SRC := $(shell cat third_party/libopenmpt_sources.txt)
OMPT_OBJ := $(patsubst %,$(BUILD)/ompt/%.o,$(OMPT_SRC))
OMPT_STD := c++17
OMPT_FLAGS := -DLIBOPENMPT_BUILD -DMPT_WITH_MINIMP3 -DMPT_WITH_MINIZ \
              -DMPT_WITH_STBVORBIS -DSTB_VORBIS_NO_PULLDATA_API -DSTB_VORBIS_NO_STDIO -I$(OMPT) -I$(OMPT)/common \
              -I$(OMPT)/include -I$(OMPT)/src -O2 -ffunction-sections -fdata-sections -w
else
CC := clang
CFLAGS := --target=armv8a-none-eabi -mcpu=cortex-a57 -mfloat-abi=softfp -mfpu=neon-fp-armv8 -fPIE \
          -ffreestanding -DRT_FLAT32 $(COMMON)
SRC := $(GEN_SRC)
endif

OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRC)))
vpath %.c gen runtime host

all: $(BUILD)/check.txt

# Huge straight-line functions take clang/GCC -O2 tens of minutes; -O1 is plenty
$(BUILD)/big_%.o: big_%.c runtime/x86rt.h | $(BUILD)
	@echo "CC $< (-O1)"
	@$(CC) $(CFLAGS) -O1 -c $< -o $@

$(BUILD)/platform_sdl.o $(BUILD)/main_posix.o $(BUILD)/gpu_gl.o: CFLAGS += $(SDL_CFLAGS)
$(BUILD)/gpu_gl.o: CFLAGS += -DGL_SILENCE_DEPRECATION
$(BUILD)/rt_vorbis.o: CFLAGS += -w

OMPT_STD ?= c++23
ifneq ($(filter a64 android win64 linux linux-arm64,$(TARGET)),)
$(BUILD)/ompt/%.cpp.o: $(OMPT)/%.cpp
	@mkdir -p $(dir $@)
	@echo "CXX $<"
	@$(CXX) $(OMPT_FLAGS) -std=$(OMPT_STD) -fexceptions -frtti -c $< -o $@
$(BUILD)/ompt/%.c.o: $(OMPT)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(OMPT_FLAGS) -std=c11 -fno-strict-aliasing -c $< -o $@
$(BUILD)/libopenmpt.a: $(OMPT_OBJ)
	@$(AR) rcs $@ $^
endif
$(BUILD)/hle_bass.o: CFLAGS += -I$(OMPT)

$(OMPT)/bin/libopenmpt.a:
	$(MAKE) -C $(OMPT) CONFIG=gcc CC=gcc-15 CXX=g++-15 LD=g++-15 $(OMPT_FLAGS) bin/libopenmpt.a

$(BUILD)/%.o: %.c runtime/x86rt.h runtime/rt.h | $(BUILD)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

# host: a runnable test binary. a64: partial link, then list what the game code still needs from outside
ifeq ($(TARGET),a64)
# GLSL -> DKSH (uam) -> one C file of byte arrays: <name>_dksh, <name>_dksh_size
SHADERS := $(wildcard host/shaders/*.glsl)
$(BUILD)/shaders/%.dksh: host/shaders/%.glsl | $(BUILD)
	@mkdir -p $(dir $@)
	@echo "UAM $<"
	@/opt/devkitpro/tools/bin/uam -s $(if $(findstring _vsh,$*),vert,frag) -o $@ $<
$(BUILD)/shaders.c: $(patsubst host/shaders/%.glsl,$(BUILD)/shaders/%.dksh,$(SHADERS))
	@$(PY) -c "import sys,os; out=open(sys.argv[1],'w'); out.write('#include <stdint.h>\n'); [out.write('const uint8_t %s_dksh[] __attribute__((aligned(256))) = {%s};\nconst uint32_t %s_dksh_size = %d;\n' % (os.path.basename(f)[:-5], ','.join(str(b) for b in open(f,'rb').read()), os.path.basename(f)[:-5], os.path.getsize(f))) for f in sys.argv[2:]]" $@ $^
$(BUILD)/shaders.o: $(BUILD)/shaders.c
	@$(CC) $(ARCH) -c $< -o $@
SHADER_OBJ := $(BUILD)/shaders.o
endif

WIN_RES := $(if $(filter win64,$(TARGET)),host/win/pvzdlc.rc host/win/pvzdlc.ico host/win/pvzdlc.manifest)
WIN_RES += $(if $(filter a64,$(TARGET)),nx/icon.jpg)
$(BUILD)/check.txt: $(OBJ) $(OMPT_LIB) $(SHADER_OBJ) $(WIN_RES)
ifneq ($(filter host play count,$(TARGET)),)
	@$(CC) -o $(BUILD)/pvzdlc_host $(OBJ) $(OMPT_LIB) $(STDCXX) -lpthread $(SDL_LIBS) -framework OpenGL -framework Foundation -lobjc
	@echo "built $(BUILD)/pvzdlc_host" > $@
	@cat $@
else ifeq ($(TARGET),a64)
	@$(CXX) -specs=$(LIBNX)/switch.specs $(ARCH) -g -Wl,-Map,$(BUILD)/pvzdlc.map -Wl,--gc-sections \
	    -o $(BUILD)/pvzdlc.elf $(OBJ) $(BUILD)/shaders.o $(OMPT_LIB) -L$(LIBNX)/lib -ldeko3d -lnx -lm
	@/opt/devkitpro/tools/bin/nacptool --create "Plants vs. Zombies: DLC Edition" "aks796, Samen" "Beta 2.5.1" $(BUILD)/pvzdlc.nacp >/dev/null
	@# the port's pictures (../gamedata/wide: HenryJk's widescreen art, GPL-3, PvZ Touch's bushes and the port's own) in the NRO's romfs
	@mkdir -p $(BUILD)/romfs/wide
	@rsync -a --delete --exclude .DS_Store --exclude '*.off' ../gamedata/wide/ $(BUILD)/romfs/wide/
	@/opt/devkitpro/tools/bin/elf2nro $(BUILD)/pvzdlc.elf $(BUILD)/pvzdlc.nro --icon=nx/icon.jpg \
	    --nacp=$(BUILD)/pvzdlc.nacp --romfsdir=$(BUILD)/romfs >/dev/null
	@echo "built $(BUILD)/pvzdlc.nro" > $@
	@cat $@
else ifeq ($(TARGET),win64)
	@# the icon and the manifest (UTF-8 file names, DPI aware), then one exe with no C++/pthread DLLs beside SDL2.dll
	@$(WINDRES) -I host/win host/win/pvzdlc.rc -O coff -o $(BUILD)/pvzdlc_res.o
	@$(CXX) -o $(BUILD)/pvzdlc.exe -mwindows -static -Wl,--gc-sections $(OBJ) $(BUILD)/pvzdlc_res.o $(OMPT_LIB) \
	    -L$(SDL2_WIN)/lib -lmingw32 -lSDL2main $(SDL2_WIN)/lib/libSDL2.dll.a -lopengl32 -lcomdlg32 -lshell32 -lpthread
	@echo "built $(BUILD)/pvzdlc.exe" > $@
	@cat $@
else ifneq ($(filter linux linux-arm64,$(TARGET)),)
	@# (the C++ runtime linked in; SDL2 beside it in lib/, found through the program's own folder)
	@$(CXX) -o $(BUILD)/pvzdlc -Wl,--gc-sections -Wl,-rpath,'$$ORIGIN/lib' $(OBJ) $(OMPT_LIB) -L$(SDLP)/lib -lSDL2 \
	    $(GL_LIBS) -lpthread -ldl -lm -static-libstdc++ -static-libgcc
	@echo "built $(BUILD)/pvzdlc" > $@
	@cat $@
else ifeq ($(TARGET),android)
	@# (libc++ linked in ahead of SDL2, whose libSDL2.so exports a copy of its own: the game keeps one C++ runtime)
	@$(CXX) -shared -nostdlib++ -o $(BUILD)/libmain.so -Wl,--gc-sections -Wl,-soname,libmain.so $(OBJ) $(OMPT_LIB) \
	    -lc++_static -lc++abi -L$(BUILD)/sdl -lSDL2 -lGLESv3 -lEGL -llog -landroid -Wl,--no-undefined
	@echo "built $(BUILD)/libmain.so" > $@
	@cat $@
else
	@echo "$(words $(OBJ)) objects" > $@
	@echo "compiled $(words $(OBJ)) objects"
endif

# header dependencies written by -MMD (objects built before it existed have none)
-include $(OBJ:.o=.d)

gen:
	cd tools && $(PY) -m x86recomp ../$(GAME_EXE) ../gen

clean:
	rm -rf build

.PHONY: all gen clean
