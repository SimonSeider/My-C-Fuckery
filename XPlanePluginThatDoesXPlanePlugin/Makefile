PLUGIN := XPlanePluginThatDoesXPlanePlugin
SDK    := SDK/CHeaders/XPLM

OUT    := $(PLUGIN)/64
XPL    := $(OUT)/lin.xpl

SRC    := src/mod.c src/tel.c
OBJ    := $(SRC:src/%.c=build/%.o)

DEF    := -DXPLM420=1 -DXPLM410=1 -DXPLM400=1 -DXPLM303=1 -DXPLM301=1 \
          -DXPLM300=1 -DXPLM210=1 -DXPLM200=1 -DLIN=1

CC     := gcc
CFLAGS := -std=c99 -O2 -fPIC -fvisibility=hidden -Wall -I$(SDK) $(DEF)
LFLAGS := -shared -m64 -fPIC -Wl,-Bsymbolic
LIBS   := -lm

.PHONY: all clean

all: $(XPL)

$(XPL): $(OBJ)
	@mkdir -p $(OUT)
	$(CC) $(LFLAGS) -o $@ $(OBJ) $(LIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(PLUGIN)
