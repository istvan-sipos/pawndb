CC     := i686-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -static-libgcc -std=gnu99
LDFLAGS := -shared -Wl,--kill-at

# Version is the single source of truth for what users see in the log /
# CLI banner / release zip name. Bump it before each release; both
# binaries embed the value via -DPAWNDB_VERSION="<v>" at compile time, so
# stale builds don't lie about which release they came from. Matching
# bumps on the zip name + folder name make it obvious from the download
# alone which version a user has.
VERSION := $(shell cat VERSION)

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

# Companion CLI: tools/restore_pawn.exe — splices an archived pawn's .xml
# sidecar into a target DDDA.sav as the main pawn. Win32 exe so it can run
# from the same Wine prefix the game uses; reuses pawnsave.c (region locator)
# and easyzlib (inflate/deflate). Pure ANSI C, no DLL deps.
RESTORE_TARGET := tools/restore_pawn.exe
RESTORE_SRC    := tools/restore_pawn.c

# Binary release archive — what end users download from Nexus etc. Lays
# the files out the way RESTORE_PAWN.md / INSTALL.md describe them, so a
# user can extract and follow the instructions without reading code:
#   pawndb-release/
#     pawndb.dll               → goes in steam_settings/load_dlls/
#     pawndb.ini               → default config, sits next to pawndb.dll
#     tools/restore_pawn.exe   → run from cmd / Wine prefix
#     README.md                → project overview
#     INSTALL.md               → step-by-step gbe_fork + pawndb setup
#     RESTORE_PAWN.md          → restore_pawn usage
#     NEXUS.md                 → Nexus mod page copy
RELEASE_NAME    := pawndb-$(VERSION)
RELEASE_STAGING := build/$(RELEASE_NAME)
RELEASE_ZIP     := build/$(RELEASE_NAME).zip
RELEASE_DOCS    := README.md INSTALL.md RESTORE_PAWN.md NEXUS.md

.PHONY: all clean deploy install restore_pawn release release-test check-release-tag

# Default target builds AND deploys. Running `make` without the deploy step
# was the source of a debugging wild-goose-chase — the game was loading a
# stale DLL while we thought we were testing fresh code. Better to couple them.
all: deploy restore_pawn

$(TARGET): $(SRC) $(XFS_SRC) $(SAVE_SRC) $(ZLIB_SRC) pawnxfs.h pawnsave.h VERSION
	$(CC) $(CFLAGS) -DPAWNDB_VERSION='"$(VERSION)"' $(LDFLAGS) -o $@ $(SRC) $(XFS_SRC) $(SAVE_SRC) $(ZLIB_SRC) -luser32 -lkernel32 -lshell32 -lole32

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

restore_pawn: $(RESTORE_TARGET)

$(RESTORE_TARGET): $(RESTORE_SRC) $(SAVE_SRC) $(ZLIB_SRC) pawnsave.h VERSION
	$(CC) $(CFLAGS) -DPAWNDB_VERSION='"$(VERSION)"' -o $@ $(RESTORE_SRC) $(SAVE_SRC) $(ZLIB_SRC)

# Stage the release tree under build/ then zip with the wrapping folder
# preserved, so `unzip pawndb-X.Y.Z.zip` produces a single tidy directory
# instead of dumping files into the user's CWD.
#
# Two entry points:
#   release-test  - just builds the zip; no git side effects. Use for
#                   smoke-testing the layout / running CI / sanity checks
#                   before the actual release.
#   release       - bails out if v$(VERSION) already exists (so you can't
#                   accidentally re-ship the same version), builds the
#                   zip, then creates a local annotated tag pointing at
#                   the current HEAD. Pushing the tag is left manual:
#                       git push origin v$(VERSION)
release-test: $(RELEASE_ZIP)
	@echo "== release-test: zip built, no git tag created =="

release: check-release-tag $(RELEASE_ZIP)
	git tag -a v$(VERSION) -m "pawndb $(VERSION)"
	@echo
	@echo "== released v$(VERSION) =="
	@echo "  artifact: $(RELEASE_ZIP)"
	@echo "  tag:      v$(VERSION) (local — push with: git push origin v$(VERSION))"

# Fail loudly before any artifacts are built if the tag already exists.
# Catches the "forgot to bump VERSION" case. Hint at the recovery path
# in the error message — deleting the tag is the right move only if the
# previous release was unintentional.
check-release-tag:
	@if git rev-parse -q --verify refs/tags/v$(VERSION) >/dev/null 2>&1; then \
		echo "error: tag v$(VERSION) already exists at $$(git rev-parse --short v$(VERSION)) — bump VERSION before re-running 'make release'"; \
		echo "       (if you really mean to redo the tag: git tag -d v$(VERSION) && git push --delete origin v$(VERSION))"; \
		exit 1; \
	fi

$(RELEASE_ZIP): $(TARGET) $(RESTORE_TARGET) pawndb.ini $(RELEASE_DOCS)
	rm -rf $(RELEASE_STAGING) $(RELEASE_ZIP)
	mkdir -p $(RELEASE_STAGING)/tools
	cp $(TARGET)         $(RELEASE_STAGING)/
	cp pawndb.ini        $(RELEASE_STAGING)/
	cp $(RESTORE_TARGET) $(RELEASE_STAGING)/tools/
	cp $(RELEASE_DOCS)   $(RELEASE_STAGING)/
	cd build && zip -r $(RELEASE_NAME).zip $(RELEASE_NAME)
	@echo "== zip built =="
	@unzip -l $(RELEASE_ZIP)

clean:
	rm -rf $(TARGET) $(RESTORE_TARGET) build
