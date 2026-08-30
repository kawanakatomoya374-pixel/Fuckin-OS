/**
 * png_decoder.c - see png_decoder.h
 */
#include "png_decoder.h"
#include "memory.h"
#include "serial.h"
#include <string.h>
#include <stdint.h>

/* ============================================================================
   DEFLATE (RFC 1951) bit reader + Huffman decode
   ========================================================================== */
typedef struct {
    const uint8_t* data;
    uint64_t size;
    uint64_t bytepos;
    uint32_t bitbuf;
    int bitcnt;
} pbits_t;

static int pbit(pbits_t* b) {
    if (b->bitcnt == 0) {
        if (b->bytepos >= b->size) return -1;
        b->bitbuf = b->data[b->bytepos++];
        b->bitcnt = 8;
    }
    int bit = (int)(b->bitbuf & 1u);
    b->bitbuf >>= 1;
    b->bitcnt--;
    return bit;
}

static int pbits(pbits_t* b, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
        int bit = pbit(b);
        if (bit < 0) return -1;
        v |= (bit << i);
    }
    return v;
}

typedef struct {
    int counts[16];
    int symbols[288];
} phuff_t;

static void phuff_build(phuff_t* h, const uint8_t* lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    int offsets[16];
    offsets[0] = 0; offsets[1] = 0;
    for (int i = 1; i < 15; ++i) offsets[i + 1] = offsets[i] + h->counts[i];
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h->symbols[offsets[lengths[i]]++] = i;
    }
}

static int phuff_decode(pbits_t* b, phuff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        int bit = pbit(b);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t plength_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  plength_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t pdist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  pdist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const uint8_t  pclen_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/* Output sink: a fixed, pre-sized buffer (the exact decompressed size is
 * known upfront from the PNG's own width/height/bit-depth, so unlike a
 * general-purpose inflate there is no need for a growable buffer here -
 * simpler and safer in a freestanding kernel with no realloc-by-doubling
 * convention). Any attempt to write past out_cap or read a back-reference
 * that isn't fully within what's been written so far is treated as a
 * corrupt/hostile stream and aborts decoding rather than reading or
 * writing out of bounds. */
typedef struct {
    uint8_t* buf;
    uint64_t pos;
    uint64_t cap;
    bool overflow;
} psink_t;

static void psink_push(psink_t* s, uint8_t v) {
    if (s->pos >= s->cap) { s->overflow = true; return; }
    s->buf[s->pos++] = v;
}

static int inflate_huffblock(pbits_t* b, psink_t* out, phuff_t* lit, phuff_t* dist) {
    for (;;) {
        if (out->overflow) return -1;
        int sym = phuff_decode(b, lit);
        if (sym < 0) return -1;
        if (sym < 256) { psink_push(out, (uint8_t)sym); continue; }
        if (sym == 256) return 0; /* end of block */
        sym -= 257;
        if (sym >= 29) return -1;
        int extra = pbits(b, plength_extra[sym]);
        if (extra < 0) return -1;
        int length = plength_base[sym] + extra;
        int dsym = phuff_decode(b, dist);
        if (dsym < 0 || dsym >= 30) return -1;
        int dextra = pbits(b, pdist_extra[dsym]);
        if (dextra < 0) return -1;
        int distance = pdist_base[dsym] + dextra;
        if ((uint64_t)distance > out->pos) return -1; /* back-ref before start of stream */
        uint64_t start = out->pos - (uint64_t)distance;
        for (int i = 0; i < length; ++i) {
            if (out->overflow) return -1;
            psink_push(out, out->buf[start + (uint64_t)i]);
        }
    }
}

static void build_fixed_tables(phuff_t* lit, phuff_t* dist) {
    uint8_t lens[288];
    for (int i = 0; i < 144; ++i) lens[i] = 8;
    for (int i = 144; i < 256; ++i) lens[i] = 9;
    for (int i = 256; i < 280; ++i) lens[i] = 7;
    for (int i = 280; i < 288; ++i) lens[i] = 8;
    phuff_build(lit, lens, 288);
    uint8_t dlens[30];
    for (int i = 0; i < 30; ++i) dlens[i] = 5;
    phuff_build(dist, dlens, 30);
}

static int inflate_dynamic_tables(pbits_t* b, phuff_t* lit, phuff_t* dist) {
    int hlit_v = pbits(b, 5); if (hlit_v < 0) return -1; int hlit = hlit_v + 257;
    int hdist_v = pbits(b, 5); if (hdist_v < 0) return -1; int hdist = hdist_v + 1;
    int hclen_v = pbits(b, 4); if (hclen_v < 0) return -1; int hclen = hclen_v + 4;

    uint8_t clens[19] = {0};
    for (int i = 0; i < hclen; ++i) {
        int v = pbits(b, 3);
        if (v < 0) return -1;
        clens[pclen_order[i]] = (uint8_t)v;
    }
    phuff_t clh;
    phuff_build(&clh, clens, 19);

    uint8_t lengths[320] = {0};
    int n = 0;
    while (n < hlit + hdist && n < 320) {
        int sym = phuff_decode(b, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            int rep_v = pbits(b, 2); if (rep_v < 0) return -1;
            int rep = rep_v + 3;
            uint8_t prev = (n > 0) ? lengths[n - 1] : 0;
            while (rep-- > 0 && n < 320) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep_v = pbits(b, 3); if (rep_v < 0) return -1;
            int rep = rep_v + 3;
            while (rep-- > 0 && n < 320) lengths[n++] = 0;
        } else {
            int rep_v = pbits(b, 7); if (rep_v < 0) return -1;
            int rep = rep_v + 11;
            while (rep-- > 0 && n < 320) lengths[n++] = 0;
        }
    }
    if (n < hlit + hdist) return -1;
    phuff_build(lit, lengths, hlit);
    phuff_build(dist, lengths + hlit, hdist);
    return 0;
}

/* zlib (RFC 1950) wrapper: 2-byte header, DEFLATE stream, 4-byte Adler32
 * (not verified - a corrupt/truncated stream is already caught by the
 * bounds checks throughout the inflate itself). */
static bool zlib_inflate(const uint8_t* zdata, uint64_t zsize, uint8_t* out, uint64_t out_size) {
    if (zsize < 2) return false;
    pbits_t b = {0};
    b.data = zdata + 2;
    b.size = zsize - 2;
    psink_t sink = {0};
    sink.buf = out;
    sink.cap = out_size;

    int final_block;
    do {
        final_block = pbit(&b);
        if (final_block < 0) return false;
        int type = pbits(&b, 2);
        if (type < 0) return false;

        if (type == 0) {
            /* Stored (uncompressed) block: align to byte boundary. */
            b.bitcnt = 0;
            if (b.bytepos + 4 > b.size) return false;
            uint32_t len = (uint32_t)b.data[b.bytepos] | ((uint32_t)b.data[b.bytepos + 1] << 8);
            b.bytepos += 4; /* len + ~len */
            if (b.bytepos + len > b.size) return false;
            for (uint32_t i = 0; i < len; ++i) psink_push(&sink, b.data[b.bytepos++]);
            if (sink.overflow) return false;
        } else if (type == 1) {
            phuff_t lit, dist;
            build_fixed_tables(&lit, &dist);
            if (inflate_huffblock(&b, &sink, &lit, &dist) < 0) return false;
        } else if (type == 2) {
            phuff_t lit, dist;
            if (inflate_dynamic_tables(&b, &lit, &dist) < 0) return false;
            if (inflate_huffblock(&b, &sink, &lit, &dist) < 0) return false;
        } else {
            return false; /* type 3 is reserved/invalid */
        }
    } while (!final_block);

    return sink.pos == out_size; /* must have produced exactly what we expected */
}

/* See the declaration in png_decoder.h. gzip's envelope, unlike zlib's,
 * has a variable length: a 10-byte fixed header followed by optional
 * fields selected by bits in the FLG byte (RFC 1952 section 2.3.1). */
bool gzip_inflate(const uint8_t* gzdata, uint64_t gzsize, uint8_t* out,
                  uint64_t out_cap, uint64_t* out_len) {
    /* Smallest possible well-formed stream: 10-byte header + a 5-byte
     * empty stored/type-0 deflate block + 8-byte CRC32+ISIZE trailer. */
    if (gzsize < 23 || out == NULL || out_cap == 0) return false;
    if (gzdata[0] != 0x1f || gzdata[1] != 0x8b) return false; /* not gzip */
    if (gzdata[2] != 8) return false; /* CM: 8 = deflate, the only method gzip defines in practice */
    uint8_t flg = gzdata[3];
    uint64_t pos = 10; /* ID1 ID2 CM FLG MTIME(4) XFL OS */

    if (flg & 0x04) { /* FEXTRA */
        if (pos + 2 > gzsize) return false;
        uint32_t xlen = (uint32_t)gzdata[pos] | ((uint32_t)gzdata[pos + 1] << 8);
        pos += 2;
        if (pos + xlen > gzsize) return false;
        pos += xlen;
    }
    if (flg & 0x08) { /* FNAME: NUL-terminated original filename */
        while (pos < gzsize && gzdata[pos] != 0) pos++;
        if (pos >= gzsize) return false;
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT: NUL-terminated */
        while (pos < gzsize && gzdata[pos] != 0) pos++;
        if (pos >= gzsize) return false;
        pos++;
    }
    if (flg & 0x02) { /* FHCRC: 2-byte CRC16 of the header just parsed, not verified */
        if (pos + 2 > gzsize) return false;
        pos += 2;
    }
    if (pos >= gzsize) return false;

    /* The 8-byte CRC32+ISIZE trailer that follows the compressed payload is
     * deliberately never consumed here: the loop below stops as soon as the
     * bitstream's own end-of-block marker says so (matching zlib_inflate's
     * same choice not to verify Adler32), so those trailer bytes simply sit
     * unread past the end of what pbits_t looks at. */
    pbits_t b = {0};
    b.data = gzdata + pos;
    b.size = gzsize - pos;
    psink_t sink = {0};
    sink.buf = out;
    sink.cap = out_cap;

    int final_block;
    do {
        final_block = pbit(&b);
        if (final_block < 0) return false;
        int type = pbits(&b, 2);
        if (type < 0) return false;

        if (type == 0) {
            b.bitcnt = 0;
            if (b.bytepos + 4 > b.size) return false;
            uint32_t len = (uint32_t)b.data[b.bytepos] | ((uint32_t)b.data[b.bytepos + 1] << 8);
            b.bytepos += 4; /* len + one's-complement check, neither verified */
            if (b.bytepos + len > b.size) return false;
            for (uint32_t i = 0; i < len; ++i) psink_push(&sink, b.data[b.bytepos++]);
            if (sink.overflow) return false;
        } else if (type == 1) {
            phuff_t lit, dist;
            build_fixed_tables(&lit, &dist);
            if (inflate_huffblock(&b, &sink, &lit, &dist) < 0) return false;
        } else if (type == 2) {
            phuff_t lit, dist;
            if (inflate_dynamic_tables(&b, &lit, &dist) < 0) return false;
            if (inflate_huffblock(&b, &sink, &lit, &dist) < 0) return false;
        } else {
            return false; /* type 3 is reserved/invalid */
        }
    } while (!final_block);

    if (out_len) *out_len = sink.pos;
    return true;
}

/* ============================================================================
   PNG filtering + chunk parsing
   ========================================================================== */
static int paeth_predict(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint32_t png_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool png_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                 uint64_t max_out_w, uint64_t max_out_h,
                 uint64_t* out_w, uint64_t* out_h) {
    static const uint8_t sig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    if (size < 8 + 25 || memcmp(data, sig, 8) != 0) return false;

    uint64_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t palette[256 * 3];
    int pal_n = 0;
    uint8_t trns[256];
    int trns_n = 0;
    bool have_ihdr = false;

    uint8_t* idat = NULL;
    uint64_t idat_len = 0, idat_cap = 0;

    while (pos + 12 <= size) {
        uint32_t clen = png_be32(&data[pos]);
        const uint8_t* ctype = &data[pos + 4];
        if (pos + 8 + (uint64_t)clen + 4 > size) break; /* truncated chunk */
        const uint8_t* cdata = &data[pos + 8];

        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            width = png_be32(cdata);
            height = png_be32(cdata + 4);
            bitdepth = cdata[8];
            colortype = cdata[9];
            interlace = cdata[12];
            have_ihdr = true;
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            pal_n = (int)(clen / 3);
            if (pal_n > 256) pal_n = 256;
            memcpy(palette, cdata, (size_t)pal_n * 3);
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            trns_n = (int)clen;
            if (trns_n > 256) trns_n = 256;
            memcpy(trns, cdata, (size_t)trns_n);
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            uint64_t new_len = idat_len + clen;
            if (new_len > idat_cap) {
                uint64_t new_cap = new_len + (new_len / 2) + 4096;
                uint8_t* grown = (uint8_t*)kmalloc((size_t)new_cap);
                if (!grown) { if (idat) kfree(idat); return false; }
                if (idat) { memcpy(grown, idat, (size_t)idat_len); kfree(idat); }
                idat = grown;
                idat_cap = new_cap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len = new_len;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos += 8 + (uint64_t)clen + 4;
    }

    if (!have_ihdr || width == 0 || height == 0 || !idat || idat_len == 0) {
        if (idat) kfree(idat);
        serial_puts("[PNG] Missing/invalid IHDR or IDAT\n");
        return false;
    }
    if (interlace != 0) {
        kfree(idat);
        serial_puts("[PNG] Adam7-interlaced PNGs are not supported\n");
        return false;
    }
    if (bitdepth != 1 && bitdepth != 2 && bitdepth != 4 && bitdepth != 8 && bitdepth != 16) {
        kfree(idat);
        serial_puts("[PNG] Unsupported bit depth\n");
        return false;
    }
    int channels;
    switch (colortype) {
        case 0: channels = 1; break; /* gray */
        case 2: channels = 3; break; /* RGB */
        case 3: channels = 1; break; /* palette */
        case 4: channels = 2; break; /* gray+alpha */
        case 6: channels = 4; break; /* RGBA */
        default:
            kfree(idat);
            serial_puts("[PNG] Unsupported color type\n");
            return false;
    }
    if (colortype == 3 && pal_n == 0) {
        kfree(idat);
        serial_puts("[PNG] Palette color type with no PLTE chunk\n");
        return false;
    }

    uint64_t stride = ((uint64_t)width * (uint64_t)channels * bitdepth + 7ULL) / 8ULL;
    uint64_t raw_size = (stride + 1ULL) * (uint64_t)height; /* +1 per row for the filter-type byte */

    /* Guard against absurd/hostile headers before committing to a big
     * allocation - also doubles as our real supported-size ceiling. */
    if (width > 20000 || height > 20000 || raw_size > (64ULL * 1024ULL * 1024ULL)) {
        kfree(idat);
        serial_puts("[PNG] Image too large\n");
        return false;
    }

    uint8_t* raw = (uint8_t*)kmalloc((size_t)raw_size);
    if (!raw) { kfree(idat); return false; }

    bool ok = zlib_inflate(idat, idat_len, raw, raw_size);
    kfree(idat);
    if (!ok) {
        kfree(raw);
        serial_puts("[PNG] DEFLATE decompression failed (corrupt or truncated data)\n");
        return false;
    }

    /* Un-filter each scanline in place. bpp is measured in whole bytes,
     * rounded up - for sub-byte bit depths (1/2/4-bit) this correctly
     * becomes 1, meaning the Sub/Up/Paeth predictors reference the
     * previous whole byte rather than the previous sample, exactly as
     * the PNG spec requires. */
    int bpp = (int)((channels * bitdepth + 7) / 8);
    if (bpp < 1) bpp = 1;
    uint8_t* pixels = (uint8_t*)kmalloc((size_t)stride * height);
    if (!pixels) { kfree(raw); return false; }

    uint8_t* prev_row = (uint8_t*)kmalloc((size_t)stride);
    if (!prev_row) { kfree(raw); kfree(pixels); return false; }
    memset(prev_row, 0, (size_t)stride);

    uint64_t rp = 0;
    bool filter_ok = true;
    for (uint32_t y = 0; y < height && filter_ok; ++y) {
        uint8_t filter = raw[rp++];
        uint8_t* line = pixels + (uint64_t)y * stride;
        memcpy(line, raw + rp, (size_t)stride);
        rp += stride;

        for (uint64_t x = 0; x < stride; ++x) {
            int a = (x >= (uint64_t)bpp) ? line[x - bpp] : 0;
            int b = prev_row[x];
            int c = (x >= (uint64_t)bpp) ? prev_row[x - bpp] : 0;
            int v = line[x];
            switch (filter) {
                case 0: break;
                case 1: v = (v + a) & 0xFF; break;
                case 2: v = (v + b) & 0xFF; break;
                case 3: v = (v + ((a + b) / 2)) & 0xFF; break;
                case 4: v = (v + paeth_predict(a, b, c)) & 0xFF; break;
                default: filter_ok = false; break;
            }
            line[x] = (uint8_t)v;
        }
        memcpy(prev_row, line, (size_t)stride);
    }
    kfree(raw);
    kfree(prev_row);

    if (!filter_ok) {
        kfree(pixels);
        serial_puts("[PNG] Unknown scanline filter type (corrupt data)\n");
        return false;
    }

    /* Convert to BGRA in out_bgra, clamped to the caller's max dimensions
     * and densely packed (stride = actual output width), matching
     * decode_bmp()'s convention in jpeg_viewer.c. */
    uint64_t ow = width < max_out_w ? width : max_out_w;
    uint64_t oh = height < max_out_h ? height : max_out_h;

    for (uint64_t y = 0; y < oh; ++y) {
        const uint8_t* line = pixels + y * stride;
        uint8_t* dst_row = out_bgra + y * ow * 4;
        for (uint64_t x = 0; x < ow; ++x) {
            int r, g, bch, a = 255;

            if (bitdepth == 8) {
                const uint8_t* px = line + x * channels;
                if (colortype == 0) { r = g = bch = px[0]; }
                else if (colortype == 2) { r = px[0]; g = px[1]; bch = px[2]; }
                else if (colortype == 3) {
                    int idx = px[0];
                    if (idx >= pal_n) idx = 0;
                    r = palette[idx * 3]; g = palette[idx * 3 + 1]; bch = palette[idx * 3 + 2];
                    a = (idx < trns_n) ? trns[idx] : 255;
                } else if (colortype == 4) { r = g = bch = px[0]; a = px[1]; }
                else { r = px[0]; g = px[1]; bch = px[2]; a = px[3]; } /* colortype 6 */
            } else if (bitdepth == 16) {
                /* Take the high byte of each 16-bit big-endian sample. */
                const uint8_t* px = line + x * channels * 2;
                if (colortype == 0) { r = g = bch = px[0]; }
                else if (colortype == 2) { r = px[0]; g = px[2]; bch = px[4]; }
                else if (colortype == 4) { r = g = bch = px[0]; a = px[2]; }
                else { r = px[0]; g = px[2]; bch = px[4]; a = px[6]; }
            } else {
                /* 1/2/4-bit: grayscale or palette index packed multiple
                 * samples per byte, MSB first. */
                uint64_t bit_off = x * (uint64_t)bitdepth;
                uint64_t byte_off = bit_off / 8;
                int shift = 8 - bitdepth - (int)(bit_off % 8);
                int mask = (1 << bitdepth) - 1;
                int sample = (line[byte_off] >> shift) & mask;
                if (colortype == 3) {
                    int idx = sample;
                    if (idx >= pal_n) idx = 0;
                    r = palette[idx * 3]; g = palette[idx * 3 + 1]; bch = palette[idx * 3 + 2];
                    a = (idx < trns_n) ? trns[idx] : 255;
                } else {
                    /* Grayscale: scale sample up to 0-255. */
                    int maxval = mask;
                    r = g = bch = (maxval > 0) ? (sample * 255 / maxval) : 0;
                }
            }

            dst_row[x * 4 + 0] = (uint8_t)bch;
            dst_row[x * 4 + 1] = (uint8_t)g;
            dst_row[x * 4 + 2] = (uint8_t)r;
            dst_row[x * 4 + 3] = (uint8_t)a;
        }
    }

    kfree(pixels);
    *out_w = ow;
    *out_h = oh;
    return true;
}
