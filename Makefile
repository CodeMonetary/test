# cod4-worldflip - d3d9.dll proxy for CoD4 MP
# Cross-compile from Linux with mingw-w64 (i686), or use any x86 MSVC.

CXX      := i686-w64-mingw32-g++
DLLTOOL  := i686-w64-mingw32-dlltool
STRIP    := i686-w64-mingw32-strip

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0501 \
            -static -static-libgcc -static-libstdc++ \
            -fno-exceptions -fno-rtti
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ \
            -Wl,--enable-stdcall-fixup \
            -Wl,--kill-at \
            -Wl,--subsystem,windows

LIBS     := -ld3d9 -lgdi32 -luser32 -lkernel32 -lole32 -lpsapi

SRC := src/dllmain.cpp \
       src/d3d9_wrapper.cpp \
       src/device_wrapper.cpp \
       src/config.cpp \
       src/logger.cpp

OBJ := $(SRC:.cpp=.o)

OUT := dist/d3d9.dll
DEF := src/d3d9.def

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ) $(DEF) | dist
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(DEF) $(LIBS)
	$(STRIP) --strip-unneeded $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

dist:
	mkdir -p dist

clean:
	rm -f $(OBJ) $(OUT)
