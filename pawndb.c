/*
 * pawndb.c - DDDA pawn archive shim for gbe_fork.
 *
 * WHAT IT DOES
 * ------------
 * DDDA's rift ("hire a pawn") feature rides on Steam leaderboards: the game
 * FileShare's an 8 KB pawn blob to get a UGCHandle, then uploads that handle
 * as the 2-int32 details of an entry on leader_231 / leader_232. Searching
 * the rift by level N queries leader_N (18-int32 details = packed metadata).
 * On Capcom's original backend, server-side cross-posting put entries from
 * leader_231 onto the relevant leader_N; gbe_fork doesn't replicate that,
 * which is why the rift shows nothing by default.
 *
 * This shim:
 *   1. Snapshots the user's own pawn blob every time they rest at an inn,
 *      writing <HEX>.pawn + <HEX>.meta + <HEX>.json to <save_dir>/<NNN>/
 *      where <NNN> is the pawn's current level zero-padded to 3 digits and
 *      <HEX> is the 8-hex-digit unix-epoch offset from 2026-01-01 UTC. The
 *      same "NNN:HEX" stem is also injected into the archive's mArisenName
 *      cName slot so the blob self-identifies on subsequent hire (release-
 *      time writeback reads mArisenName from memory to find the source
 *      file). The rest sequence is: FileShare('0'), two cDetails=2 uploads,
 *      then the cDetails=18 cluster. Archive is deferred to the FIRST
 *      cDetails=18 upload so .meta always captures this-rest-accurate
 *      stats (the 2-detail phase fires with stale stats still in flight).
 *   2. The JSON sidecar carries level, vocation, stats, vocation ranks,
 *      inclinations, and equipped skills, read directly from live memory
 *      using offsets ported from ddda-dinput8 (both DLLs load into the
 *      same DDDA.exe process).
 *   3. Archive set is scanned once at worker startup.  Current-session
 *      archives are hidden from rift search anyway (in-game pawn-dedup
 *      blocks self-hire), so mid-session disk changes don't matter.  On
 *      every FindLeaderboard for a rift board (leader_231/232, or a
 *      leader_<N> search board for N=1..200) we rewrite the boards from
 *      the cached g_fakes[] - the rewrite is needed because gbe_fork's
 *      UploadLeaderboardScore during rests wipes our fakes and leaves
 *      just the user's own entry.  leader_<N> gets up to
 *      max_search_results fakes whose level matches N; leader_231/232
 *      gets every fake (no cap) so any surfaced pawn stays summonable.
 *   4. At summon time (game calls UGCDownload(fake_handle)), the shim
 *      substitutes the handle with one gbe_fork knows (g_fresh_ugc),
 *      records which archive blob to serve, and on the subsequent UGCRead
 *      copies bytes from the archive file.  Preview UGC reads are served
 *      from the user's own remote/1.
 *   5. Fixes gbe_fork's broken DLEForUsers filtering: gbe_fork returns
 *      the top of the board instead of the requested user's entry. Our
 *      GDLE hook rewrites the returned entry based on the last DLEForUsers
 *      steamid.
 *   6. Acquires a valid g_fresh_ugc at startup by calling FileShare on an
 *      anchor file and using ISteamUtils::GetAPICallResult to harvest the
 *      resulting UGCHandle synchronously, so summoning works even without
 *      the user first resting at an inn this session.
 *
 * DESIGN TRADE-OFFS
 *   - Archive set is scanned once at worker startup; each FindLeaderboard
 *     rewrites the rift boards from the cached g_fakes[] without touching
 *     disk (mandatory because gbe_fork's own writes during rest wipe us).
 *   - Per-level folders filter search results by level. The folder name
 *     must be zero-padded "NNN"; anything else is ignored.
 *   - max_search_results caps per-level result count (default 100);
 *     oldest archives beyond the cap stay on disk but don't surface.
 *   - leader_231/232 are NOT capped - every archive stays summonable
 *     once it has been seen in any search.
 *   - Archives written in the current session are hidden from leader_<N>
 *     search boards until next session (DDDA's in-memory pawn-dedup
 *     refuses to summon a clone of the Arisen's just-uploaded main pawn).
 *
 * KEY FILES AND DIRECTORIES
 *   <dll_dir>/pawndb.ini        - config (save_dir, max_search_results, logging)
 *   <dll_dir>/<save_dir>/<NNN>/<HEX>.pawn - archives keyed by level
 *   <gbe_fork remote_storage>/     - '0', '1' are current session's blobs
 *   <gbe_fork leaderboard>/        - leader_<N> files written by our shim
 *
 * HOOKS (all via direct vtable overwrite - no function-entry patching):
 *   ISteamUserStats:
 *     [22] FindOrCreateLeaderboard   - populate leader_N with injected fakes
 *     [23] FindLeaderboard           - same
 *     [28] DownloadLeaderboardEntries        - observability
 *     [29] DownloadLeaderboardEntriesForUsers - record requested steamid
 *     [30] GetDownloadedLeaderboardEntry      - fix the DLEForUsers filter bug
 *     [31] UploadLeaderboardScore             - capture g_fresh_ugc (2-detail);
 *                                               archive pawn + write fresh .meta
 *                                               on first 18-detail of a pending rest
 *   ISteamRemoteStorage:
 *     [ 4] FileShare                 - observability, reset per-rest guard
 *     [21] UGCDownload               - substitute fake handle -> fresh handle
 *     [24] UGCRead                   - serve archive blob (or remote/1 for preview)
 *
 * CALLING CONVENTION: Windows 32-bit __thiscall for virtual methods (this in
 * ECX, other args right-to-left on stack, callee cleans). MinGW-w64's
 * __thiscall ABI matches gbe_fork's MSVC build on i686.
 */

#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "pawnxfs.h"
#include "pawnsave.h"

typedef uint64_t SteamAPICall_t;
typedef uint64_t UGCHandle_t;
typedef uint64_t SteamLeaderboard_t;
typedef uint64_t SteamLeaderboardEntries_t;
typedef uint64_t CSteamID;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int EUGCReadAction;
typedef int ELeaderboardDataRequest;

#pragma pack(push, 8)
typedef struct LeaderboardEntry_t {
    CSteamID m_steamIDUser;
    int32    m_nGlobalRank;
    int32    m_nScore;
    int32    m_cDetails;
    UGCHandle_t m_hUGC;
} LeaderboardEntry_t;
#pragma pack(pop)

/* ============================================================================
 * Logging
 * ============================================================================ */

static FILE *g_log = NULL;
static CRITICAL_SECTION g_log_lock;

static void log_line(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

/* Dump an int32 array as hex + 4-char ASCII per element. Used for inspecting the
 * 2-int and 18-int details arrays we see in the leaderboard APIs. */
static void log_details(const char *tag, const int32_t *details, int count)
{
    if (!g_log || !details || count <= 0) return;
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "  %s details[%d]:", tag, count);
    for (int i = 0; i < count; i++) {
        fprintf(g_log, " 0x%08x", (unsigned)details[i]);
    }
    fprintf(g_log, " | ascii:");
    for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)details[i];
        char c0 = (char)(v & 0xff), c1 = (char)((v >> 8) & 0xff);
        char c2 = (char)((v >> 16) & 0xff), c3 = (char)((v >> 24) & 0xff);
        fprintf(g_log, " '%c%c%c%c'",
                (c0 >= 32 && c0 < 127) ? c0 : '.',
                (c1 >= 32 && c1 < 127) ? c1 : '.',
                (c2 >= 32 && c2 < 127) ? c2 : '.',
                (c3 >= 32 && c3 < 127) ? c3 : '.');
    }
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

/* --- Vtable indices (from gbe_fork SDK headers) --- */

#define IUSERSTATS_FindOrCreateLeaderboard             22
#define IUSERSTATS_FindLeaderboard                     23
#define IUSERSTATS_DownloadLeaderboardEntries          28
#define IUSERSTATS_DownloadLeaderboardEntriesForUsers  29
#define IUSERSTATS_GetDownloadedLeaderboardEntry       30
#define IUSERSTATS_UploadLeaderboardScore              31

#define IREMOTESTORAGE_FileWrite         0
#define IREMOTESTORAGE_FileShare         4
#define IREMOTESTORAGE_UGCDownload       21
#define IREMOTESTORAGE_UGCRead           24

#define IUSER_BLoggedOn                  1    /* ISteamUser::BLoggedOn, polled ~60x/sec — used as a per-tick tick-point for the fake-SteamID scrubber */

/* --- Function pointer types for the original methods ---
 * Windows 32-bit uses __thiscall for C++ virtual methods: `this` in ECX,
 * remaining args right-to-left on the stack, callee cleans.
 */

typedef bool           (__thiscall *fnFileWrite)(void *self, const char *pchFile,
                                                  const void *pvData, int32 cubData);
typedef SteamAPICall_t (__thiscall *fnFileShare)(void *self, const char *pchFile);
typedef SteamAPICall_t (__thiscall *fnUGCDownload)(void *self, UGCHandle_t h, uint32 prio);
typedef int32          (__thiscall *fnUGCRead)(void *self, UGCHandle_t h, void *pvData,
                                               int32 cub, uint32 cOffset, EUGCReadAction act);
typedef int            (__thiscall *fnGetDownloadedLeaderboardEntry)(
    void *self, SteamLeaderboardEntries_t entries, int index,
    LeaderboardEntry_t *pEntry, int32 *pDetails, int cDetailsMax);
typedef SteamAPICall_t (__thiscall *fnDownloadLeaderboardEntries)(
    void *self, SteamLeaderboard_t board, ELeaderboardDataRequest req,
    int rangeStart, int rangeEnd);
typedef SteamAPICall_t (__thiscall *fnDownloadLeaderboardEntriesForUsers)(
    void *self, SteamLeaderboard_t board, CSteamID *users, int cUsers);
typedef SteamAPICall_t (__thiscall *fnUploadLeaderboardScore)(
    void *self, SteamLeaderboard_t board, int method, int32 score,
    const int32 *details, int cDetails);
typedef SteamAPICall_t (__thiscall *fnFindLeaderboard)(void *self, const char *name);
typedef SteamAPICall_t (__thiscall *fnFindOrCreateLeaderboard)(
    void *self, const char *name, int sort_method, int display_type);
typedef int            (__thiscall *fnBLoggedOn)(void *self);

static fnFileWrite                              g_orig_FileWrite = NULL;
static fnFileShare                              g_orig_FileShare = NULL;
static fnUGCDownload                            g_orig_UGCDownload = NULL;
static fnUGCRead                                g_orig_UGCRead = NULL;
static fnGetDownloadedLeaderboardEntry          g_orig_GetDownloadedLeaderboardEntry = NULL;
static fnDownloadLeaderboardEntries             g_orig_DownloadLeaderboardEntries = NULL;
static fnDownloadLeaderboardEntriesForUsers     g_orig_DownloadLeaderboardEntriesForUsers = NULL;
static fnUploadLeaderboardScore                 g_orig_UploadLeaderboardScore = NULL;
static fnFindLeaderboard                        g_orig_FindLeaderboard = NULL;
static fnFindOrCreateLeaderboard                g_orig_FindOrCreateLeaderboard = NULL;
static fnBLoggedOn                              g_orig_BLoggedOn = NULL;

/* Captured state */
static UGCHandle_t g_fresh_ugc = 0;     /* a UGCHandle that gbe_fork's shared_files recognizes this session; used as the substitution target when a fake UGC is downloaded. Acquired either via our startup FileShare or from the first inn-rest upload, whichever happens first. */
static CSteamID    g_captured_self = 0;
static void       *g_steam_utils = NULL;        /* ISteamUtils pointer, for GetAPICallResult */
static void       *g_steam_remotestorage = NULL;/* ISteamRemoteStorage pointer, for FileShare from worker */

/* ---- Fake pawn registry ----
 * One entry per .pawn file found under <save_dir>/<NNN>/. Populated once
 * per session by rescan_all_levels (at worker startup, or lazily on first
 * FindLeaderboard if the startup scan was skipped for lack of a self steamID).
 * Current-session archives are included here but filtered out of leader_<N>
 * writes by write_search_board; they surface next session. */
#define MAX_FAKES 2048

struct FakePawn {
    CSteamID     steamid;
    UGCHandle_t  ugc_main;       /* distinct handle per fake; we watch for it in UGCDownload */
    UGCHandle_t  ugc_preview;    /* preview reads are redirected to user's remote/1 */
    char         pawn_path[MAX_PATH];    /* on-disk path of the .pawn blob (8KB) */
    char         meta_path[MAX_PATH];    /* sibling .meta with the 18-int card snapshot */
    int32        details[18];            /* loaded from .meta when has_details=1 */
    int          has_details;            /* 0 = .meta missing; fall back to g_last_18_details */
    int          level;                  /* parsed from containing folder name (zero-padded "NNN") */
    uint64_t     mtime;                  /* FILETIME of the .pawn file; for "most recent first" sort */
};

static struct FakePawn g_fakes[MAX_FAKES];
static int              g_fake_count = 0;
static CRITICAL_SECTION g_fakes_lock;

/* Pending UGC read state. DDDA's summon flow serializes UGCDownload->UGCRead per pawn,
 * so a single global is enough. -1 = nothing pending (pass reads through to gbe_fork). */
static int g_pending_fake_idx     = -1;
static int g_pending_is_preview   = 0;

/* Forward-declared because hook_UGCDownload (in the hooks section, right below)
 * calls it but the registry helpers live further down the file. */
static int find_fake_by_ugc(UGCHandle_t h, int *is_preview_out);

/* DLEForUsers filter fix: gbe_fork's DLEForUsers doesn't actually filter to the
 * requested user - it returns the board-sorted top entry. We capture what the game
 * asked for so GDLE can rewrite the entry to match. Single slot is enough since
 * DDDA's flow is serialized. */
static CSteamID g_last_dleforusers_steamid = 0;
static int         g_rest_pending = 0;          /* rest-in-progress flag. Set on FileShare('0'), cleared when we
                                                   archive on the first cDetails==18 upload of the cycle.
                                                   Rest sequence (verified consistent across inn rests):
                                                     FileShare('0') -> cDetails=2 (leader_231) -> FileShare('1')
                                                       -> cDetails=2 (leader_232) -> cDetails=18 x ~4-6
                                                   We defer the whole archive (remote/0 copy + .meta) to the first
                                                   18-detail upload so .meta always captures this-rest stats, never
                                                   stale data. If cDetails=18 never fires, the rest produces no
                                                   archive by design - better missing than wrong. */

/* Cached 18-int32 pawn-metadata template.  Seeded at worker startup from disk leader_227
 * and refreshed whenever the game emits an 18-detail upload.  Used only as a fallback
 * in write_search_board for archives that lack a .meta sidecar (older archives, or
 * archives from hypothetical sessions where no 18-detail upload fired). New archives
 * always carry their own fresh .meta so this fallback rarely kicks in. */
static int32 g_last_18_details[18]           = {0};
static int   g_has_18_details                = 0;

/* ---- Fake-SteamID scrubber (wide-scan) ----
 * When a mod-served pawn is hired, DDDA copies the leaderboard entry's owner
 * SteamID (a mod-synthetic high-bit-set value) into the hired pawn's
 * mOnlinePawnInfo.mNetUniqueId.mData.  That value then cascades into
 * mNetRewardStock, which on the next rest short-circuits the pawn-upload path
 * (no FileShare('1'), no cDetails=18 cluster, no .pawn archive).  Zeroing the
 * fake in mCmc before release prevents the downstream cascade — mPawnHistory,
 * mNetRewardStock, and save-time secondary-copy sites are all populated FROM
 * mCmc[hired], so clean primary → clean everywhere else.
 *
 * Address discovery: empirically (see diag-scan logs) the hired pawn 1
 * MtNetUniqueId.mData sits at *pBase + 0x0AAAB0, and hired pawn 2 at
 * +0x0AC110 (stride 0x1660).  We do NOT hardcode these — instead, every
 * BLoggedOn tick we walk a 1 MB window starting at *pBase and zero every
 * fake-match. Naturally handles N hired pawns (the stride-2 second slot is
 * just another hit in the same scan). Robust against layout drift across
 * builds; cost ~0.15 ms/tick = ~1% CPU at 60 Hz, well within budget.
 *
 * mData layout when populated with a Steam ID:
 *   [0..3]   = 0x00000002 LE    -- MtNetUniqueId type tag (Steam)
 *   [4..7]   = account_id LE    -- byte 7 MSB set distinguishes fake from real
 *   [8..11]  = 0x01100001 LE    -- Steam universe/type/instance
 *   [12..63] = 0x00              -- unused for Steam IDs
 * The u32 four bytes before mData is mDataLength (64 when populated; we zero too).
 *
 * Real pawn netuids (user's own main pawn at *pBase + 0x0A9450) have byte[7]
 * MSB clear and are not touched. Only mod-synthetic IDs carry byte[7] MSB set. */
#define NETUID_WIDE_SCAN_SIZE  0x00100000u   /* 1 MB — covers full char block */

/* Log policy: first sighting of each unique fake address gets one log line;
 * repeats at the same address stay silent. Bounded set; if we exceed the cap
 * we just skip logging (still zero the fake). Reset when *pBase moves. */
#define NETUID_SEEN_MAX 8
static BYTE *g_netuid_seen[NETUID_SEEN_MAX] = {0};
static int   g_netuid_seen_count            = 0;
static BYTE *g_netuid_seen_base             = NULL;

/* Configuration loaded from pawndb.ini (beside the DLL). */
static char g_dll_dir[MAX_PATH]       = {0};   /* resolved at DllMain, includes trailing backslash */
static char g_save_dir_rel[128]       = "pawn_database";     /* root; <NNN>/ subdirs underneath */
static int  g_max_search_results      = 100;                  /* cap per leader_<N> search board */
static int  g_enable_exports          = 1;                    /* 0 = don't archive the player's main pawn at rest */
static int  g_enable_updates          = 1;                    /* 0 = don't writeback hired-pawn gear/knowledge at save */

enum log_mode { LOG_DISABLED = 0, LOG_TRUNCATE = 1, LOG_APPEND = 2 };
static enum log_mode g_log_mode = LOG_TRUNCATE;                 /* pawndb.log handling */

/* Session-start wall-clock time, captured in FILETIME format (100ns ticks since 1601
 * UTC) at DllMain.  Archives written during this session have fp->mtime >= this and
 * are hidden from leader_<N> search boards - the game's in-memory state post-rest
 * refuses to summon a just-archived pawn because the embedded pawn ID matches the
 * Arisen's own main pawn still cached in memory.  Cross-session summon works (next
 * launch the cache is rebuilt and the match no longer triggers), so we still list
 * these archives in leader_231/232. */
static uint64_t g_session_start_ft = 0;

/* gbe_fork stores per-app save data under %APPDATA%\GSE Saves\367500\{remote,leaderboard}.
 * Resolved at DllMain via SHGetFolderPathA(CSIDL_APPDATA) so pawndb runs anywhere the
 * game does (Windows-native and Proton/Wine both: Wine maps CSIDL_APPDATA to
 * <prefix>\drive_c\users\steamuser\AppData\Roaming, which is the same bytes the old
 * hardcoded Z:\home\... path resolved to). */
static char g_remote_dir[MAX_PATH] = {0};
static char g_leader_dir[MAX_PATH] = {0};

/* ============================================================================
 * Hook implementations (ISteamRemoteStorage - data plane)
 * ============================================================================ */

/* XFS gear-record layout, validated end-to-end by tools/save_to_archive.py:
 *   slot k record base = 0x2098 + k * 0x46
 *   mItemNo (s16) at base + 2
 *   mFlag   (u32) at base + 8, but with the 0x80 "equipped" bit cleared
 *     (the game re-applies that bit when the blob is next loaded). */
#define WB_GEAR_BASE       0x2098u
#define WB_GEAR_STRIDE     0x46u
#define WB_GEAR_COUNT      12
#define WB_ITEM_OFF        0x02u
#define WB_FLAG_OFF        0x08u
#define WB_EQUIPPED_BIT    0x80u

/* mStudyFlag / mLocalStudyFlag layout, validated by patch-and-rehire tests:
 *   each lives inside a `0x142`-tagged 1292-byte struct (4-byte tag + array).
 *   array bytes start at the offsets below; size is 322 * sizeof(u32) = 1288.
 *   Save XML and XFS bytes match bit-for-bit, so a verbatim copy works.
 *   DO NOT touch the 4-byte tag at 0x2828 / 0x2D34 — that crashes summoning. */
#define WB_STUDY_FLAG_OFF        0x282Cu
#define WB_LOCAL_STUDY_FLAG_OFF  0x2D38u
#define WB_STUDY_COUNT           322

/* Parse "NNN:HHHHHHHH" -> (level, stem). Returns 1 on success, 0 on any
 * format error. Used to decode a hired pawn's mArisenName back into the
 * archive coordinates that archive_rest stamped into it. */
static int parse_arisen_stem(const char *s, int *out_level, char *out_stem, size_t out_stem_cap)
{
    if (!s || !out_level || !out_stem || out_stem_cap < 9) return 0;
    /* Expect exactly: 3 digits, ':', 8 hex digits, then NUL or beyond-length byte. */
    for (int i = 0; i < 3; i++) if (s[i] < '0' || s[i] > '9') return 0;
    if (s[3] != ':') return 0;
    for (int i = 0; i < 8; i++) {
        char c = s[4 + i];
        int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return 0;
    }
    int level = (s[0]-'0')*100 + (s[1]-'0')*10 + (s[2]-'0');
    if (level < 1 || level > 200) return 0;
    *out_level = level;
    memcpy(out_stem, s + 4, 8);
    out_stem[8] = 0;
    return 1;
}

/* Patch the source archive with the 12 equipment records and the two
 * 322-entry knowledge arrays (mStudyFlag, mLocalStudyFlag) pulled from the
 * save XML. The source is identified by mArisenName (which archive_rest
 * stamped with "NNN:HEX" when the archive was first written), so no hire-
 * binding state is required — the save itself is authoritative. */
static void writeback_to_archive(int slot, const struct pawnsave_hired_info *info)
{
    if (!info->present) {
        log_line("writeback: slot %d — no matching cSAVE_DATA_CMC block, skipping", slot);
        return;
    }

    int level = 0;
    char stem[16] = {0};
    if (!parse_arisen_stem(info->creator_name, &level, stem, sizeof(stem))) {
        log_line("writeback: slot %d — mArisenName='%s' not a pawndb stem, skipping",
                 slot, info->creator_name);
        return;
    }

    char pawn_path[MAX_PATH];
    _snprintf(pawn_path, sizeof(pawn_path), "%s%s\\%03d\\%s.pawn",
              g_dll_dir, g_save_dir_rel, level, stem);
    for (char *q = pawn_path; *q; q++) if (*q == '/') *q = '\\';

    if (GetFileAttributesA(pawn_path) == INVALID_FILE_ATTRIBUTES) {
        log_line("writeback: slot %d (%03d:%s) archive not found — '%s'",
                 slot, level, stem, pawn_path);
        return;
    }

    /* Build the patch list: 2 per gear slot (mItemNo, mFlag) + 1 per study
     * array (whole-buffer memcpy). Backing bytes live on this function's
     * stack / inside `info` for the duration of pawnxfs_poke. */
    struct pawnxfs_patch patches[WB_GEAR_COUNT * 2 + 2];
    uint8_t               item_bytes[WB_GEAR_COUNT][2];
    uint8_t               flag_bytes[WB_GEAR_COUNT][4];
    int npatches = 0;
    for (int k = 0; k < WB_GEAR_COUNT; k++) {
        uint32_t base = WB_GEAR_BASE + (uint32_t)k * WB_GEAR_STRIDE;
        int16_t  item = info->gear[k].mItemNo;
        uint32_t flag = info->gear[k].mFlag & ~(uint32_t)WB_EQUIPPED_BIT;

        item_bytes[k][0] = (uint8_t)(item & 0xFF);
        item_bytes[k][1] = (uint8_t)((item >> 8) & 0xFF);
        flag_bytes[k][0] = (uint8_t)(flag & 0xFF);
        flag_bytes[k][1] = (uint8_t)((flag >> 8) & 0xFF);
        flag_bytes[k][2] = (uint8_t)((flag >> 16) & 0xFF);
        flag_bytes[k][3] = (uint8_t)((flag >> 24) & 0xFF);

        patches[npatches++] = (struct pawnxfs_patch){
            .offset = base + WB_ITEM_OFF, .bytes = item_bytes[k], .len = 2,
        };
        patches[npatches++] = (struct pawnxfs_patch){
            .offset = base + WB_FLAG_OFF, .bytes = flag_bytes[k], .len = 4,
        };
    }

    /* Knowledge arrays: u32 LE on disk == host-byte u32 on x86, so we point
     * straight at info->study_flag without any byte-shuffle. */
    patches[npatches++] = (struct pawnxfs_patch){
        .offset = WB_STUDY_FLAG_OFF,
        .bytes  = (const uint8_t *)info->study_flag,
        .len    = WB_STUDY_COUNT * sizeof(uint32_t),
    };
    patches[npatches++] = (struct pawnxfs_patch){
        .offset = WB_LOCAL_STUDY_FLAG_OFF,
        .bytes  = (const uint8_t *)info->local_study_flag,
        .len    = WB_STUDY_COUNT * sizeof(uint32_t),
    };

    char err[128] = {0};
    int prc = pawnxfs_poke(pawn_path, pawn_path, patches, npatches, err, sizeof(err));
    if (prc != 0) {
        log_line("writeback: slot %d (%03d:%s) pawnxfs_poke rc=%d (%s) — '%s'",
                 slot, level, stem, prc, err, pawn_path);
        return;
    }
    int equipped = 0;
    for (int k = 0; k < WB_GEAR_COUNT; k++) if (info->gear[k].mItemNo != -1) equipped++;
    int known = 0;
    for (int i = 0; i < WB_STUDY_COUNT; i++) if (info->study_flag[i] || info->local_study_flag[i]) known++;
    log_line("writeback: slot %d (%03d:%s) gear=%d study=%d -> '%s'",
             slot, level, stem, equipped, known, pawn_path);
}

static bool __thiscall hook_FileWrite(void *self, const char *pchFile,
                                      const void *pvData, int32 cubData)
{
    /* Call the original first so the save is on disk regardless of what we
     * do afterwards. If our writeback crashes, the game's save is safe. */
    bool r = g_orig_FileWrite(self, pchFile, pvData, cubData);

    if (pchFile && strcmp(pchFile, "DDDA.sav") == 0 && pvData && cubData > 32) {
        log_line("FileWrite('DDDA.sav', %d bytes) -> %s",
                 (int)cubData, r ? "true" : "false");
        if (!g_enable_updates) {
            log_line("writeback: enable_updates=0, skipping hired-pawn writeback");
        } else {
            struct pawnsave_hired_info info[2];
            int rc = pawnsave_read_hired(pvData, cubData, info);
            if (rc != 0) {
                log_line("writeback: pawnsave_read_hired rc=%d — skipping both slots", rc);
            } else {
                for (int s = 0; s < 2; s++) writeback_to_archive(s, &info[s]);
            }
        }
    }
    return r;
}

static SteamAPICall_t __thiscall hook_FileShare(void *self, const char *pchFile)
{
    log_line("FileShare('%s')", pchFile ? pchFile : "(null)");
    /* FileShare('0') marks the start of an inn-rest upload cycle. Arm the pending flag
     * so the first cDetails=18 upload (which arrives ~1s later with fresh pawn stats)
     * triggers the archive write. */
    if (pchFile && strcmp(pchFile, "0") == 0) {
        g_rest_pending = 1;
    }
    SteamAPICall_t r = g_orig_FileShare(self, pchFile);
    log_line("FileShare('%s') -> SteamAPICall_t=%llu",
             pchFile ? pchFile : "(null)", (unsigned long long)r);
    return r;
}

static SteamAPICall_t __thiscall hook_UGCDownload(void *self, UGCHandle_t h, uint32 prio)
{
    /* If the game is downloading one of our fake UGC handles, route that request:
     *  - remember which archive blob to serve on the imminent UGCRead;
     *  - substitute the handle with this session's fresh UGC so gbe_fork's callback
     *    machinery resolves (its shared_files has fresh_ugc mapped to remote/0). */
    int is_preview = 0;
    int idx = find_fake_by_ugc(h, &is_preview);
    if (idx >= 0 && g_fresh_ugc != 0) {
        g_pending_fake_idx   = idx;
        g_pending_is_preview = is_preview;
        log_line("UGCDownload: fake #%d (%s) 0x%016llx -> substitute with fresh 0x%016llx",
                 idx, is_preview ? "preview" : "main",
                 (unsigned long long)h, (unsigned long long)g_fresh_ugc);
        h = g_fresh_ugc;
    } else {
        log_line("UGCDownload(handle=0x%016llx, prio=%u)",
                 (unsigned long long)h, (unsigned)prio);
    }
    return g_orig_UGCDownload(self, h, prio);
}

static int32 __thiscall hook_UGCRead(void *self, UGCHandle_t h, void *pvData,
                                     int32 cub, uint32 cOffset, EUGCReadAction act)
{
    /* If a fake UGCDownload was just issued, serve the appropriate blob from disk:
     *   - main:    the fake's archived .pawn file
     *   - preview: the user's current remote/1 (every fake shares the same preview)
     * One-shot - cleared after serving. */
    if (g_pending_fake_idx >= 0 && g_pending_fake_idx < g_fake_count) {
        const struct FakePawn *fp = &g_fakes[g_pending_fake_idx];
        char preview_path[MAX_PATH];
        _snprintf(preview_path, sizeof(preview_path), "%s\\1", g_remote_dir);
        const char *src = g_pending_is_preview ? preview_path : fp->pawn_path;
        FILE *f = fopen(src, "rb");
        if (!f) {
            log_line("UGCRead: pending fake #%d, but fopen('%s') failed (errno=%d); falling through",
                     g_pending_fake_idx, src, errno);
            g_pending_fake_idx = -1;
        } else {
            fseek(f, (long)cOffset, SEEK_SET);
            size_t nread = fread(pvData, 1, (size_t)cub, f);
            fclose(f);
            log_line("UGCRead: served fake #%d (%s) from '%s' - %zu bytes",
                     g_pending_fake_idx, g_pending_is_preview ? "preview" : "main",
                     src, nread);
            g_pending_fake_idx = -1;
            return (int32)nread;
        }
    }
    int32 r = g_orig_UGCRead(self, h, pvData, cub, cOffset, act);
    log_line("UGCRead(handle=0x%016llx, cub=%d, offset=%u, act=%d) -> %d",
             (unsigned long long)h, cub, (unsigned)cOffset, act, r);
    return r;
}

/* LEADERBOARD_DIR and REMOTE_DIR were compile-time string macros keyed to
 * my Proton prefix. They are now resolved at DllMain into g_leader_dir /
 * g_remote_dir. See resolve_gse_paths() below. */

/* ============================================================================
 * Configuration (pawndb.ini) and archive write path
 * ============================================================================ */

/* Strip whitespace + inline comments from a line read by fgets. */
static char *trim(char *s)
{
    if (!s) return s;
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    /* Cut at '#' or ';' comment (but not inside a value) */
    for (end = s; *end; end++) {
        if (*end == '#' || *end == ';') { *end = 0; break; }
    }
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = 0;
    }
    return s;
}

/* Very small key=value parser. Sections are ignored (flat namespace). */
static void load_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("load_ini: '%s' not found, using defaults", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!*key) continue;

        if (strcmp(key, "save_dir") == 0) {
            strncpy(g_save_dir_rel, val, sizeof(g_save_dir_rel) - 1);
            g_save_dir_rel[sizeof(g_save_dir_rel) - 1] = 0;
        } else if (strcmp(key, "max_search_results") == 0) {
            int n = atoi(val);
            if (n > 0 && n <= MAX_FAKES) g_max_search_results = n;
            else log_line("load_ini: max_search_results=%d out of range (1..%d), keeping %d",
                          n, MAX_FAKES, g_max_search_results);
        } else if (strcmp(key, "logging") == 0) {
            if      (strcmp(val, "disabled") == 0) g_log_mode = LOG_DISABLED;
            else if (strcmp(val, "truncate") == 0) g_log_mode = LOG_TRUNCATE;
            else if (strcmp(val, "append")   == 0) g_log_mode = LOG_APPEND;
            else log_line("load_ini: logging='%s' not in {disabled,truncate,append}, keeping default", val);
        } else if (strcmp(key, "enable_exports") == 0) {
            g_enable_exports = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "enable_updates") == 0) {
            g_enable_updates = atoi(val) ? 1 : 0;
        } else {
            log_line("load_ini: unknown key '%s'", key);
        }
    }
    fclose(f);
    /* Summary is logged by DllMain after opening the log file - log_line here
     * is silent because load_ini runs before g_log is set. */
}

/* Resolve g_remote_dir / g_leader_dir from %APPDATA%\GSE Saves\367500\{remote,leaderboard}.
 * SHGetFolderPathA returns the Windows-style Roaming AppData path on both native
 * Windows (C:\Users\<you>\AppData\Roaming) and Proton/Wine (C:\users\steamuser\
 * AppData\Roaming inside the prefix). Returns 1 on success, 0 on failure — in
 * which case the globals stay empty and every gbe_fork-path fopen will miss. */
static int resolve_gse_paths(void)
{
    char appdata[MAX_PATH];
    HRESULT hr = SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata);
    if (hr != S_OK) {
        log_line("resolve_gse_paths: SHGetFolderPathA failed hr=0x%08lx", (unsigned long)hr);
        return 0;
    }
    _snprintf(g_remote_dir, sizeof(g_remote_dir),
              "%s\\GSE Saves\\367500\\remote", appdata);
    _snprintf(g_leader_dir, sizeof(g_leader_dir),
              "%s\\GSE Saves\\367500\\leaderboard", appdata);
    return 1;
}

/* Copy a file by reading all of it into memory then writing out. Returns bytes copied, or -1 on error. */
static long copy_file(const char *src, const char *dst)
{
    FILE *fi = fopen(src, "rb");
    if (!fi) return -1;
    FILE *fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); return -1; }
    long total = 0;
    char buf[8192];
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fi);
        if (n == 0) break;
        if (fwrite(buf, 1, n, fo) != n) { fclose(fi); fclose(fo); return -1; }
        total += (long)n;
    }
    fclose(fi);
    fclose(fo);
    return total;
}

/* Write the .meta sidecar (18 int32) at <stem>.meta using the supplied fresh details.
 * Returns 1 on success, 0 on write failure. */
static int write_meta(const char *pawn_path, const int32 *details)
{
    char dst[MAX_PATH];
    _snprintf(dst, sizeof(dst), "%s", pawn_path);
    /* Replace trailing ".pawn" with ".meta". */
    char *ext = strrchr(dst, '.');
    if (!ext) return 0;
    strcpy(ext, ".meta");

    FILE *f = fopen(dst, "wb");
    if (!f) {
        log_line("write_meta: fopen '%s' failed, errno=%d", dst, errno);
        return 0;
    }
    size_t w = fwrite(details, sizeof(int32), 18, f);
    fclose(f);
    if (w != 18) {
        log_line("write_meta: short write to '%s' (%zu of 18 ints)", dst, w);
        return 0;
    }
    log_line("write_meta: wrote '%s'", dst);
    return 1;
}

/* ============================================================================
 * Pawn-memory reader + JSON exporter
 * ----------------------------------------------------------------------------
 * Offsets and the base-pointer signature are ported from ddda-dinput8
 * (PlayerStats.cpp, Cheats.cpp, dinput8.cpp).  ddda-dinput8 loads into the
 * same DDDA.exe process we're in, so we can read the exact same memory.
 *
 * The game exposes four "characters" at strided offsets from a single base:
 *   Arisen       = 0x00000 from the character base
 *   Main Pawn    = 0x007F0
 *   Pawn 1       = 0x01E50  (0x7F0 + 0x1660)
 *   Pawn 2       = 0x034B0  (0x7F0 + 0x1660 * 2)
 * All our archives are the main pawn, so pawn_offset is hardcoded 0x7F0.
 *
 * Base-pointer discovery: signature-scan DDDA.exe's .text section for the
 * byte pattern the game uses when loading the character base into EDX;
 * the 4-byte immediate after the opcode is the address of the global
 * variable that holds the actual base pointer (populated by the game once
 * a save loads).  Dereferenced once at read time.
 * ============================================================================ */

#define PAWN_MAIN_OFFSET        0x7F0       /* main pawn stride from character base */

#define OFF_BASE_REGION         0xA7000     /* character data region starts here */
#define OFF_LEVEL               0xDD0       /* UINT16, level (relative to base+pawn_offset) */
#define OFF_VOCATION            0x6E0       /* UINT32, current vocation (1..9) */
#define OFF_XP                  0x994
#define OFF_XP_NEXT             0x998
#define OFF_DP                  0xA14

#define OFF_STATS_FROM_BASE     0x96C       /* statsOffset = baseOffset + this */
#define OFF_VOCATIONS_FROM_STATS (13 * 4)   /* vocationOffset = statsOffset + this */
#define OFF_INCLINATIONS_FROM_STATS 0x1224  /* inclinationsOffset = statsOffset + this (pawn-only) */
#define OFF_SKILLS_REGION       0xA7808     /* skillsOffset = this + pawn_offset */

/* Base pointer for character data.  The signature `8B 15 ?? ?? ?? ?? 33 DB 8B F8`
 * is `MOV EDX, [imm32]; XOR EBX, EBX; MOV EDI, EAX` at a known game-code site.
 * The imm32 is the address of the *variable* that holds the actual base pointer;
 * so g_pawn_base_var is `BYTE**` - `*g_pawn_base_var` is the live base (NULL
 * until a save is loaded). */
static BYTE **g_pawn_base_var = NULL;

static void discover_pawn_base(void)
{
    HMODULE exe = GetModuleHandleA(NULL);       /* DDDA.exe */
    if (!exe) { log_line("discover_pawn_base: no exe handle"); return; }

    BYTE *exe_base = (BYTE*)exe;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)exe_base;
    PIMAGE_NT_HEADERS nt  = (PIMAGE_NT_HEADERS)(exe_base + dos->e_lfanew);
    BYTE *code_base = exe_base + nt->OptionalHeader.BaseOfCode;
    BYTE *code_end  = code_base + nt->OptionalHeader.SizeOfCode;

    static const unsigned char sig[]  = { 0x8B, 0x15, 0xCC, 0xCC, 0xCC, 0xCC, 0x33, 0xDB, 0x8B, 0xF8 };
    static const unsigned char mask[] = {    1,    1,    0,    0,    0,    0,    1,    1,    1,    1 };
    const size_t len = sizeof(sig);
    if ((size_t)(code_end - code_base) < len) return;

    for (BYTE *p = code_base; p <= code_end - len; p++) {
        int ok = 1;
        for (size_t i = 0; i < len; i++) {
            if (mask[i] && p[i] != sig[i]) { ok = 0; break; }
        }
        if (ok) {
            g_pawn_base_var = *(BYTE***)(p + 2);
            log_line("discover_pawn_base: pBase variable at %p (code site %p)",
                     (void*)g_pawn_base_var, (void*)p);
            return;
        }
    }
    log_line("discover_pawn_base: signature not found in .text");
}

/* Resolve (baseOffset + local_offset) to a typed pointer into live memory.
 * Returns NULL if the base isn't populated yet (no save loaded). */
static void *pawn_ptr(int pawn_offset, int local_offset)
{
    if (!g_pawn_base_var) return NULL;
    BYTE *base = *g_pawn_base_var;
    if (!base) return NULL;
    return base + OFF_BASE_REGION + pawn_offset + local_offset;
}

/* Absolute-offset variant for skills region (not relative to pawn's base chunk). */
static void *pawn_abs_ptr(int absolute_offset)
{
    if (!g_pawn_base_var) return NULL;
    BYTE *base = *g_pawn_base_var;
    if (!base) return NULL;
    return base + absolute_offset;
}

static const char *vocation_name(unsigned v)
{
    switch (v) {
        case 1: return "Fighter";
        case 2: return "Strider";
        case 3: return "Mage";
        case 4: return "Mystic Knight";
        case 5: return "Assassin";
        case 6: return "Magick Archer";
        case 7: return "Warrior";
        case 8: return "Ranger";
        case 9: return "Sorcerer";
        default: return "Unknown";
    }
}

/* Weapon-class skill table: JSON key, weapon-slot index, slot count.
 * Ported directly from PlayerStats.cpp renderStatsSkills(). Each weapon
 * gets 24 bytes (6 x UINT32) in the skills region; slot count says how
 * many are meaningful.  Slot 12 = augments (6 x UINT32 separately). */
struct weapon_skill_def { const char *key; int idx; int slot_count; };
static const struct weapon_skill_def g_weapon_skills[] = {
    { "sword",         0, 3 },
    { "mace",          1, 3 },
    { "longsword",     2, 3 },
    { "dagger",        3, 3 },
    { "staff",         4, 6 },
    { "archistaff",    5, 6 },
    { "warhammer",     6, 3 },
    { "shield",        7, 3 },
    { "magick_shield", 8, 3 },
    { "bow",           9, 3 },
    { "longbow",      10, 3 },
    { "magick_bow",   11, 3 },
    { "augments",     12, 6 },
};

/* Weapon-skill ID -> human name lookup.  Three-tier format "T1/T2/T3" where
 * T3 is the Dark Arisen mastered tier (only some skills have one); two-tier
 * "T1/T2" when no DA upgrade exists.  Tier order verified against the fandom
 * wiki: individual skill pages describe T2 as "an advanced form of T1", so
 * the base form is always first.
 *
 * Note: mace shares IDs 40-59 with sword, and warhammer shares 100-109 with
 * longsword, so one lookup covers all four weapon classes. */
struct skill_name_def { int id; const char *name; };
static const struct skill_name_def g_skill_names[] = {
    /* Sword / Mace */
    {  40, "Blink Strike/Burst Strike/Blitz Strike" },
    {  41, "Broad Cut/Broad Slash" },
    {  42, "Downthrust/Downcrack/Downcrush" },
    {  43, "Tusk Toss/Antler Toss" },
    {  44, "Compass Slash/Full Moon Slash" },
    {  45, "Skyward Lash/Heavenward Lash" },
    {  46, "Flesh Skewer/Soul Skewer/Fate Skewer" },
    {  47, "Hindsight Slash/Hindsight Sweep/Hindsight Strike" },
    {  48, "Stone Will/Steel Will" },
    {  49, "Legion's Bite/Dragon's Maw" },
    {  50, "Perilous Sigil/Ruinous Sigil" },
    {  51, "Magick Cannon/Great Cannon" },
    {  52, "Funnel Sigil/Vortex Sigil/Maelstrom Sigil" },
    {  53, "Sky Dance/Sky Rapture" },
    {  54, "Stone Grove/Stone Forest/Stone Jungle" },
    {  55, "Intimate Strike/Intimate Gambit" },
    {  56, "Windmill Slash/Great Windmill" },
    {  57, "Powder Charge/Powder Blast/Powder Barrage" },
    {  58, "Gouge/Dire Gouge/Deadly Gouge" },
    {  59, "Clarity/Clairvoyance" },
    /* Longsword / Warhammer */
    { 100, "Upward Strike/Whirlwind Slash" },
    { 101, "Pommel Strike/Pommel Bash" },
    { 102, "Savage Lunge/Indomitable Lunge/Calamitous Lunge" },
    { 103, "Escape Slash/Exodus Slash" },
    { 104, "Savage Lash/Indomitable Lash/Calamitous Lash" },
    { 105, "Ladder Blade/Catapult Blade" },
    { 106, "Spark Slash/Corona Slash/Ecliptic Slash" },
    { 107, "Act of Atonement/Act of Vengeance" },
    { 108, "Battle Cry/War Cry" },
    { 109, "Arc of Might/Arc of Deliverance/Arc of Obliteration" },
    /* Dagger */
    { 150, "Biting Wind/Cutting Wind/Shearing Wind" },
    { 151, "Toss and Trigger/Advanced Trigger" },
    { 152, "Scarlet Kisses/Hundred Kisses/Thousand Kisses" },
    { 153, "Dazzle Hold/Dazzle Blast" },
    { 154, "Sprint/Mad Dash" },
    { 155, "Helm Splitter/Skull Splitter/Brain Splitter" },
    { 156, "Ensnare/Implicate" },
    { 157, "Pilfer/Master Thief" },
    { 158, "Reset/Instant Reset" },
    { 159, "Stepping Stone/Leaping Stone/Soaring Stone" },
    { 160, "Sunburst/Sunflare" },
    { 161, "Shadowpin/Shadowshackle/Shadowsnare" },
    { 162, "Scension/Grand Scension" },
    { 163, "Magick Rebuffer/Magick Rebalancer" },
    { 164, "Wind Harness/Gale Harness/Tempest Harness" },
    { 165, "Back Kick/Escape Onslaught/Shirking Offensive" },
    { 166, "Spiderbite/Snakebite" },
    { 167, "Backfire/Immolation/Flameshroud" },
    { 168, "Stealth/Invisibility" },
    { 169, "Easy Kill/Masterful Kill" },
    /* Staff / Archistaff */
    { 210, "Ingle/High Ingle/Grand Ingle" },
    { 211, "Frazil/High Frazil" },
    { 212, "Levin/High Levin/Grand Levin" },
    { 213, "Comestion/High Comestion" },
    { 214, "Frigor/High Frigor/Grand Frigor" },
    { 215, "Brontide/High Brontide/Grand Brontide" },
    { 216, "Grapnel/High Grapnel" },
    { 217, "Silentium/High Silentium" },
    { 218, "Blearing/High Blearing" },
    { 219, "Lassitude/High Lassitude" },
    { 220, "Anodyne/High Anodyne/Grand Anodyne" },
    { 221, "Halidom/High Halidom" },
    { 222, "Fire Boon/Fire Affinity/Fire Pact" },
    { 223, "Ice Boon/Ice Affinity/Ice Pact" },
    { 224, "Thunder Boon/Thunder Affinity/Thunder Pact" },
    { 225, "Holy Boon/Holy Affinity/Holy Pact" },
    { 226, "Dark Boon/Dark Affinity/Dark Pact" },
    { 227, "Bolide/High Bolide/Grand Bolide" },
    { 228, "Gicel/High Gicel/Grand Gicel" },
    { 229, "Fulmination/High Fulmination/Grand Fulmination" },
    { 230, "Seism/High Seism/Grand Seism" },
    { 231, "Maelstrom/High Maelstrom" },
    { 232, "Exequy/High Exequy" },
    { 233, "Petrifaction/High Petrifaction" },
    { 234, "Miasma/High Miasma" },
    { 235, "Perdition/High Perdition" },
    { 236, "Sopor/High Sopor/Grand Sopor" },
    { 237, "Voidspell/High Voidspell" },
    { 238, "Spellscreen/High Spellscreen" },
    { 239, "Necromancy/High Necromancy" },
    /* Shield */
    { 270, "Shield Strike/Shield Storm/Shield Slam" },
    { 271, "Springboard/Launchboard" },
    { 272, "Shield Summons/Shield Drum" },
    { 273, "Cymbal Attack/Cymbal Onslaught" },
    { 274, "Sheltered Spike/Sheltered Assault/Sheltered Fusillade" },
    { 275, "Perfect Defense/Divine Defense" },
    { 276, "Moving Castle/Swift Castle" },
    { 277, "Flight Response/Enhanced Response" },
    { 278, "Staredown/Showdown/Crackdown" },
    /* Magick Shield */
    { 310, "Firecounter/Flame Riposte/Inferno Feint" },
    { 311, "Icecounter/Frost Riposte/Blizzard Feint" },
    { 312, "Thundercounter/Thunder Riposte/Boltstorm Feint" },
    { 313, "Holycounter/Blessed Riposte/Hallowed Feint" },
    { 314, "Darkcounter/Abyssal Riposte/Desecration Feint" },
    { 315, "Fire Enchanter/Flame Trance/Inferno Invocation" },
    { 316, "Ice Enchanter/Frost Trance/Blizzard Invocation" },
    { 317, "Thunder Enchanter/Lightning Trance/Boltstorm Invocation" },
    { 318, "Holy Enchanter/Blessed Trance/Hallowed Invocation" },
    { 319, "Dark Enchanter/Abyssal Trance/Desecration Invocation" },
    { 320, "Holy Glare/Holy Furor/Holy Retribution" },
    { 321, "Dark Anguish/Abyssal Anguish" },
    { 322, "Holy Wall/Holy Fortress" },
    { 323, "Demonspite/Demonswrath" },
    { 324, "Holy Aid/Holy Grace" },
    /* Bow (Shortbow) */
    { 350, "Threefold Arrow/Fivefold Flurry" },
    { 351, "Triad Shot/Pentad Shot" },
    { 352, "Full Bend/Mighty Bend/Terrible Bend" },
    { 353, "Cloudburst Volley/Downpour Volley/Hailstorm Volley" },
    { 354, "Splinter Dart/Fracture Dart" },
    { 355, "Whistle Dart/Shriek Dart" },
    { 356, "Keen Sight/Lyncean Sight/Eagle Sight" },
    { 357, "Puncture Dart/Skewer Dart" },
    { 358, "Blunting Arrow/Plegic Arrow" },
    /* Magick Bow */
    { 359, "Threefold Bolt/Sixfold Bolt/Ninefold Bolt" },
    { 360, "Seeker Bolt/Hunter Bolt" },
    { 361, "Explosive Bolt/Explosive Rivet/Explosive Volley" },
    { 362, "Ricochet Seeker/Ricochet Hunter" },
    { 363, "Magickal Flare/Magickal Gleam/Magickal Radiance" },
    { 364, "Funnel Trail/Vortex Trail" },
    { 365, "Ward Arrow/Great Ward Arrow" },
    { 366, "Bracer Arrow/Great Bracer Arrow" },
    { 367, "Sacrificial Bolt/Great Sacrifice" },
    /* Longbow */
    { 400, "Sixfold Arrow/Tenfold Flurry" },
    { 401, "Heptad Shot/Endecad Shot" },
    { 402, "Dire Arrow/Deathly Arrow/Reaper's Arrow" },
    { 403, "Foot Binder/Body Binder/Secure Binder" },
    { 404, "Invasive Arrow/Crippling Arrow" },
    { 405, "Flying Din/Fearful Din" },
    { 406, "Meteor Shot/Comet Shot" },
    { 407, "Whirling Arrow/Spiral Arrow/Corkscrew Arrow" },
    { 408, "Gamble Draw/Great Gamble" },
};

/* Augment ID -> name.  Separate table because augment IDs (0-6, 10-16, ...,
 * 60-63) overlap weapon-skill IDs (40 = Blink Strike as a sword skill
 * vs. Watchfulness as an augment).  Single-tier names. */
static const struct skill_name_def g_augment_names[] = {
    {  0, "Fitness" },       {  1, "Sinew" },        {  2, "Egression" },
    {  3, "Prescience" },    {  4, "Exhilaration" }, {  5, "Vehemence" },
    {  6, "Vigilance" },
    { 10, "Leg-Strength" },  { 11, "Arm-Strength" }, { 12, "Grit" },
    { 13, "Damping" },       { 14, "Dexterity" },    { 15, "Eminence" },
    { 16, "Endurance" },
    { 20, "Infection" },     { 21, "Equanimity" },   { 22, "Beatitude" },
    { 23, "Perpetuation" },  { 24, "Intervention" }, { 25, "Attunement" },
    { 26, "Apotropaism" },
    { 30, "Adamance" },      { 31, "Periphery" },    { 32, "Sanctuary" },
    { 33, "Restoration" },   { 34, "Retribution" },  { 35, "Reinforcement" },
    { 36, "Fortitude" },
    { 40, "Watchfulness" },  { 41, "Preemption" },   { 42, "Autonomy" },
    { 43, "Bloodlust" },     { 44, "Entrancement" }, { 45, "Sanguinity" },
    { 46, "Toxicity" },
    { 50, "Resilience" },    { 51, "Resistance" },   { 52, "Detection" },
    { 53, "Regeneration" },  { 54, "Allure" },       { 55, "Potential" },
    { 56, "Magnitude" },
    { 60, "Temerity" },      { 61, "Audacity" },     { 62, "Proficiency" },
    { 63, "Ferocity" },      { 64, "Impact" },       { 65, "Bastion" },
    { 66, "Clout" },
    { 70, "Trajectory" },    { 71, "Morbidity" },    { 72, "Precision" },
    { 73, "Stability" },     { 74, "Efficacy" },     { 75, "Radiance" },
    { 76, "Longevity" },
    { 80, "Gravitas" },      { 81, "Articulacy" },   { 82, "Conservation" },
    { 83, "Emphasis" },      { 84, "Suasion" },      { 85, "Acuity" },
    { 86, "Awareness" },
    /* dinput8 lists 90 as "Unknown" and 91 as "Suasion" (duplicate of 84) -
     * likely placeholders; left as-is. */
    { 90, "Unknown" },       { 91, "Suasion" },      { 92, "Thrift" },
    { 93, "Weal" },          { 94, "Renown" },
    { 100, "Predation" },    { 101, "Fortune" },     { 102, "Tenacity" },
    { 103, "Conveyance" },   { 104, "Acquisition" }, { 105, "Prolongation" },
    { 107, "Mettle" },       { 108, "Athleticism" }, { 109, "Recuperation" },
    { 110, "Adhesion" },     { 111, "Opportunism" }, { 112, "Flow" },
    { 113, "Grace" },        { 114, "Facility" },
};

static const char *lookup_name(const struct skill_name_def *tbl, size_t n, int id)
{
    for (size_t i = 0; i < n; i++) if (tbl[i].id == id) return tbl[i].name;
    return NULL;
}

/* Format a skill-slot value as a JSON string.  -1 = empty slot; known id
 * renders "ID: Name"; unknown id renders just the id.  `is_augment` selects
 * the lookup table since augment IDs collide with weapon-skill IDs. */
static void format_skill_slot(int32_t id, int is_augment, char *out, size_t out_cap)
{
    if (id == -1) {
        _snprintf(out, out_cap, "empty");
        if (out_cap > 0) out[out_cap - 1] = 0;
        return;
    }
    const char *name = is_augment
        ? lookup_name(g_augment_names, sizeof(g_augment_names)/sizeof(g_augment_names[0]), id)
        : lookup_name(g_skill_names,   sizeof(g_skill_names)  /sizeof(g_skill_names[0]),   id);
    if (name) _snprintf(out, out_cap, "%d: %s", (int)id, name);
    else      _snprintf(out, out_cap, "%d",     (int)id);
    if (out_cap > 0) out[out_cap - 1] = 0;
}

/* Write a JSON descriptor for the main pawn to <pawn_path with .json ext>.
 * Returns 1 on success, 0 when the base pointer isn't live yet or the write fails. */
static int write_json(const char *pawn_path)
{
    if (!g_pawn_base_var || !*g_pawn_base_var) {
        log_line("write_json: pawn base not resolved yet, skipping");
        return 0;
    }

    /* Replace trailing ".pawn" with ".json". */
    char dst[MAX_PATH];
    _snprintf(dst, sizeof(dst), "%s", pawn_path);
    char *ext = strrchr(dst, '.');
    if (!ext) return 0;
    strcpy(ext, ".json");

    FILE *f = fopen(dst, "wb");
    if (!f) { log_line("write_json: fopen '%s' failed, errno=%d", dst, errno); return 0; }

    const int po = PAWN_MAIN_OFFSET;

    uint16_t level  = *(uint16_t*)pawn_ptr(po, OFF_LEVEL);
    uint32_t voc    = *(uint32_t*)pawn_ptr(po, OFF_VOCATION);
    uint32_t xp     = *(uint32_t*)pawn_ptr(po, OFF_XP);
    uint32_t xpnext = *(uint32_t*)pawn_ptr(po, OFF_XP_NEXT);
    uint32_t dp     = *(uint32_t*)pawn_ptr(po, OFF_DP);

    float *stats = (float*)pawn_ptr(po, OFF_STATS_FROM_BASE);
    float hp_cur  = stats[0], hp_max  = stats[1], hp_pp  = stats[2];
    float st_cur  = stats[3], st_max  = stats[4], st_pp  = stats[5];
    float atk     = stats[6], def     = stats[7], matk   = stats[8], mdef = stats[9];

    fprintf(f, "{\n");
    fprintf(f, "  \"level\": %u,\n", (unsigned)level);
    fprintf(f, "  \"vocation\": { \"id\": %u, \"name\": \"%s\" },\n",
            (unsigned)voc, vocation_name(voc));
    fprintf(f, "  \"xp\": %u,\n", (unsigned)xp);
    fprintf(f, "  \"xp_next\": %u,\n", (unsigned)xpnext);
    fprintf(f, "  \"dp\": %u,\n", (unsigned)dp);

    fprintf(f, "  \"stats\": {\n");
    fprintf(f, "    \"hp\":             { \"current\": %g, \"max\": %g, \"max_plus\": %g },\n",
            hp_cur, hp_max, hp_pp);
    fprintf(f, "    \"stamina\":        { \"current\": %g, \"max\": %g, \"max_plus\": %g },\n",
            st_cur, st_max, st_pp);
    fprintf(f, "    \"attack\": %g,\n", atk);
    fprintf(f, "    \"defense\": %g,\n", def);
    fprintf(f, "    \"magick_attack\": %g,\n", matk);
    fprintf(f, "    \"magick_defense\": %g\n", mdef);
    fprintf(f, "  },\n");

    /* Per-vocation rank/XP/next.  Three parallel UINT32 arrays at strides of 0x28. */
    uint32_t *vrank = (uint32_t*)pawn_ptr(po, OFF_STATS_FROM_BASE + OFF_VOCATIONS_FROM_STATS);
    uint32_t *vxp   = (uint32_t*)((char*)vrank + 0x28);
    uint32_t *vnext = (uint32_t*)((char*)vrank + 0x50);
    fprintf(f, "  \"vocations\": {\n");
    for (int i = 0; i < 9; i++) {
        fprintf(f, "    \"%s\": { \"rank\": %u, \"xp\": %u, \"xp_next\": %u }%s\n",
                vocation_name(i + 1),
                (unsigned)vrank[i], (unsigned)vxp[i], (unsigned)vnext[i],
                i == 8 ? "" : ",");
    }
    fprintf(f, "  },\n");

    /* Inclinations: 9 floats at strides of 12 bytes, plus "skill_use" at +116. */
    float *incl = (float*)pawn_ptr(po, OFF_STATS_FROM_BASE + OFF_INCLINATIONS_FROM_STATS);
    static const char *incl_names[9] = {
        "scather", "medicant", "mitigator", "challenger", "utilitarian",
        "guardian", "nexus", "pioneer", "acquisitor"
    };
    fprintf(f, "  \"inclinations\": {\n");
    for (int i = 0; i < 9; i++) {
        float v = *(float*)((char*)incl + i * 12);
        fprintf(f, "    \"%s\": %g,\n", incl_names[i], v);
    }
    float skill_use = *(float*)((char*)incl + 8 * 12 + 20);
    fprintf(f, "    \"skill_use\": %g\n", skill_use);
    fprintf(f, "  },\n");

    /* Equipped skills per weapon class. Emitted as JSON strings ("ID: Name"
     * when mapped, bare "ID" when unmapped, "empty" for -1). */
    uint32_t *skills = (uint32_t*)pawn_abs_ptr(OFF_SKILLS_REGION + po);
    fprintf(f, "  \"equipped_skills\": {\n");
    const int n_weapons = (int)(sizeof(g_weapon_skills) / sizeof(g_weapon_skills[0]));
    for (int w = 0; w < n_weapons; w++) {
        const struct weapon_skill_def *ws = &g_weapon_skills[w];
        uint32_t *row = skills + (ws->idx * 6);
        int is_augment = (ws->idx == 12);
        fprintf(f, "    \"%s\": [", ws->key);
        for (int s = 0; s < ws->slot_count; s++) {
            char buf[64];
            format_skill_slot((int32_t)row[s], is_augment, buf, sizeof(buf));
            fprintf(f, "%s\"%s\"", s ? ", " : "", buf);
        }
        fprintf(f, "]%s\n", w == n_weapons - 1 ? "" : ",");
    }
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
    fclose(f);
    log_line("write_json: wrote '%s'", dst);
    return 1;
}

/* Read the main pawn's level from live memory.  Returns 0 if the base pointer
 * isn't resolved yet or isn't populated. */
static int read_pawn_level(void)
{
    if (!g_pawn_base_var || !*g_pawn_base_var) return 0;
    uint16_t *pLevel = (uint16_t*)pawn_ptr(PAWN_MAIN_OFFSET, OFF_LEVEL);
    if (!pLevel) return 0;
    return (int)(*pLevel);
}

/* Archive the user's current main pawn (remote/0) into <save_dir>/<NNN>/<HEX>.pawn
 * where <NNN> is the pawn's current level zero-padded to 3 digits, and <HEX>
 * is the 8-hex-digit unix-epoch offset from 2026-01-01 UTC. The same
 * "NNN:HEX" stem is injected into the pawn's mArisenName cName slot(s) at
 * XFS offsets 0x36F0 and 0x375E, so the archive self-identifies on a
 * subsequent hire — at release time we read mArisenName out of game memory
 * and know which file on disk to update.
 *
 * Writes sibling .meta / .json under the same stem. Skips the archive
 * entirely if the level can't be read or is outside the game's valid
 * range (1..200). save_dir may contain forward slashes (user-authored INI);
 * we normalize and create each component. Preview is never archived -
 * preview UGC reads are served live from remote/1 at UGCRead time. */
/* 2026-01-01 00:00:00 UTC. Chosen so the u32 offset stays small and readable
 * in the embedded stem. A u32 covers ~136 years from this base — plenty. */
#define EPOCH_2026_UTC 1767225600u

/* cName capacity at the mArisenName slot(s) in the XFS instance data.
 * Both 0x36F0 and 0x375E host an identical cName with u32-capacity prefix
 * right before the bytes; the value there is 0x19 = 25 in every archive
 * we've inspected (see gear-persistence.md for the RE write-up). */
#define MARISENNAME_CAP        25
#define MARISENNAME_OFF_1      0x36F0u
#define MARISENNAME_OFF_2      0x375Eu

static void archive_rest(const int32 *fresh_details)
{
    int level = read_pawn_level();
    if (level < 1 || level > 200) {
        log_line("archive_rest: pawn level %d out of range (need 1..200); skipping archive", level);
        return;
    }

    /* Compute the archive stem: "NNN:HHHHHHHH". */
    uint32_t epoch_off = (uint32_t)((uint64_t)time(NULL) - (uint64_t)EPOCH_2026_UTC);
    char stem_hex[16];
    _snprintf(stem_hex, sizeof(stem_hex), "%08X", epoch_off);
    char stem_full[16];
    _snprintf(stem_full, sizeof(stem_full), "%03u:%s", (unsigned)level, stem_hex);

    /* Destination dir: <dll_dir>/<save_dir>/<NNN>. */
    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%s%s\\%03u", g_dll_dir, g_save_dir_rel, (unsigned)level);
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';

    /* Create directory components (idempotent). */
    for (char *p = dir + 1; *p; p++) {
        if (*p == '\\') { *p = 0; CreateDirectoryA(dir, NULL); *p = '\\'; }
    }
    CreateDirectoryA(dir, NULL);

    char dst[MAX_PATH];
    _snprintf(dst, sizeof(dst), "%s\\%s.pawn", dir, stem_hex);

    char src0[MAX_PATH];
    _snprintf(src0, sizeof(src0), "%s\\0", g_remote_dir);
    long n = copy_file(src0, dst);
    if (n <= 0) {
        log_line("archive_rest: copy_file failed for '%s' (errno=%d)", dst, errno);
        return;
    }
    log_line("archive_rest: wrote %ld bytes (level %d) to '%s'", n, level, dst);
    write_meta(dst, fresh_details);
    write_json(dst);

    /* Stamp mArisenName with the archive stem so this blob self-identifies
     * when later hired. Both cName occurrences get the same bytes; cName
     * capacity is 25 so we zero-pad to match. Failure is non-fatal — the
     * archive is still playable, just without release-time writeback. */
    uint8_t name_buf[MARISENNAME_CAP];
    memset(name_buf, 0, sizeof(name_buf));
    size_t slen = strlen(stem_full);
    if (slen > MARISENNAME_CAP - 1) slen = MARISENNAME_CAP - 1;
    memcpy(name_buf, stem_full, slen);
    struct pawnxfs_patch patches[2] = {
        { MARISENNAME_OFF_1, name_buf, sizeof(name_buf) },
        { MARISENNAME_OFF_2, name_buf, sizeof(name_buf) },
    };
    char perr[128] = {0};
    int rc = pawnxfs_poke(dst, dst, patches, 2, perr, sizeof(perr));
    if (rc != 0) {
        log_line("archive_rest: mArisenName stamp failed rc=%d (%s) on '%s'", rc, perr, dst);
    } else {
        log_line("archive_rest: stamped mArisenName='%s' into '%s'", stem_full, dst);
    }
}

/* Read the 18-int32 details from a local leader_N file (entry 0 only).
 * Returns 1 on success, 0 on failure. */
static int load_template_details(const char *leader_name, int32 out[18])
{
    char full[MAX_PATH];
    _snprintf(full, sizeof(full), "%s\\%s", g_leader_dir, leader_name);
    FILE *f = fopen(full, "rb");
    if (!f) return 0;
    uint32 header[4];
    if (fread(header, 4, 4, f) != 4) { fclose(f); return 0; }
    uint32 cnt = header[3];
    if (cnt < 18) { fclose(f); return 0; }
    size_t n = fread(out, 4, 18, f);
    fclose(f);
    return n == 18;
}

/* Read the first 8 bytes of any existing leaderboard file to recover the owner's steamID. */
static CSteamID read_self_id_from_disk(void)
{
    const char *candidates[] = { "leader_227", "leader_228", "leader_101", "leader_2", "leader_231", NULL };
    for (int i = 0; candidates[i]; i++) {
        char full[MAX_PATH];
        _snprintf(full, sizeof(full), "%s\\%s", g_leader_dir, candidates[i]);
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        uint32 lo = 0, hi = 0;
        if (fread(&lo, 4, 1, f) == 1 && fread(&hi, 4, 1, f) == 1) {
            fclose(f);
            CSteamID id = (CSteamID)lo | ((CSteamID)hi << 32);
            log_line("read_self_id_from_disk: got %llu from '%s'",
                     (unsigned long long)id, candidates[i]);
            return id;
        }
        fclose(f);
    }
    log_line("read_self_id_from_disk: no leaderboard file readable, falling back to 0");
    return 0;
}

/* DDDA stores the low 32 bits of the UGCHandle byte-swapped in details[0]
 * (it reads them back with ntohl/bswap when reconstructing the handle).
 * details[1] holds the high 32 bits unswapped. */
static UGCHandle_t ugc_from_encoded_details(uint32 details0, uint32 details1)
{
    uint32 lo = __builtin_bswap32(details0);
    return ((uint64_t)details1 << 32) | lo;
}

/* Inverse: given an actual UGCHandle, produce the (details0, details1) the game
 * would have stored for it. Used when we inject fake leaderboard entries. */
static void ugc_to_encoded_details(UGCHandle_t ugc, uint32 *out_d0, uint32 *out_d1)
{
    uint32 lo = (uint32)(ugc & 0xffffffffu);
    uint32 hi = (uint32)(ugc >> 32);
    *out_d0 = __builtin_bswap32(lo);
    *out_d1 = hi;
}

/* ============================================================================
 * Fake pawn registry: identity derivation, scan, lookup
 * ============================================================================ */

/* FNV-1a 32-bit hash - used to derive stable identities per archive file.
 * Same input always yields the same (fake_steamid, ugc_main, ugc_preview) so
 * every rewrite of leader_231/232/N agrees on a given archive's identity
 * (and the same identity is reproduced across sessions since filenames are
 * stable). */
static uint32 hash_str(const char *s)
{
    uint32 h = 2166136261u;
    while (*s) { h ^= (uint8_t)(*s++); h *= 16777619u; }
    return h;
}

/* Derive a valid CSteamID + two distinct UGCHandles from an archive file's stem.
 * - steamid: keep self's upper 32 bits (so universe/type/instance are valid);
 *            vary account_id in a range that won't collide with the real user.
 * - ugc_main / ugc_preview: prefix 0xFA** so they're distinguishable from the
 *            random handles gbe_fork generates via FileShare. */
static void derive_fake_identity(const char *stem, CSteamID self,
                                 CSteamID *out_sid, UGCHandle_t *out_main, UGCHandle_t *out_preview)
{
    uint32 h = hash_str(stem);
    uint64_t upper = self & 0xffffffff00000000ULL;
    /* Account ID set to 0x80000000..0xFFFFFFFF - comfortably above real user IDs (~835M range). */
    uint32 acct = 0x80000000u | (h & 0x7fffffffu);
    *out_sid     = upper | (uint64_t)acct;
    *out_main    = 0xFAFA000100000000ULL | (uint64_t)h;
    *out_preview = 0xFAFA000200000000ULL | (uint64_t)h;
}

static int find_fake_by_steamid(CSteamID sid)
{
    for (int i = 0; i < g_fake_count; i++) {
        if (g_fakes[i].steamid == sid) return i;
    }
    return -1;
}

static int find_fake_by_ugc(UGCHandle_t h, int *is_preview_out)
{
    for (int i = 0; i < g_fake_count; i++) {
        if (g_fakes[i].ugc_main == h)    { if (is_preview_out) *is_preview_out = 0; return i; }
        if (g_fakes[i].ugc_preview == h) { if (is_preview_out) *is_preview_out = 1; return i; }
    }
    return -1;
}

/* Load a .meta sidecar (18 int32) into the fake.  When absent, leaves the fake's
 * details zero-filled and has_details=0 (search-board writer falls back to the
 * g_last_18_details template for that row). */
static void load_fake_meta(struct FakePawn *fp)
{
    fp->has_details = 0;
    memset(fp->details, 0, sizeof(fp->details));
    FILE *f = fopen(fp->meta_path, "rb");
    if (!f) return;
    if (fread(fp->details, sizeof(int32), 18, f) == 18) fp->has_details = 1;
    fclose(f);
}

/* Parse "NNN" (zero-padded 3-digit level) -> N. Returns 0 on malformed input. */
static int parse_level_folder(const char *name)
{
    if (!name || !*name) return 0;
    int v = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 100000) return 0;  /* sanity */
    }
    return v;
}

/* qsort comparator: descending mtime (most recent first). */
static int cmp_fake_mtime_desc(const void *a, const void *b)
{
    uint64_t ma = ((const struct FakePawn*)a)->mtime;
    uint64_t mb = ((const struct FakePawn*)b)->mtime;
    if (mb > ma) return  1;
    if (mb < ma) return -1;
    return 0;
}

/* Rescan every .pawn under <save_dir>/<NNN>/; repopulate g_fakes[].
 * Called once per session from the worker thread (or lazily from write_rift_boards
 * if the worker couldn't resolve self steamID).  O(N) in total archive count;
 * small folders so cost is cheap.  Sorts by mtime descending so
 * write_search_board takes the first K = most recent. */
static int rescan_all_levels(CSteamID self)
{
    EnterCriticalSection(&g_fakes_lock);
    g_fake_count = 0;

    char root[MAX_PATH];
    _snprintf(root, sizeof(root), "%s%s", g_dll_dir, g_save_dir_rel);
    for (char *p = root; *p; p++) if (*p == '/') *p = '\\';

    /* Scan all subdirectories under <save_dir>; parse_level_folder accepts
     * only zero-padded "NNN" names. Non-parseable names yield level 0 and
     * are skipped below. */
    char pat[MAX_PATH];
    _snprintf(pat, sizeof(pat), "%s\\*", root);

    int with_meta = 0;
    int levels_scanned = 0;

    WIN32_FIND_DATAA fd_lvl;
    HANDLE h_lvl = FindFirstFileA(pat, &fd_lvl);
    if (h_lvl != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd_lvl.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            int level = parse_level_folder(fd_lvl.cFileName);
            if (level < 1 || level > 200) continue;
            levels_scanned++;

            char sub_dir[MAX_PATH];
            _snprintf(sub_dir, sizeof(sub_dir), "%s\\%s", root, fd_lvl.cFileName);
            char sub_pat[MAX_PATH];
            _snprintf(sub_pat, sizeof(sub_pat), "%s\\*.pawn", sub_dir);

            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(sub_pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (g_fake_count >= MAX_FAKES) break;

                struct FakePawn *fp = &g_fakes[g_fake_count];
                memset(fp, 0, sizeof(*fp));
                _snprintf(fp->pawn_path, sizeof(fp->pawn_path), "%s\\%s", sub_dir, fd.cFileName);

                /* Strip ".pawn" to get the stem used for identity hashing + meta path. */
                char stem[64];
                strncpy(stem, fd.cFileName, sizeof(stem) - 1);
                stem[sizeof(stem) - 1] = 0;
                char *dot = strrchr(stem, '.');
                if (dot) *dot = 0;
                _snprintf(fp->meta_path, sizeof(fp->meta_path), "%s\\%s.meta", sub_dir, stem);

                derive_fake_identity(stem, self, &fp->steamid, &fp->ugc_main, &fp->ugc_preview);
                load_fake_meta(fp);
                if (fp->has_details) with_meta++;
                fp->level = level;
                fp->mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32)
                          | (uint64_t)fd.ftLastWriteTime.dwLowDateTime;
                g_fake_count++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        } while (FindNextFileA(h_lvl, &fd_lvl));
        FindClose(h_lvl);
    }

    /* Sort descending by mtime so write_search_board's take-first-K is naturally
     * "most recent first". */
    qsort(g_fakes, g_fake_count, sizeof(g_fakes[0]), cmp_fake_mtime_desc);

    int n = g_fake_count;
    LeaveCriticalSection(&g_fakes_lock);

    log_line("rescan_all_levels: loaded %d fake pawn(s) across %d level folder(s) (%d with .meta)",
             n, levels_scanned, with_meta);
    return n;
}

/* Read the existing on-disk leaderboard file, keeping only entries that look like
 * real-user entries (account_id < 0x80000000 - our fakes use the high half).
 * Returns number of bytes copied into `out`. This prevents fakes we wrote in a
 * previous session from being re-preserved as if they were real user data. */
static size_t filter_real_entries(const unsigned char *in, size_t in_len,
                                  unsigned char *out, size_t out_cap)
{
    size_t ri = 0, wi = 0;
    while (ri + 16 <= in_len) {
        uint32 sid_lo   = *(const uint32*)(in + ri);
        uint32 cDetails = *(const uint32*)(in + ri + 12);
        size_t entry_size = 16 + (size_t)cDetails * 4;
        if (ri + entry_size > in_len) break;
        if (sid_lo < 0x80000000u) {   /* real-user account ID */
            if (wi + entry_size > out_cap) break;
            memcpy(out + wi, in + ri, entry_size);
            wi += entry_size;
        }
        ri += entry_size;
    }
    return wi;
}

/* Overwrite a pawn-UGC board (leader_231 or leader_232) on disk with the user's
 * real entries (filtered out of the existing file) plus one row per fake. */
static void write_ugc_board(const char *board_name, int use_preview_handle)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\%s", g_leader_dir, board_name);

    unsigned char raw[4096], kept[4096];
    size_t raw_len = 0, kept_len = 0;
    FILE *fr = fopen(path, "rb");
    if (fr) { raw_len = fread(raw, 1, sizeof(raw), fr); fclose(fr); }
    if (raw_len > 0) kept_len = filter_real_entries(raw, raw_len, kept, sizeof(kept));

    FILE *fw = fopen(path, "wb");
    if (!fw) {
        log_line("write_ugc_board(%s): open-for-write failed, errno=%d", board_name, errno);
        return;
    }
    if (kept_len > 0) fwrite(kept, 1, kept_len, fw);

    EnterCriticalSection(&g_fakes_lock);
    for (int i = 0; i < g_fake_count; i++) {
        const struct FakePawn *fp = &g_fakes[i];
        UGCHandle_t ugc = use_preview_handle ? fp->ugc_preview : fp->ugc_main;
        uint32 d0, d1;
        ugc_to_encoded_details(ugc, &d0, &d1);

        uint32 row[6];
        row[0] = (uint32)(fp->steamid & 0xffffffffu);
        row[1] = (uint32)(fp->steamid >> 32);
        row[2] = (uint32)(10000 - i);   /* descending score for stable sort */
        row[3] = 2;
        row[4] = d0;
        row[5] = d1;
        fwrite(row, sizeof(row), 1, fw);
    }
    int n = g_fake_count;
    LeaveCriticalSection(&g_fakes_lock);

    fclose(fw);
    log_line("write_ugc_board(%s): kept %zu real bytes + %d fakes (use_preview=%d)",
             board_name, kept_len, n, use_preview_handle);
}

/* Write leader_<N> (per-level search board) on disk with the most-recent
 * g_max_search_results fakes whose level matches `level`.  g_fakes[] is
 * pre-sorted mtime-descending by rescan_all_levels, so we just iterate and
 * pick the first K matches.  Other-level fakes are skipped; no user entries
 * are preserved (leader_<N> is never written by the game itself). */
static void write_search_board(const char *board_name, int level)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\%s", g_leader_dir, board_name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        log_line("write_search_board(%s): fopen failed, errno=%d", board_name, errno);
        return;
    }

    int written = 0;
    int hidden_current_session = 0;
    EnterCriticalSection(&g_fakes_lock);
    for (int i = 0; i < g_fake_count && written < g_max_search_results; i++) {
        const struct FakePawn *fp = &g_fakes[i];
        if (fp->level != level) continue;

        /* Hide archives written during this session from the rift - the game's
         * in-memory pawn-dedup cache refuses to summon a pawn with an ID matching
         * the Arisen's freshly-uploaded main pawn.  Cross-session summon works,
         * so these stay in leader_231/232 (written by write_ugc_board). */
        if (fp->mtime >= g_session_start_ft) { hidden_current_session++; continue; }

        uint32 d0, d1;
        ugc_to_encoded_details(fp->ugc_main, &d0, &d1);

        uint32 header[4];
        header[0] = (uint32)(fp->steamid & 0xffffffffu);
        header[1] = (uint32)(fp->steamid >> 32);
        header[2] = (uint32)(10000 - written);    /* descending score for stable sort */
        header[3] = 18;
        fwrite(header, sizeof(header), 1, f);

        /* Per-pawn .meta when present, else fall back to the template snapshot. */
        int32 det[18];
        if (fp->has_details) memcpy(det, fp->details,         sizeof(det));
        else                 memcpy(det, g_last_18_details,   sizeof(det));
        det[0] = (int32)d0;
        det[1] = (int32)d1;
        fwrite(det, sizeof(det), 1, f);
        written++;
    }
    LeaveCriticalSection(&g_fakes_lock);

    fclose(f);
    log_line("write_search_board(%s): wrote %d entries (level %d, cap %d, hid %d current-session)",
             board_name, written, level, g_max_search_results, hidden_current_session);
}

/* Call FileShare on a known-existing remote file ourselves and harvest the resulting
 * UGCHandle via ISteamUtils::GetAPICallResult. This sidesteps the requirement that the
 * user rest at an inn before summoning an archived pawn - we set up a valid entry in
 * gbe_fork's shared_files map at startup so the UGCDownload substitution has a target
 * handle that actually resolves.
 *
 * Returns 0 on failure (e.g. the remote file doesn't exist yet on disk). */
static UGCHandle_t acquire_fresh_ugc_via_fileshare(void)
{
    if (!g_steam_remotestorage || !g_steam_utils || !g_orig_FileShare) return 0;

    /* Prefer an anchor file we create+control so we don't stomp on the game's remote/0
     * if it happens to be in flux. Create it if missing so FileShare can always succeed. */
    const char *anchor = "pawndb_anchor";
    char anchor_path[MAX_PATH];
    _snprintf(anchor_path, sizeof(anchor_path), "%s\\%s", g_remote_dir, anchor);
    FILE *fcheck = fopen(anchor_path, "rb");
    if (!fcheck) {
        FILE *fnew = fopen(anchor_path, "wb");
        if (!fnew) {
            log_line("acquire_fresh_ugc: could not create anchor '%s' (errno=%d)", anchor_path, errno);
            return 0;
        }
        static const char placeholder[] = "pawndb placeholder for UGC handle acquisition";
        fwrite(placeholder, 1, sizeof(placeholder), fnew);
        fclose(fnew);
        log_line("acquire_fresh_ugc: created anchor file '%s'", anchor_path);
    } else {
        fclose(fcheck);
    }

    /* Kick off FileShare through the original (unhooked) entry - we don't want our own
     * FileShare hook logging this as if it were a game-triggered share. */
    SteamAPICall_t hCall = g_orig_FileShare(g_steam_remotestorage, anchor);
    if (hCall == 0) {
        log_line("acquire_fresh_ugc: FileShare returned 0");
        return 0;
    }
    log_line("acquire_fresh_ugc: FileShare('%s') call id = %llu", anchor, (unsigned long long)hCall);

    /* ISteamUtils vtable slots (v007):
     *   [11] IsAPICallCompleted(hCall, bool *pbFailed) -> bool
     *   [13] GetAPICallResult(hCall, void *pCallback, int cub, int iCallbackExpected, bool *pbFailed) -> bool */
    typedef unsigned char (__thiscall *fnIsAPICallCompleted)(void *, SteamAPICall_t, unsigned char *);
    typedef unsigned char (__thiscall *fnGetAPICallResult)(void *, SteamAPICall_t, void *, int, int, unsigned char *);
    void **vt = *(void***)g_steam_utils;
    fnIsAPICallCompleted isCompleted = (fnIsAPICallCompleted)vt[11];
    fnGetAPICallResult   getResult   = (fnGetAPICallResult)vt[13];

    /* Poll for completion. gbe_fork's addCallResult makes results available quickly but
     * some dispatch paths depend on SteamAPI_RunCallbacks which runs on the game's main thread. */
    unsigned char failed = 0;
    int ok = 0;
    for (int i = 0; i < 200; i++) {
        if (isCompleted(g_steam_utils, hCall, &failed)) { ok = 1; break; }
        Sleep(10);
    }
    if (!ok) {
        log_line("acquire_fresh_ugc: IsAPICallCompleted never returned true (timeout)");
        return 0;
    }

    #pragma pack(push, 8)
    struct ShareResult {
        int32_t   m_eResult;      /* EResult (4 bytes), pad aligns m_hFile to 8 */
        int32_t   _pad;
        uint64_t  m_hFile;
        char      m_rgchFilename[260];
    };
    #pragma pack(pop)

    struct ShareResult r = {0};
    failed = 0;
    if (!getResult(g_steam_utils, hCall, &r, (int)sizeof(r), 1307 /*RemoteStorageFileShareResult_t::k_iCallback*/, &failed) || failed) {
        log_line("acquire_fresh_ugc: GetAPICallResult failed (failed_flag=%d)", failed);
        return 0;
    }
    if (r.m_eResult != 1 /*k_EResultOK*/) {
        log_line("acquire_fresh_ugc: EResult=%d (not OK)", r.m_eResult);
        return 0;
    }
    log_line("acquire_fresh_ugc: captured UGCHandle 0x%016llx (from '%s')",
             (unsigned long long)r.m_hFile, anchor);
    return r.m_hFile;
}

/* Returns 1 if `name` is a rift board the shim cares about: a per-level search
 * board (leader_<N>, N in 1..200) OR one of the UGC-handle boards
 * (leader_231 / leader_232).  Everything else (e.g. the 18-int stat boards
 * the game uploads to) is pass-through. */
static int is_rift_board(const char *name)
{
    if (!name || strncmp(name, "leader_", 7) != 0) return 0;
    if (strcmp(name, "leader_231") == 0 || strcmp(name, "leader_232") == 0) return 1;
    int lvl = atoi(name + 7);
    return (lvl >= 1 && lvl <= 200) ? 1 : 0;
}

/* Narrower: rift board AND not one of the UGC boards. */
static int is_search_board(const char *name)
{
    return is_rift_board(name)
        && strcmp(name, "leader_231") != 0
        && strcmp(name, "leader_232") != 0;
}

/* Rewrite leader_231, leader_232, and (for per-level requests) leader_<N> from the
 * cached in-memory g_fakes[].  No disk rescan - the archive set is frozen at session
 * start (current-session archives are hidden by design, so mid-session disk changes
 * don't affect rift visibility).  We still rewrite on every FindLeaderboard because
 * gbe_fork's UploadLeaderboardScore (fired by the game's cDetails=2 upload during
 * rest) overwrites leader_231/232 with just the user's entry, wiping our fakes.
 *
 * If the initial scan was skipped at worker startup (no self steamID on disk), we
 * retry it here - by this point the user has been playing long enough that
 * read_self_id_from_disk should succeed. */
static void write_rift_boards(const char *request_name)
{
    if (g_fake_count == 0) {
        if (g_captured_self == 0) g_captured_self = read_self_id_from_disk();
        if (g_captured_self == 0) {
            log_line("write_rift_boards: no self steamID available, skipping");
            return;
        }
        rescan_all_levels(g_captured_self);
    }
    write_ugc_board("leader_231", /*use_preview=*/0);
    write_ugc_board("leader_232", /*use_preview=*/1);
    if (is_search_board(request_name)) {
        int level = atoi(request_name + 7);
        write_search_board(request_name, level);
    }
}

/* ============================================================================
 * Hook implementations (ISteamUserStats - control plane)
 * ============================================================================ */

/* Called from FindLeaderboard / FindOrCreateLeaderboard BEFORE passing through.
 * If the board is a rift board, rewrites leader_231/232 (and, for per-level
 * requests, leader_<N>) from the cached g_fakes[] - needed because gbe_fork's
 * own writes during rest cycles wipe our fakes. */
static void maybe_populate_search_board(const char *name)
{
    if (!is_rift_board(name)) return;
    log_line("FindLeaderboard('%s'): refreshing boards", name);
    write_rift_boards(name);
}

static SteamAPICall_t __thiscall hook_FindLeaderboard(void *self, const char *name)
{
    if (name) maybe_populate_search_board(name);
    return g_orig_FindLeaderboard(self, name);
}

static SteamAPICall_t __thiscall hook_FindOrCreateLeaderboard(
    void *self, const char *name, int sort_method, int display_type)
{
    if (name) maybe_populate_search_board(name);
    return g_orig_FindOrCreateLeaderboard(self, name, sort_method, display_type);
}

/* ---- Fake-SteamID scrubber: helpers ----
 * See the design block near the top of the file. Summary: zero the hired
 * pawns' mNetUniqueId.mData at known offsets from *pBase, learned once from
 * mCmc[1] the first time we see it populated with a fake. */

static int netuid_pattern_match(const BYTE *p)
{
    /* type tag = Steam(2) */
    if (*(const uint32_t*)p != 0x00000002u) return 0;
    /* Steam universe/type/instance = 0x01100001 LE */
    if (*(const uint32_t*)(p + 8) != 0x01100001u) return 0;
    /* byte 7 MSB set = mod-fake account_id (mod uses 0x80000000..0xFFFFFFFF);
     * real users are ~0x30000000..0x40000000, MSB clear. */
    if ((p[7] & 0x80) == 0) return 0;
    /* rest of 64-byte mData buffer must be zero (Steam IDs only use bytes 0..11) */
    for (int i = 12; i < 64; i++) if (p[i]) return 0;
    return 1;
}

static void netuid_zero_at(BYTE *p)
{
    memset(p, 0, 64);
    /* u32 four bytes before mData is mDataLength per the serialized layout;
     * near-certain to be the same relative position in memory since the
     * save serializer mirrors the in-memory struct. */
    *(volatile uint32_t*)(p - 4) = 0;
}

/* Diagnostic wide-scan: walk the same 1 MB window and log every
 * MtNetUniqueId candidate (real or fake) with its offset-from-pBase.
 * Kept as periodic sanity output so we can confirm the layout stays stable
 * across sessions and see both hired-pawn slots populate. Zero side effects. */
#define DIAG_MAX_HITS    16           /* cap per scan to avoid log floods */
static void diag_wide_scan(BYTE *base)
{
    uint32_t hits = 0;
    log_line("diag-scan: begin walk [%p .. %p]", (void*)base, (void*)(base + NETUID_WIDE_SCAN_SIZE));
    for (uint32_t off = 0; off + 64 <= NETUID_WIDE_SCAN_SIZE; off += 4) {
        const BYTE *p = base + off;
        if (*(const uint32_t*)p != 0x00000002u) continue;
        if (*(const uint32_t*)(p + 8) != 0x01100001u) continue;
        /* Found a candidate MtNetUniqueId.mData start.  Classify and log. */
        const char *kind = (p[7] & 0x80) ? "FAKE" : "real";
        log_line("  candidate @ +0x%06X  byte[4..7]=%02X %02X %02X %02X  kind=%s",
                 (unsigned)off, p[4], p[5], p[6], p[7], kind);
        if (++hits >= DIAG_MAX_HITS) {
            log_line("  (hit cap %u reached, stopping scan)", DIAG_MAX_HITS);
            break;
        }
    }
    log_line("diag-scan: done, %u hit(s)", hits);
}

/* Seen-set helpers: log once per unique fake address per session. */
static int netuid_seen_contains(BYTE *p)
{
    for (int i = 0; i < g_netuid_seen_count; i++) if (g_netuid_seen[i] == p) return 1;
    return 0;
}
static void netuid_seen_add(BYTE *p)
{
    if (g_netuid_seen_count < NETUID_SEEN_MAX) g_netuid_seen[g_netuid_seen_count++] = p;
}

/* Scrubber state.  The scrubber ticks at ~60 Hz from hook_BLoggedOn.  These
 * counters make the hook's liveness visible in logs and cap periodic diag-scan
 * volume. */
static int      g_scrub_seen_pbase = 0;    /* proof-of-life on first *pBase != NULL */
static uint32_t g_scrub_ticks      = 0;    /* total ticks since hook started firing */
static uint32_t g_scrub_diag_scans = 0;    /* diag scans performed so far (capped) */
#define DIAG_SCAN_INTERVAL   3600u         /* ~60s at 60 Hz */
#define DIAG_SCAN_MAX_COUNT  10            /* stop diag-scans after ~10 min */

/* Core scrubber: walk the 1 MB window and zero every fake mData hit.  Handles
 * N hired pawns inherently — each fake appears as its own hit, no special
 * casing needed.  Real user IDs (byte[7] MSB clear) are not matched by
 * netuid_pattern_match and are untouched. */
static void netuid_scrub_wide(BYTE *base)
{
    /* Invalidate seen-set if *pBase has moved (save reload, session restart). */
    if (base != g_netuid_seen_base) {
        g_netuid_seen_base  = base;
        g_netuid_seen_count = 0;
    }
    for (uint32_t off = 0; off + 64 <= NETUID_WIDE_SCAN_SIZE; off += 4) {
        BYTE *p = base + off;
        if (*(const uint32_t*)p != 0x00000002u) continue;        /* hot early-out */
        if (!netuid_pattern_match(p)) continue;
        if (!netuid_seen_contains(p)) {
            log_line("netuid: fake seen at %p (+0x%06X), zeroing this and every subsequent tick",
                     (void*)p, (unsigned)off);
            netuid_seen_add(p);
        }
        netuid_zero_at(p);
    }
}

static void netuid_scrub(void)
{
    g_scrub_ticks++;
    if (!g_pawn_base_var) return;
    BYTE *base = *g_pawn_base_var;
    if (!base) return;                                  /* no save loaded yet */

    /* Proof-of-life: one-shot, first tick we see *pBase populated. */
    if (!g_scrub_seen_pbase) {
        g_scrub_seen_pbase = 1;
        log_line("scrubber: first tick with *pBase set — pBase=%p *pBase=%p (tick #%u)",
                 (void*)g_pawn_base_var, (void*)base, g_scrub_ticks);
    }

    /* Periodic diagnostic snapshot, capped.  Useful for verifying the layout
     * stays stable across sessions and observing multi-pawn hires. */
    if (g_scrub_diag_scans < DIAG_SCAN_MAX_COUNT
        && (g_scrub_ticks % DIAG_SCAN_INTERVAL) == 0)
    {
        g_scrub_diag_scans++;
        log_line("diag-scan #%u (tick #%u): MtNetUniqueId candidate dump",
                 g_scrub_diag_scans, g_scrub_ticks);
        diag_wide_scan(base);
    }

    netuid_scrub_wide(base);
}

static int __thiscall hook_BLoggedOn(void *self)
{
    netuid_scrub();
    return g_orig_BLoggedOn(self);
}

static SteamAPICall_t __thiscall hook_UploadLeaderboardScore(
    void *self, SteamLeaderboard_t board, int method, int32 score,
    const int32 *details, int cDetails)
{
    log_line("UploadLeaderboardScore(board=%llu, method=%d, score=%d, cDetails=%d)",
             (unsigned long long)board, method, score, cDetails);
    if (details && cDetails > 0) log_details("upload", details, cDetails);

    if (cDetails == 2 && details != NULL) {
        /* The game byte-swaps the low 32 bits when encoding the UGCHandle into details[0],
         * so un-swap here to recover the value the game will pass to UGCDownload. */
        UGCHandle_t this_ugc = ugc_from_encoded_details((uint32)details[0], (uint32)details[1]);
        if (g_fresh_ugc == 0) {
            g_fresh_ugc = this_ugc;
            log_line("  captured fresh UGC = 0x%016llx", (unsigned long long)g_fresh_ugc);
        }
    } else if (cDetails == 18 && details) {
        /* 18-detail upload = stat/metadata board (leader_227 et al.) carrying pawn
         * name/level/vocation/stats.  Refresh the template cache for .meta-less fallback. */
        memcpy(g_last_18_details, details, sizeof(g_last_18_details));
        g_has_18_details = 1;

        /* If a rest is pending (FileShare('0') fired, cDetails=2 uploads completed),
         * this is the first fresh-stats signal for that rest - archive now with these
         * this-rest-accurate details.  Subsequent 18-detail uploads in the cluster
         * keep refreshing g_last_18_details but don't re-archive. */
        if (g_rest_pending) {
            if (g_enable_exports) {
                archive_rest(details);
            } else {
                log_line("archive_rest: enable_exports=0, skipping pawn export");
            }
            g_rest_pending = 0;
        }
    }
    return g_orig_UploadLeaderboardScore(self, board, method, score, details, cDetails);
}

static SteamAPICall_t __thiscall hook_DownloadLeaderboardEntries(
    void *self, SteamLeaderboard_t board, ELeaderboardDataRequest req,
    int rangeStart, int rangeEnd)
{
    log_line("DownloadLeaderboardEntries(board=%llu, req=%d, [%d,%d])",
             (unsigned long long)board, req, rangeStart, rangeEnd);
    return g_orig_DownloadLeaderboardEntries(self, board, req, rangeStart, rangeEnd);
}

static SteamAPICall_t __thiscall hook_DownloadLeaderboardEntriesForUsers(
    void *self, SteamLeaderboard_t board, CSteamID *users, int cUsers)
{
    log_line("DownloadLeaderboardEntriesForUsers(board=%llu, cUsers=%d, first=%llu)",
             (unsigned long long)board, cUsers,
             cUsers > 0 && users ? (unsigned long long)users[0] : 0ULL);

    /* Snag the caller's own steamID - the game asks its own stats first. */
    if (cUsers == 1 && users && g_captured_self == 0) {
        g_captured_self = users[0];
        log_line("  captured self steamID = %llu", (unsigned long long)g_captured_self);
    }
    /* Remember what was asked for so the GDLE hook can fix gbe_fork's broken filtering. */
    if (cUsers == 1 && users) g_last_dleforusers_steamid = users[0];
    return g_orig_DownloadLeaderboardEntriesForUsers(self, board, users, cUsers);
}

static int __thiscall hook_GetDownloadedLeaderboardEntry(
    void *self, SteamLeaderboardEntries_t entries, int index,
    LeaderboardEntry_t *pEntry, int32 *pDetails, int cDetailsMax)
{
    int r = g_orig_GetDownloadedLeaderboardEntry(self, entries, index, pEntry, pDetails, cDetailsMax);

    /* DLEForUsers filter fix:
     * gbe_fork's DLEForUsers sets m_hSteamLeaderboardEntries = m_hSteamLeaderboard and doesn't
     * actually narrow to the requested user - so GDLE returns the board's top entry instead.
     * When the summon flow is in play (cDetailsMax == 2 = pawn UGC lookup) and we have a fake
     * matching the last DLEForUsers request, rewrite the entry to that fake. Ignore when the
     * last request was the real user - let gbe_fork's answer stand. */
    if (r && pEntry && pDetails && cDetailsMax >= 2 && g_last_dleforusers_steamid != 0) {
        int fake_idx = find_fake_by_steamid(g_last_dleforusers_steamid);
        if (fake_idx >= 0) {
            const struct FakePawn *fp = &g_fakes[fake_idx];
            pEntry->m_steamIDUser = fp->steamid;
            pEntry->m_nGlobalRank = 1;
            pEntry->m_nScore      = 10000 - fake_idx;
            pEntry->m_cDetails    = 2;
            pEntry->m_hUGC        = 0xffffffffffffffffULL; /* gbe_fork leaves this invalid anyway */

            uint32 d0, d1;
            ugc_to_encoded_details(fp->ugc_main, &d0, &d1);
            pDetails[0] = (int32)d0;
            pDetails[1] = (int32)d1;
            log_line("GDLE: rewrote entry to fake #%d (steamid=%llu ugc_main=0x%016llx pawn_path='%s')",
                     fake_idx, (unsigned long long)fp->steamid,
                     (unsigned long long)fp->ugc_main, fp->pawn_path);
            /* Consume the pending request so a stray GDLE on a different board doesn't
             * also get rewritten. */
            g_last_dleforusers_steamid = 0;
            return 1;
        }
    }

    if (r && pEntry) {
        log_line("GetDownloadedLeaderboardEntry(entries=%llu, idx=%d, cDetailsMax=%d) -> steamid=%llu score=%d rank=%d details=%d UGC=0x%016llx",
                 (unsigned long long)entries, index, cDetailsMax,
                 (unsigned long long)pEntry->m_steamIDUser,
                 pEntry->m_nScore, pEntry->m_nGlobalRank, pEntry->m_cDetails,
                 (unsigned long long)pEntry->m_hUGC);
        if (pDetails && pEntry->m_cDetails > 0) {
            int n = pEntry->m_cDetails < cDetailsMax ? pEntry->m_cDetails : cDetailsMax;
            log_details("get", pDetails, n);
        }
    } else {
        log_line("GetDownloadedLeaderboardEntry(entries=%llu, idx=%d) -> false",
                 (unsigned long long)entries, index);
    }
    return r;
}

/* ============================================================================
 * Hook installation: vtable slot overwrite
 * ============================================================================ */

static int patch_vtable(void *instance, int method_index, void *new_fn, void **orig_fn_out, const char *name)
{
    if (!instance) return 0;
    void ***pvtbl = (void ***)instance;
    void **vtable = *pvtbl;
    if (!vtable) {
        log_line("patch_vtable(%s): NULL vtable on instance %p", name, instance);
        return 0;
    }

    DWORD old_prot;
    if (!VirtualProtect(&vtable[method_index], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        log_line("patch_vtable(%s): VirtualProtect failed, err=%lu", name, GetLastError());
        return 0;
    }
    *orig_fn_out = vtable[method_index];
    vtable[method_index] = new_fn;
    DWORD tmp;
    VirtualProtect(&vtable[method_index], sizeof(void*), old_prot, &tmp);

    log_line("patch_vtable(%s): slot %d was %p, now %p", name, method_index,
             *orig_fn_out, new_fn);
    return 1;
}

/* ============================================================================
 * Worker thread: waits for Steam interfaces, installs hooks, seeds state
 * ============================================================================ */

typedef void* (__cdecl *fn_SteamUserStats)(void);
typedef void* (__cdecl *fn_SteamRemoteStorage)(void);
typedef void* (__cdecl *fn_SteamUtils)(void);
typedef void* (__cdecl *fn_SteamUser)(void);

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;

    HMODULE steam_api = NULL;
    for (int i = 0; i < 300 && !steam_api; i++) {
        steam_api = GetModuleHandleA("steam_api.dll");
        if (!steam_api) Sleep(100);
    }
    if (!steam_api) {
        log_line("worker: steam_api.dll never loaded, giving up");
        return 0;
    }
    log_line("worker: steam_api.dll base = %p", (void*)steam_api);

    fn_SteamUserStats     get_userstats     = (fn_SteamUserStats)    (void*)GetProcAddress(steam_api, "SteamUserStats");
    fn_SteamRemoteStorage get_remotestorage = (fn_SteamRemoteStorage)(void*)GetProcAddress(steam_api, "SteamRemoteStorage");
    fn_SteamUtils         get_utils         = (fn_SteamUtils)        (void*)GetProcAddress(steam_api, "SteamUtils");
    fn_SteamUser          get_user          = (fn_SteamUser)         (void*)GetProcAddress(steam_api, "SteamUser");
    if (!get_userstats || !get_remotestorage || !get_utils || !get_user) {
        log_line("worker: SteamUserStats=%p SteamRemoteStorage=%p SteamUtils=%p SteamUser=%p (one missing)",
                 (void*)get_userstats, (void*)get_remotestorage, (void*)get_utils, (void*)get_user);
        return 0;
    }

    void *userstats = NULL, *remotestorage = NULL, *utils = NULL, *user = NULL;
    for (int i = 0; i < 600; i++) {
        if (!userstats)     userstats     = get_userstats();
        if (!remotestorage) remotestorage = get_remotestorage();
        if (!utils)         utils         = get_utils();
        if (!user)          user          = get_user();
        if (userstats && remotestorage && utils && user) break;
        Sleep(100);
    }
    log_line("worker: userstats=%p, remotestorage=%p, utils=%p, user=%p (after poll)",
             userstats, remotestorage, utils, user);
    if (!userstats || !remotestorage || !utils || !user) {
        log_line("worker: interface(s) still NULL, aborting hook install");
        return 0;
    }
    g_steam_remotestorage = remotestorage;
    g_steam_utils         = utils;

    patch_vtable(userstats,     IUSERSTATS_FindLeaderboard,
                 (void*)hook_FindLeaderboard,
                 (void**)&g_orig_FindLeaderboard,
                 "FindLeaderboard");
    patch_vtable(userstats,     IUSERSTATS_FindOrCreateLeaderboard,
                 (void*)hook_FindOrCreateLeaderboard,
                 (void**)&g_orig_FindOrCreateLeaderboard,
                 "FindOrCreateLeaderboard");
    patch_vtable(userstats,     IUSERSTATS_DownloadLeaderboardEntries,
                 (void*)hook_DownloadLeaderboardEntries,
                 (void**)&g_orig_DownloadLeaderboardEntries,
                 "DownloadLeaderboardEntries");
    patch_vtable(userstats,     IUSERSTATS_DownloadLeaderboardEntriesForUsers,
                 (void*)hook_DownloadLeaderboardEntriesForUsers,
                 (void**)&g_orig_DownloadLeaderboardEntriesForUsers,
                 "DownloadLeaderboardEntriesForUsers");
    patch_vtable(userstats,     IUSERSTATS_GetDownloadedLeaderboardEntry,
                 (void*)hook_GetDownloadedLeaderboardEntry,
                 (void**)&g_orig_GetDownloadedLeaderboardEntry,
                 "GetDownloadedLeaderboardEntry");
    patch_vtable(userstats,     IUSERSTATS_UploadLeaderboardScore,
                 (void*)hook_UploadLeaderboardScore,
                 (void**)&g_orig_UploadLeaderboardScore,
                 "UploadLeaderboardScore");

    patch_vtable(remotestorage, IREMOTESTORAGE_FileWrite,
                 (void*)hook_FileWrite,
                 (void**)&g_orig_FileWrite,
                 "FileWrite");
    patch_vtable(remotestorage, IREMOTESTORAGE_FileShare,
                 (void*)hook_FileShare,
                 (void**)&g_orig_FileShare,
                 "FileShare");
    patch_vtable(remotestorage, IREMOTESTORAGE_UGCDownload,
                 (void*)hook_UGCDownload,
                 (void**)&g_orig_UGCDownload,
                 "UGCDownload");
    patch_vtable(remotestorage, IREMOTESTORAGE_UGCRead,
                 (void*)hook_UGCRead,
                 (void**)&g_orig_UGCRead,
                 "UGCRead");

    patch_vtable(user,          IUSER_BLoggedOn,
                 (void*)hook_BLoggedOn,
                 (void**)&g_orig_BLoggedOn,
                 "BLoggedOn");

    log_line("worker: all hooks installed");

    /* Locate DDDA's character-data base pointer by signature-scanning its .text.
     * Safe to call now - DDDA.exe code is fully mapped long before steam_api.dll.
     * The resolved variable remains NULL until the user loads a save, at which
     * point subsequent reads in write_json() will see live data. */
    discover_pawn_base();

    /* Before any injection: register a placeholder file with gbe_fork and capture the
     * UGCHandle it generates. That handle becomes g_fresh_ugc and is used as the
     * substitution target when the game issues UGCDownload for a fake archive handle -
     * so summoning archived pawns works even if the user never rests at an inn. */
    UGCHandle_t startup_ugc = acquire_fresh_ugc_via_fileshare();
    if (startup_ugc != 0) {
        g_fresh_ugc = startup_ugc;
    } else {
        log_line("worker: could not acquire fresh UGC at startup (substitution will wait for first inn rest)");
    }

    /* Seed the 18-int leader_227 cache from disk so archive_current_pawn can write .meta
     * synchronously on the very first rest of the session (before any in-session 18-detail
     * upload). Absent on a fresh install - the cache fills in on the next stat upload. */
    if (load_template_details("leader_227", g_last_18_details)) {
        g_has_18_details = 1;
        log_line("worker: seeded 18-detail cache from disk leader_227");
    }

    /* One-shot archive scan.  Current-session archives get hidden from the rift
     * anyway (pawn-dedup in-game), so the archive set is effectively frozen at
     * session start and we don't need to re-scan on every FindLeaderboard.  If
     * read_self_id_from_disk fails (first-ever run, no leaderboard files yet),
     * we defer to write_rift_boards which retries on first rift open. */
    g_captured_self = read_self_id_from_disk();
    if (g_captured_self != 0) {
        rescan_all_levels(g_captured_self);
    } else {
        log_line("worker: no self steamID recoverable at startup; deferring archive scan");
    }

    return 0;
}

/* ============================================================================
 * DllMain
 * ============================================================================ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_log_lock);
        InitializeCriticalSection(&g_fakes_lock);

        /* Capture session start time in FILETIME format for current-session archive hiding. */
        FILETIME ft_now;
        GetSystemTimeAsFileTime(&ft_now);
        g_session_start_ft = ((uint64_t)ft_now.dwHighDateTime << 32) | (uint64_t)ft_now.dwLowDateTime;

        /* Resolve our own DLL's directory (includes trailing backslash). */
        DWORD n = GetModuleFileNameA(hinstDLL, g_dll_dir, sizeof(g_dll_dir));
        if (n > 0 && n < sizeof(g_dll_dir)) {
            char *slash = strrchr(g_dll_dir, '\\');
            if (slash) *(slash + 1) = 0;
        } else {
            strcpy(g_dll_dir, ".\\");
        }

        /* Load pawndb.ini FIRST so the `logging` knob controls how the log file
         * is opened below.  load_ini's log_line calls are silent pre-open (g_log is
         * still NULL); we re-log the final config once the log is ready. */
        char ini_path[MAX_PATH];
        _snprintf(ini_path, sizeof(ini_path), "%spawndb.ini", g_dll_dir);
        load_ini(ini_path);

        /* Resolve gbe_fork's per-app save paths from %APPDATA%.  Same physical
         * location under Windows and Proton/Wine; no hardcoded usernames or
         * prefix layouts. */
        resolve_gse_paths();

        /* Log file lives next to the game EXE (matches STEAM_LOG's location).
         * logging=disabled -> skip fopen entirely; logging=truncate -> "w" (fresh
         * per session); logging=append -> "a" (history preserved). */
        if (g_log_mode != LOG_DISABLED) {
            char logpath[MAX_PATH];
            DWORD nExe = GetModuleFileNameA(NULL, logpath, sizeof(logpath));
            if (nExe > 0 && nExe < sizeof(logpath)) {
                char *slash = strrchr(logpath, '\\');
                if (slash) { *(slash + 1) = 0; strncat(logpath, "pawndb.log", sizeof(logpath) - strlen(logpath) - 1); }
                else       { strcpy(logpath, "pawndb.log"); }
            } else {
                strcpy(logpath, "pawndb.log");
            }
            const char *mode = (g_log_mode == LOG_TRUNCATE) ? "w" : "a";
            g_log = fopen(logpath, mode);
            if (g_log) {
                setvbuf(g_log, NULL, _IONBF, 0);
                log_line("=== pawndb loaded, log at '%s' (mode=%s) ===", logpath, mode);
                log_line("dll_dir    = '%s'", g_dll_dir);
                log_line("remote_dir = '%s'", g_remote_dir);
                log_line("leader_dir = '%s'", g_leader_dir);
                log_line("config: save_dir='%s' max_search_results=%d logging=%d enable_exports=%d enable_updates=%d",
                         g_save_dir_rel, g_max_search_results, (int)g_log_mode,
                         g_enable_exports, g_enable_updates);
            }
        }

        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
