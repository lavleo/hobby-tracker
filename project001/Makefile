# MediaLog — Makefile
# Requires: Emscripten SDK (emcc)  https://emscripten.org/docs/getting_started/

CC  := emcc
SRC := src/app.c
OUT := web/app.js

# All C functions that JavaScript calls directly must be listed here.
EXPORTS := '["_main","_malloc","_free",\
"_app_init","_app_start","_app_navigate",\
"_app_set_type","_app_detail","_app_delete",\
"_app_search","_app_submit"]'

# Emscripten runtime helpers used in the JS bridge (index.html).
RUNTIME := '["stringToNewUTF8","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'

# ── release build (optimised, small output) ──────────────────────────────────
build:
	$(CC) $(SRC) -o $(OUT) \
		-s WASM=1 \
		-s EXPORTED_FUNCTIONS=$(EXPORTS) \
		-s EXPORTED_RUNTIME_METHODS=$(RUNTIME) \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s ENVIRONMENT='web' \
		-s MODULARIZE=0 \
		-s SINGLE_FILE=0 \
		-O2

# ── dev build (debug info, assertions) ───────────────────────────────────────
dev:
	$(CC) $(SRC) -o $(OUT) \
		-s WASM=1 \
		-s EXPORTED_FUNCTIONS=$(EXPORTS) \
		-s EXPORTED_RUNTIME_METHODS=$(RUNTIME) \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s ENVIRONMENT='web' \
		-s MODULARIZE=0 \
		-s ASSERTIONS=2 \
		-s SAFE_HEAP=1 \
		-g

# ── local dev server (WASM requires HTTP, not file://) ───────────────────────
serve:
	@echo "Open: http://localhost:8080"
	cd web && python3 -m http.server 8080

# ── clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f web/app.js web/app.wasm

.PHONY: build dev serve clean
