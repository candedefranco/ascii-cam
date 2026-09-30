CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS := -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation

BIN := ascii-cam
SRC := src/main.c src/camera_mac.m

$(BIN): $(SRC) src/camera.h
	$(CC) $(CFLAGS) -fobjc-arc $(SRC) -o $@ $(LDFLAGS) -lm

run: $(BIN)
	./$(BIN)

demo: $(BIN)
	./$(BIN) --demo

clean:
	rm -f $(BIN)

.PHONY: run demo clean
