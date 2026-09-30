CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS := -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation

BIN := ascii-cam
SRC := src/main.c src/render.c src/camera_mac.m

$(BIN): $(SRC) src/camera.h src/render.h
	$(CC) $(CFLAGS) -fobjc-arc $(SRC) -o $@ $(LDFLAGS) -lm

run: $(BIN)
	./$(BIN)

demo: $(BIN)
	./$(BIN) --demo

# Browser build: the same engine compiled to WebAssembly, published from docs/
WEB_FUNCS := _ac_rgba_buffer,_ac_set,_ac_render,_ac_render_demo,_ac_len

web: docs/ascii-cam.js docs/index.html

docs/ascii-cam.js: src/render.c src/render.h web/wasm.c
	mkdir -p docs
	emcc -O3 -std=c11 src/render.c web/wasm.c -o $@ \
		-sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createModule -sENVIRONMENT=web \
		-sALLOW_MEMORY_GROWTH -sEXPORTED_FUNCTIONS=$(WEB_FUNCS) -sEXPORTED_RUNTIME_METHODS=HEAPU8

docs/index.html: web/index.html
	mkdir -p docs
	cp $< $@

clean:
	rm -f $(BIN)

.PHONY: run demo web clean
