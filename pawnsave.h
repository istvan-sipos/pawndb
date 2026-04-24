#ifndef PAWNSAVE_H
#define PAWNSAVE_H

#include <stdint.h>
#include <stddef.h>

/* One gear slot in a pawn's mEquipItem array, mirroring the save XML's
 * sItemManager::cITEM_PARAM_DATA record. See gear-persistence.md for how
 * these map to XFS offsets inside a .pawn archive. */
struct sav_gear_record {
    int16_t  mNum;
    int16_t  mItemNo;         /* -1 = empty slot */
    uint32_t mFlag;            /* bit 7 (0x80) is the runtime "equipped" marker */
    uint16_t mChgNum;
    uint16_t mDay1;
    uint16_t mDay2;
    uint16_t mDay3;
    int8_t   mMutationPool;
    int8_t   mOwnerId;         /* 0=Arisen, 1=MainPawn, 2=Hired1, 3=Hired2 */
    uint32_t mKey;
};

/* One hired-pawn snapshot extracted from DDDA.sav: creator name (which the
 * mod injects as "NNN:HEX" when the archive is written), the 12 gear records
 * tagged with this pawn's mOwnerId, and the two 322-entry knowledge arrays
 * that the .pawn archive round-trips byte-for-byte from the save XML. */
struct pawnsave_hired_info {
    int      present;          /* 1 if a matching block was found, 0 otherwise */
    char     creator_name[32]; /* NUL-terminated; up to 25 chars + NUL from mArisenName */
    struct   sav_gear_record gear[12];
    uint32_t study_flag[322];        /* mStudyFlag       — pawn's per-entry knowledge state */
    uint32_t local_study_flag[322];  /* mLocalStudyFlag  — local/instance knowledge state   */
};

/* Parse DDDA.sav once and fill both hired-pawn slots (Hired1 -> out[0],
 * Hired2 -> out[1]). Each out[i].present reflects whether a cSAVE_DATA_CMC
 * block was found whose first non-empty gear record has mOwnerId == 2 + i.
 *
 * save_bytes / save_len describe the raw save as handed to FileWrite: a
 * 32-byte header followed by zlib-compressed XML. The save is decompressed
 * into a heap buffer, scanned, and freed before this returns.
 *
 * Returns 0 on success (out[] populated, possibly with present=0 entries),
 * -1 on bad header, -2 on zlib failure. */
int pawnsave_read_hired(const void *save_bytes, int32_t save_len,
                        struct pawnsave_hired_info out[2]);

#endif
