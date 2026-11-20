# BrilliantGd Makefile
# Cross-compiles build/XInput1_4.dll with MinGW-w64 (Linux, or an MSYS2 MinGW shell).
#
#   make                         build the DLL (downloads dependencies on the first run)
#   make install GD_DIR="/path/to/Geometry Dash"
#   make clean                   remove build output
#   make distclean               also remove the downloaded dependencies
#
# Options:
#   make TRIPLE=i686-w64-mingw32     32-bit build (default is 64-bit: x86_64-w64-mingw32)
#   make CXX=g++ CC=gcc              use the compilers on your PATH (e.g. MSYS2)

TRIPLE   ?= x86_64-w64-mingw32
CXX      := $(TRIPLE)-g++
CC       := $(TRIPLE)-gcc

LUAU_TAG ?= 0.650
JSON_VER ?= v3.11.3
IMGUI_TAG ?= v1.91.9

OUT      := build/XInput1_4.dll
OBJ_DIR  := build/obj
STAMP    := deps/.stamp
IMGUI_SRC := deps/imgui/imgui.cpp

.PHONY: all dll check install clean distclean

# Step 1: make sure dependencies exist. Step 2 runs in a fresh make so that the
# wildcards below see the downloaded sources.
all: $(STAMP) $(IMGUI_SRC)
	@$(MAKE) --no-print-directory dll

$(STAMP):
	@command -v git  >/dev/null || { echo "error: git is required";  exit 1; }
	@command -v curl >/dev/null || { echo "error: curl is required"; exit 1; }
	rm -rf deps/luau deps/minhook deps/json
	mkdir -p deps/json/include/nlohmann
	git clone --depth 1 --branch $(LUAU_TAG) https://github.com/luau-lang/luau.git deps/luau
	git clone --depth 1 https://github.com/TsudaKageyu/minhook.git deps/minhook
	curl -fL -o deps/json/include/nlohmann/json.hpp https://github.com/nlohmann/json/releases/download/$(JSON_VER)/json.hpp
	touch $@

$(IMGUI_SRC):
	@command -v git >/dev/null || { echo "error: git is required"; exit 1; }
	rm -rf deps/imgui
	git clone --depth 1 --branch $(IMGUI_TAG) https://github.com/ocornut/imgui.git deps/imgui

# ---------------------------------------------------------------- sources

ifneq ($(findstring x86_64,$(TRIPLE)),)
  HDE := hde64
else
  HDE := hde32
endif

OWN_SRCS  := $(wildcard src/*.cpp)
LUAU_SRCS := $(wildcard deps/luau/Ast/src/*.cpp) \
             $(wildcard deps/luau/Compiler/src/*.cpp) \
             $(wildcard deps/luau/VM/src/*.cpp)
MH_SRCS   := deps/minhook/src/buffer.c deps/minhook/src/hook.c \
             deps/minhook/src/trampoline.c deps/minhook/src/hde/$(HDE).c

IMGUI_SRCS := deps/imgui/imgui.cpp deps/imgui/imgui_draw.cpp deps/imgui/imgui_tables.cpp \
              deps/imgui/imgui_widgets.cpp deps/imgui/backends/imgui_impl_win32.cpp \
              deps/imgui/backends/imgui_impl_opengl3.cpp

OBJS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OWN_SRCS) $(LUAU_SRCS) $(IMGUI_SRCS)) \
        $(patsubst %.c,$(OBJ_DIR)/%.o,$(MH_SRCS))

INCLUDES := -Isrc \
            -Ideps/json/include \
            -Ideps/minhook/include \
            -Ideps/imgui \
            -Ideps/imgui/backends \
            -Ideps/luau/Common/include \
            -Ideps/luau/Ast/include \
            -Ideps/luau/Compiler/include \
            -Ideps/luau/VM/include

COMMON   := -O2 -DNDEBUG -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -MMD -MP
CXXFLAGS := -std=c++20 $(COMMON) -Wall -Wno-cast-function-type $(INCLUDES)
CFLAGS   := $(COMMON) $(INCLUDES)
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -Wl,--kill-at -s
LDLIBS   := -lopengl32 -lgdi32 -limm32 -ldwmapi

# Don't spam warnings from third-party code.
$(OBJ_DIR)/deps/%.o: CXXFLAGS += -w
$(OBJ_DIR)/deps/%.o: CFLAGS   += -w

# ---------------------------------------------------------------- build

dll: check $(OUT)
	@echo "Built $(OUT)"

check:
	@command -v $(CXX) >/dev/null || { \
	  echo "error: $(CXX) not found. Install MinGW-w64 (Debian/Ubuntu: sudo apt install mingw-w64, Arch: mingw-w64-gcc)."; \
	  exit 1; }

$(OUT): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

# ---------------------------------------------------------------- install / clean

install: all
	@test -n "$(GD_DIR)" || { echo 'usage: make install GD_DIR="/path/to/Geometry Dash"'; exit 1; }
	@test -d "$(GD_DIR)" || { echo "error: not a directory: $(GD_DIR)"; exit 1; }
	cp "$(OUT)" "$(GD_DIR)/"
	mkdir -p "$(GD_DIR)/BrilliantGd/mods"
	cp -r example-mod/hello-world "$(GD_DIR)/BrilliantGd/mods/"
	@echo "Installed. On Linux/Proton, set the Steam launch option:"
	@echo '  WINEDLLOVERRIDES="XInput1_4=n,b" %command%'

clean:
	rm -rf build

distclean: clean
	rm -rf deps
