# DDDA pawn upload blob — format & crypto notes

Reverse-engineered from DDDA.exe (Steam build, `FUN_00c9edd0` and friends),
corroborated by unpacking every archive in `…/load_dlls/pawndb` (38/38 OK).

## On-disk layout (fixed 8192 bytes)

```
offset 0x00  u32 magic      = 0x12121300
offset 0x04  u32 body_len   = align8(orig_len) + 0x20
offset 0x08  u8[body_len]   encrypted body (Blowfish variant, see below)
tail         u8             0xDD fill to 8192
```

`magic` and `body_len` are plaintext. Everything from offset `0x08` through
`0x08 + body_len - 1` is encrypted. The tail `0xDD` pad is literal `memset`
from `FUN_00c9edd0` — not a cipher artifact.

## Encrypted body → plaintext body (same length)

```
offset 0x00  u32  magic             = 0x12121300  (sentinel, repeated)
offset 0x04  u32  orig_len          unaligned deflate stream length
offset 0x08  u32  uncompressed_size XFS buffer size (0x3888 in all our samples)
offset 0x0c  u8[20] sha1            SHA-1 over aligned deflate payload
                                    stored word-wise LE (bytes of each u32
                                    reversed vs. standard digest output)
offset 0x20  u8[align8(orig_len)]   raw deflate stream (`78 DA …`, wbits=15)
```

Decompressing the deflate stream yields `XFS\0…` — an MT Framework compiled-
XML binary, 20480 bytes in every sample observed.

## Crypto: "Capcom Blowfish"

Textbook Blowfish **with one wart**: block halves are loaded/stored as
native-endian (little-endian on x86) u32 rather than the big-endian u32 that
standard Blowfish specifies. Equivalent pre/post-process:

```
byte-swap each 4 bytes of the buffer
 → standard Blowfish ECB (encrypt/decrypt)
 → byte-swap each 4 bytes of the output
```

- Mode: **ECB**. Short tail is zero-padded to 8 bytes and encrypted.
- Key: literal ASCII bytes of `"nokupak amugod uznogarod"` (24 bytes) —
  reversed from `"doragonzu dogma kapukon"` (Japanese romaji for
  "Dragons Dogma Capcom"). No derivation, no hashing.
- Key schedule: standard, with the key read 4 bytes at a time **big-endian**
  and wrapped (`uVar9 = (uVar9 + 4) % keylen`).
- P-array and S-boxes: **standard** Blowfish π-based constants.
  - P[0]     = `0x243F6A88`  (seed at `DAT_01629768`, file offset `0x1228368`)
  - S0[0]    = `0xD1310BA6`  (seed at `DAT_016297B0`, file offset `0x12283B0`)
- SHA-1 IVs are the standard ones XOR-masked with `0xBEEFF00D` for
  trivial obfuscation (`DAT_0162A7C0..D0`, file offset `0x12293C0`).

## Game-side call graph

```
FUN_00c9e700 (main-pawn serializer)
 ├── serialize Arisen's main pawn  →  XFS buffer at +0x1578
 ├── FUN_00e0d1a0                  →  zlib deflate level 9
 ├── size < 0x1FD8? else bail
 └── FUN_00c9edd0
      ├── FUN_00d29820              allocate crypto object
      ├── FUN_00d289c0(key_string)  store key ptr + strlen
      ├── FUN_00d29af0              Blowfish key schedule
      │      ├── copy DAT_01629768 → P[0..17]
      │      ├── copy DAT_016297B0 → S-boxes
      │      ├── XOR key into P-array (BE 4-byte chunks)
      │      └── 4 + 512 × FUN_00d28e30 (single-block encrypt) to finalize
      ├── FUN_00d29970              SHA-1 of aligned payload → ESI+0x0c
      ├── (write magic, orig_len, u3 fields into ESI header)
      ├── allocate 8192 output buffer, memset 0xDD
      └── FUN_00d296e0              Blowfish-ECB encrypt ESI → out+8
             └── per 8-byte block: copy plaintext, FUN_00d28e30(out, out+1)
      └── write outer magic + body_len at out[0..7]  (plaintext overwrite)
```

## What the blob contains — and doesn't

Field names confirmed present in the XFS schema:
`mOnlinePawnInfo`, `data.mOwnerId`, `mNetUniqueId`, `mRomPawnUniqueID`,
`OnlinePoint`, plus hundreds of pawn-stat / appearance / knowledge fields.

**The owner's SteamID is NOT serialized into the blob.** Across all 38
archives the user's account-id bytes (`0x1B42839E` in either endianness) and
the Steam universe marker `01 00 10 01` appear zero times. The pawn's owner
identity is Steam UGC metadata attached at `FileShare`/publish time, not
blob bytes — which is why zeroing the blob cannot influence what lands in
`mCmc` on a subsequent hire.

Consequence: the "zero owner SteamID on write" fix has no target. The
in-memory scrubber in `pawndb.c` remains the correct intervention layer.

## Python reference implementation

```python
from Crypto.Cipher import Blowfish
import struct, zlib

def bswap32(b):
    o = bytearray(b)
    for i in range(0, len(o), 4):
        o[i:i+4] = o[i:i+4][::-1]
    return bytes(o)

KEY = b"nokupak amugod uznogarod"

def unpack(path):
    d = open(path, "rb").read()
    assert struct.unpack("<I", d[0:4])[0] == 0x12121300
    body_len = struct.unpack("<I", d[4:8])[0]
    body = d[8:8 + body_len]
    c = Blowfish.new(KEY, Blowfish.MODE_ECB)
    plain = bswap32(c.decrypt(bswap32(body)))
    orig_len = struct.unpack("<I", plain[4:8])[0]
    return zlib.decompress(plain[32:32 + orig_len])
```
