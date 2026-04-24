CC     := i686-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -static-libgcc -std=gnu99
LDFLAGS := -shared -Wl,--kill-at

TARGET := pawndb.dll

# Core mod source (Steam API hooks, scrubber, archive pipeline).
SRC     := pawndb.c
# XFS patcher: Blowfish + SHA-1 + deflate, for post-archive mArisenName injection
# and gear-writeback patches applied at DDDA.sav write time.
XFS_SRC := pawnxfs.c
# Save-file reader: decompresses DDDA.sav and extracts hired-pawn gear records.
SAVE_SRC := pawnsave.c
# Vendored single-file zlib (easyzlib by First Objective Software); provides
# compress2/uncompress without a system libz dependency.
ZLIB_SRC := third_party/easyzlib/easyzlib.c

# Drop into gbe_fork's load_dlls so it auto-loads on steam_api.dll init
DDDA_DIR := /home/istvan/.steam/steam/steamapps/common/DDDA
LOAD_DIR := $(DDDA_DIR)/steam_settings/load_dlls
DEPLOY   := $(LOAD_DIR)/$(TARGET)

.PHONY: all clean deploy install

# Default target builds AND deploys. Running `make` without the deploy step
# was the source of a debugging wild-goose-chase — the game was loading a
# stale DLL while we thought we were testing fresh code. Better to couple them.
all: deploy

$(TARGET): $(SRC) $(XFS_SRC) $(SAVE_SRC) $(ZLIB_SRC) pawnxfs.h pawnsave.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(XFS_SRC) $(SAVE_SRC) $(ZLIB_SRC) -luser32 -lkernel32 -lshell32 -lole32

# Copy into gbe_fork's load_dlls and verify the hashes match. If the game is
# running, Wine has the DLL mapped and the cp will fail with a clear error —
# close the game first.
deploy: $(TARGET)
	@mkdir -p "$(LOAD_DIR)"
	cp $(TARGET) "$(DEPLOY)"
	@echo "== deploy hash check =="
	@md5sum $(TARGET) "$(DEPLOY)"

# Full install: also seeds pawndb.ini if the user hasn't installed one yet.
install: deploy
	if [ ! -f "$(LOAD_DIR)/pawndb.ini" ]; then cp pawndb.ini "$(LOAD_DIR)/pawndb.ini"; fi
	ls -la "$(LOAD_DIR)/"

clean:
	rm -f $(TARGET)
