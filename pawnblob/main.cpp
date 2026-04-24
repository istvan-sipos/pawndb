// pawnblob - DDDA pawn upload blob (un)packer.
//
// See NOTES.md for the full format write-up. Short version: 8192-byte outer
// container with a plaintext header, Blowfish-ECB body (u32 halves stored
// native-LE), zlib-deflate inside, XFS (MT-Framework compiled XML) inside that.
//
// Commands:
//   pawnblob inspect  <file.pawn>
//       Print outer-container fields + trial inflates on the raw body.
//
//   pawnblob unpack   <file.pawn> <out.xfs>
//       Full decrypt + inflate. Writes the decompressed XFS to out.xfs.
//
//   pawnblob xfs-head <file.xfs>
//       Print the XFS header fields and known top-level offsets.
//
//   pawnblob xfs-poke <in.pawn> <out.pawn> <xfs_offset_hex> <hex_bytes>
//       Unpack, write `hex_bytes` at `xfs_offset_hex` in the decompressed
//       XFS, then repack (deflate + SHA-1 + Blowfish + outer framing) and
//       write to out.pawn. Same input and output format; round-trip
//       invariant: unpack(xfs-poke(P, off, xfs[off..off+N])) == unpack(P).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "blowfish_tables.h"

namespace {

constexpr uint32_t kMagic      = 0x12121300u;
constexpr uint8_t  kPadByte    = 0xDDu;
constexpr size_t   kFileSize   = 8192;
constexpr size_t   kOuterHdr   = 8;
constexpr size_t   kInnerHdr   = 32;   // magic(4) + orig_len(4) + u3(4) + sha1(20)
constexpr char     kKey[]      = "nokupak amugod uznogarod";

// Blowfish with standard P/S seeds. The "Capcom variant" is that each 8-byte
// block is treated as two native-LE u32s rather than the big-endian u32s the
// Blowfish spec requires -- we handle that at the ECB boundary with bswap32s
// on entry and exit, so the core round function stays textbook.
class Blowfish {
public:
    Blowfish(const uint8_t *key, size_t keylen)
    {
        std::memcpy(P_, kBlowfishP, sizeof(P_));
        std::memcpy(S_, kBlowfishS, sizeof(S_));
        size_t j = 0;
        for (int i = 0; i < 18; ++i) {
            uint32_t x = 0;
            for (int k = 0; k < 4; ++k) {
                x = (x << 8) | key[j];
                j = (j + 1) % keylen;
            }
            P_[i] ^= x;
        }
        uint32_t L = 0, R = 0;
        for (int i = 0; i < 18; i += 2) {
            encrypt_block(L, R);
            P_[i] = L; P_[i + 1] = R;
        }
        for (int s = 0; s < 4; ++s) {
            for (int k = 0; k < 256; k += 2) {
                encrypt_block(L, R);
                S_[s][k] = L; S_[s][k + 1] = R;
            }
        }
    }

    // Decrypt an ECB buffer using the Capcom native-LE block convention:
    // block halves are loaded/stored as native-endian u32 (so on an x86
    // little-endian build, bytes [0..3] become xL = u32 LE). The round
    // function itself is standard, no byte swap needed anywhere.
    void decrypt_ecb_capcom(const uint8_t *in, uint8_t *out, size_t len) const
    {
        for (size_t i = 0; i < len; i += 8) {
            uint32_t L, R;
            std::memcpy(&L, in + i, 4);
            std::memcpy(&R, in + i + 4, 4);
            decrypt_block(L, R);
            std::memcpy(out + i, &L, 4);
            std::memcpy(out + i + 4, &R, 4);
        }
    }
    void encrypt_ecb_capcom(const uint8_t *in, uint8_t *out, size_t len) const
    {
        for (size_t i = 0; i < len; i += 8) {
            uint32_t L, R;
            std::memcpy(&L, in + i, 4);
            std::memcpy(&R, in + i + 4, 4);
            encrypt_block(L, R);
            std::memcpy(out + i, &L, 4);
            std::memcpy(out + i + 4, &R, 4);
        }
    }

private:
    uint32_t P_[18];
    uint32_t S_[4][256];

    inline uint32_t F(uint32_t x) const
    {
        return ((S_[0][x >> 24] + S_[1][(x >> 16) & 0xff]) ^ S_[2][(x >> 8) & 0xff])
               + S_[3][x & 0xff];
    }
    void encrypt_block(uint32_t &L, uint32_t &R) const
    {
        for (int i = 0; i < 16; ++i) {
            L ^= P_[i];
            R ^= F(L);
            uint32_t t = L; L = R; R = t;
        }
        uint32_t t = L; L = R; R = t;
        R ^= P_[16]; L ^= P_[17];
    }
    void decrypt_block(uint32_t &L, uint32_t &R) const
    {
        for (int i = 17; i > 1; --i) {
            L ^= P_[i];
            R ^= F(L);
            uint32_t t = L; L = R; R = t;
        }
        uint32_t t = L; L = R; R = t;
        R ^= P_[1]; L ^= P_[0];
    }
};

// Minimal SHA-1 (standard, no Capcom twist -- the IV-XOR-0xBEEFF00D obfuscation
// the game uses only affects the in-memory constants at DAT_0162A7C0; the
// round function and initial state after unmasking match textbook SHA-1).
class Sha1 {
public:
    Sha1() { reset(); }
    void reset()
    {
        s_[0]=0x67452301u; s_[1]=0xEFCDAB89u; s_[2]=0x98BADCFEu;
        s_[3]=0x10325476u; s_[4]=0xC3D2E1F0u;
        bit_count_ = 0; buf_len_ = 0;
    }
    void update(const uint8_t *data, size_t len)
    {
        while (len > 0) {
            size_t copy = 64 - buf_len_;
            if (copy > len) copy = len;
            std::memcpy(buf_ + buf_len_, data, copy);
            buf_len_ += copy;
            data += copy;
            len  -= copy;
            bit_count_ += (uint64_t)copy * 8;
            if (buf_len_ == 64) { process(buf_); buf_len_ = 0; }
        }
    }
    void finalize(uint8_t out[20])
    {
        uint64_t bits = bit_count_;
        uint8_t one = 0x80;  update(&one, 1);
        uint8_t z = 0;
        while (buf_len_ != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - i*8));
        update(lb, 8);
        for (int i = 0; i < 5; ++i) {
            out[i*4+0] = (uint8_t)(s_[i] >> 24);
            out[i*4+1] = (uint8_t)(s_[i] >> 16);
            out[i*4+2] = (uint8_t)(s_[i] >> 8);
            out[i*4+3] = (uint8_t)(s_[i]);
        }
    }
private:
    uint32_t s_[5];
    uint64_t bit_count_;
    uint8_t  buf_[64];
    size_t   buf_len_;

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void process(const uint8_t b[64])
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)b[i*4]   << 24) | ((uint32_t)b[i*4+1] << 16)
                 | ((uint32_t)b[i*4+2] << 8)  |  (uint32_t)b[i*4+3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a=s_[0], bb=s_[1], c=s_[2], d=s_[3], e=s_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (bb & c) | ((~bb) & d);       k = 0x5A827999u; }
            else if (i < 40) { f = bb ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = bb ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(bb, 30); bb = a; a = t;
        }
        s_[0]+=a; s_[1]+=bb; s_[2]+=c; s_[3]+=d; s_[4]+=e;
    }
};

// Byte-swap each 4-byte group in place. The game stores SHA-1 digests in
// this "word-wise little-endian" form because it memcpys the five u32 state
// words straight out without byte-swapping them to big-endian first.
void bswap32_each(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint8_t t0 = buf[i],   t1 = buf[i+1];
        buf[i]   = buf[i+3];   buf[i+1] = buf[i+2];
        buf[i+2] = t1;         buf[i+3] = t0;
    }
}

bool read_all(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n);
    bool ok = std::fread(out.data(), 1, n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

bool write_all(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return false; }
    size_t w = std::fwrite(data, 1, len, f);
    std::fclose(f);
    return w == len;
}

int cmd_inspect(const char *path)
{
    std::vector<uint8_t> buf;
    if (!read_all(path, buf)) return 1;
    if (buf.size() != kFileSize) {
        std::fprintf(stderr, "unexpected size %zu (want %zu)\n", buf.size(), kFileSize);
        return 1;
    }
    uint32_t magic, body_len;
    std::memcpy(&magic,    buf.data() + 0, 4);
    std::memcpy(&body_len, buf.data() + 4, 4);
    std::printf("file       : %s\n", path);
    std::printf("magic      : 0x%08x (%s)\n", magic,
                magic == kMagic ? "OK" : "MISMATCH");
    std::printf("body_size  : %u\n", body_len);
    std::printf("tail_pad   : %zu bytes of 0x%02x\n",
                kFileSize - kOuterHdr - body_len, kPadByte);
    return 0;
}

int cmd_unpack(const char *in_path, const char *out_path)
{
    std::vector<uint8_t> in;
    if (!read_all(in_path, in)) return 1;
    if (in.size() != kFileSize) {
        std::fprintf(stderr, "%s: unexpected size %zu (want %zu)\n", in_path, in.size(), kFileSize);
        return 1;
    }
    uint32_t magic, body_len;
    std::memcpy(&magic,    in.data() + 0, 4);
    std::memcpy(&body_len, in.data() + 4, 4);
    if (magic != kMagic) {
        std::fprintf(stderr, "%s: bad magic 0x%08x (want 0x%08x)\n", in_path, magic, kMagic);
        return 1;
    }
    if (body_len > kFileSize - kOuterHdr || (body_len % 8) != 0) {
        std::fprintf(stderr, "%s: bad body_size %u\n", in_path, body_len);
        return 1;
    }

    // Blowfish-ECB decrypt body → plaintext (same length).
    Blowfish bf((const uint8_t *)kKey, sizeof(kKey) - 1);
    std::vector<uint8_t> plain(body_len);
    bf.decrypt_ecb_capcom(in.data() + kOuterHdr, plain.data(), body_len);

    uint32_t inner_magic, orig_len;
    std::memcpy(&inner_magic, plain.data() + 0, 4);
    std::memcpy(&orig_len,    plain.data() + 4, 4);
    if (inner_magic != kMagic) {
        std::fprintf(stderr, "%s: inner magic mismatch (got 0x%08x); decrypt wrong?\n",
                     in_path, inner_magic);
        return 1;
    }
    if (orig_len + kInnerHdr > body_len) {
        std::fprintf(stderr, "%s: inner orig_len %u overflows body %u\n",
                     in_path, orig_len, body_len);
        return 1;
    }

    // Inflate wbits=15 (zlib-wrapped). We inflate the unaligned orig_len; the
    // 0..7 slack bytes between orig_len and the aligned body tail aren't part
    // of the deflate stream.
    std::vector<uint8_t> xfs(256 * 1024);
    z_stream z{};
    if (inflateInit2(&z, 15) != Z_OK) {
        std::fprintf(stderr, "inflateInit2 failed\n");
        return 1;
    }
    z.next_in   = plain.data() + kInnerHdr;
    z.avail_in  = orig_len;
    z.next_out  = xfs.data();
    z.avail_out = (uInt)xfs.size();
    int rc = inflate(&z, Z_FINISH);
    size_t produced = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END) {
        std::fprintf(stderr, "inflate rc=%d (produced %zu bytes)\n", rc, produced);
        return 1;
    }
    xfs.resize(produced);

    if (!write_all(out_path, xfs.data(), xfs.size())) return 1;
    std::printf("unpacked %s → %s (%zu bytes)\n", in_path, out_path, xfs.size());

    // Quick sanity: the XFS should start with "XFS\0".
    if (xfs.size() < 4 || std::memcmp(xfs.data(), "XFS\0", 4) != 0) {
        std::fprintf(stderr, "warning: output doesn't start with XFS magic\n");
    }
    return 0;
}

int cmd_xfs_head(const char *path)
{
    std::vector<uint8_t> xfs;
    if (!read_all(path, xfs)) return 1;
    if (xfs.size() < 0x40 || std::memcmp(xfs.data(), "XFS\0", 4) != 0) {
        std::fprintf(stderr, "%s: not an XFS file (size=%zu)\n", path, xfs.size());
        return 1;
    }

    // What we're confident about (all samples agree):
    //   0x00 "XFS\0"
    //   0x04 u32 version (seen: 0x00000109)
    //   0x08 u32 count_a  (seen: 21)   - likely total class descriptor count
    //   0x0C u32 count_b  (seen: 8)    - top-level record count
    //   0x10 u32 data_end (seen: 0x1DE4) - end of the core data/descriptor region
    //   0x14..0x34 = 8x u32 offsets into the file (ascending) - top-level records
    //   0x34 u32 hash   (appears to vary per file; probably a class hash)
    //   0x38 u32 struct_size (seen: 0x1578) - matches the in-memory allocation
    //                                         DDDA's FUN_00c9e700 does for the
    //                                         pawn snapshot, so likely the
    //                                         deserialized-in-memory size.
    //   0x3C u32 trailing (seen: 4)    - meaning unknown
    //
    // The proper schema walk (field names → byte offsets) is still TODO; see
    // NOTES.md for the current state of that investigation.

    uint32_t version, count_a, count_b, data_end, hash, struct_size, trail;
    std::memcpy(&version,     xfs.data() + 0x04, 4);
    std::memcpy(&count_a,     xfs.data() + 0x08, 4);
    std::memcpy(&count_b,     xfs.data() + 0x0C, 4);
    std::memcpy(&data_end,    xfs.data() + 0x10, 4);
    std::memcpy(&hash,        xfs.data() + 0x34, 4);
    std::memcpy(&struct_size, xfs.data() + 0x38, 4);
    std::memcpy(&trail,       xfs.data() + 0x3C, 4);

    std::printf("file         : %s (%zu bytes)\n", path, xfs.size());
    std::printf("version      : 0x%08x\n", version);
    std::printf("count_a      : %u\n", count_a);
    std::printf("top_records  : %u\n", count_b);
    std::printf("data_end     : 0x%05x\n", data_end);
    std::printf("hash         : 0x%08x\n", hash);
    std::printf("struct_size  : 0x%05x (in-memory pawn snapshot size)\n", struct_size);
    std::printf("trailing_u32 : %u\n", trail);

    std::printf("top-record offsets (sorted, ascending):\n");
    for (uint32_t i = 0; i < count_b && i < 8; ++i) {
        uint32_t off;
        std::memcpy(&off, xfs.data() + 0x14 + 4 * i, 4);
        std::printf("  [%u] @ 0x%05x  ", i, off);
        if (off + 4 <= xfs.size()) {
            uint16_t a, b;
            std::memcpy(&a, xfs.data() + off,     2);
            std::memcpy(&b, xfs.data() + off + 2, 2);
            std::printf("leading (u16 %5u, u16 %5u)", a, b);
        }
        std::printf("\n");
    }
    return 0;
}

// Parse "0x1234" or "1234" as hex.
bool parse_hex_u32(const char *s, uint32_t &out)
{
    if (!s || !*s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    for (; *s; ++s) {
        char c = *s;
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    out = v;
    return true;
}

bool parse_hex_bytes(const char *s, std::vector<uint8_t> &out)
{
    out.clear();
    if (!s) return false;
    // allow leading 0x for convenience
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (const char *p = s; *p; ) {
        char c1 = *p++; if (!*p) return false;
        char c2 = *p++;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int n1 = nibble(c1), n2 = nibble(c2);
        if (n1 < 0 || n2 < 0) return false;
        out.push_back((uint8_t)((n1 << 4) | n2));
    }
    return true;
}

int cmd_xfs_poke(const char *in_path, const char *out_path,
                 uint32_t xfs_offset, const std::vector<uint8_t> &patch)
{
    std::vector<uint8_t> in;
    if (!read_all(in_path, in)) return 1;
    if (in.size() != kFileSize) {
        std::fprintf(stderr, "%s: unexpected size %zu\n", in_path, in.size());
        return 1;
    }
    uint32_t magic, body_len;
    std::memcpy(&magic,    in.data() + 0, 4);
    std::memcpy(&body_len, in.data() + 4, 4);
    if (magic != kMagic || body_len > kFileSize - kOuterHdr || (body_len % 8) != 0) {
        std::fprintf(stderr, "%s: bad outer header\n", in_path);
        return 1;
    }

    Blowfish bf((const uint8_t *)kKey, sizeof(kKey) - 1);
    std::vector<uint8_t> plain(body_len);
    bf.decrypt_ecb_capcom(in.data() + kOuterHdr, plain.data(), body_len);

    uint32_t inner_magic, orig_len, u3;
    std::memcpy(&inner_magic, plain.data() + 0, 4);
    std::memcpy(&orig_len,    plain.data() + 4, 4);
    std::memcpy(&u3,          plain.data() + 8, 4);
    if (inner_magic != kMagic || orig_len + kInnerHdr > body_len) {
        std::fprintf(stderr, "%s: inner header invalid\n", in_path);
        return 1;
    }

    std::vector<uint8_t> xfs(256 * 1024);
    {
        z_stream z{};
        if (inflateInit2(&z, 15) != Z_OK) return 1;
        z.next_in   = plain.data() + kInnerHdr;
        z.avail_in  = orig_len;
        z.next_out  = xfs.data();
        z.avail_out = (uInt)xfs.size();
        int rc = inflate(&z, Z_FINISH);
        size_t produced = z.total_out;
        inflateEnd(&z);
        if (rc != Z_STREAM_END) {
            std::fprintf(stderr, "inflate rc=%d produced=%zu\n", rc, produced);
            return 1;
        }
        xfs.resize(produced);
    }

    if (xfs_offset + patch.size() > xfs.size()) {
        std::fprintf(stderr, "patch exceeds XFS size (%zu)\n", xfs.size());
        return 1;
    }
    // Record pre-patch bytes for the log line.
    std::vector<uint8_t> before(xfs.data() + xfs_offset,
                                 xfs.data() + xfs_offset + patch.size());
    std::memcpy(xfs.data() + xfs_offset, patch.data(), patch.size());

    // Re-deflate at level 9 (matches what the game uses).
    std::vector<uint8_t> compressed(xfs.size() + 64);
    uLong out_len = (uLong)compressed.size();
    int rc = compress2(compressed.data(), &out_len, xfs.data(), (uLong)xfs.size(), 9);
    if (rc != Z_OK) {
        std::fprintf(stderr, "deflate rc=%d\n", rc);
        return 1;
    }
    compressed.resize(out_len);

    uint32_t new_orig_len = (uint32_t)compressed.size();
    uint32_t new_aligned  = (new_orig_len + 7u) & ~7u;
    uint32_t new_body_len = new_aligned + (uint32_t)kInnerHdr;
    if (new_body_len > kFileSize - kOuterHdr) {
        std::fprintf(stderr, "recompressed payload too large: %u bytes (max %zu)\n",
                     new_body_len, kFileSize - kOuterHdr);
        return 1;
    }

    // Build new plaintext body: magic, new_orig_len, u3, sha1, aligned payload.
    std::vector<uint8_t> new_plain(new_body_len, 0);
    std::memcpy(new_plain.data() + 0,  &kMagic,        4);
    std::memcpy(new_plain.data() + 4,  &new_orig_len,  4);
    std::memcpy(new_plain.data() + 8,  &u3,            4);
    std::memcpy(new_plain.data() + kInnerHdr, compressed.data(), compressed.size());
    // bytes [kInnerHdr + new_orig_len .. kInnerHdr + new_aligned) stay zero-padded.

    // SHA-1 over the aligned payload region. Store digest word-wise LE.
    {
        Sha1 sha;
        sha.update(new_plain.data() + kInnerHdr, new_aligned);
        uint8_t dig[20];
        sha.finalize(dig);
        bswap32_each(dig, 20);
        std::memcpy(new_plain.data() + 12, dig, 20);
    }

    // Encrypt and emit.
    std::vector<uint8_t> out(kFileSize, kPadByte);
    std::memcpy(out.data() + 0, &kMagic,       4);
    std::memcpy(out.data() + 4, &new_body_len, 4);
    bf.encrypt_ecb_capcom(new_plain.data(), out.data() + kOuterHdr, new_body_len);
    if (!write_all(out_path, out.data(), out.size())) return 1;

    // Log: pre / post patch bytes + size deltas.
    std::printf("xfs-poke: %s -> %s\n", in_path, out_path);
    std::printf("  xfs_offset = 0x%05x  patch = %zu bytes\n",
                xfs_offset, patch.size());
    std::fputs("  before:", stdout);
    for (auto b : before) std::printf(" %02x", b);
    std::fputs("\n  after :", stdout);
    for (auto b : patch)  std::printf(" %02x", b);
    std::fputs("\n", stdout);
    std::printf("  body_len: %u -> %u (deflate %u -> %u, aligned %u)\n",
                body_len, new_body_len, orig_len, new_orig_len, new_aligned);
    return 0;
}

int usage()
{
    std::fprintf(stderr,
        "pawnblob - DDDA pawn-upload blob tool\n"
        "usage:\n"
        "  pawnblob inspect  <file.pawn>\n"
        "  pawnblob unpack   <file.pawn> <out.xfs>\n"
        "  pawnblob xfs-head <file.xfs>\n"
        "  pawnblob xfs-poke <in.pawn> <out.pawn> <xfs_off_hex> <patch_hex>\n"
    );
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "inspect"  && argc == 3) return cmd_inspect(argv[2]);
    if (cmd == "unpack"   && argc == 4) return cmd_unpack(argv[2], argv[3]);
    if (cmd == "xfs-head" && argc == 3) return cmd_xfs_head(argv[2]);
    if (cmd == "xfs-poke" && argc == 6) {
        uint32_t off;
        std::vector<uint8_t> patch;
        if (!parse_hex_u32(argv[4], off)) {
            std::fprintf(stderr, "bad xfs_off_hex: %s\n", argv[4]); return 2;
        }
        if (!parse_hex_bytes(argv[5], patch) || patch.empty()) {
            std::fprintf(stderr, "bad patch_hex: %s\n", argv[5]); return 2;
        }
        return cmd_xfs_poke(argv[2], argv[3], off, patch);
    }
    return usage();
}
