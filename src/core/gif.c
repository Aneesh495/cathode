/* ==========================================================================
 * gif.c  -  from-scratch animated GIF89a encoder (no external libraries).
 *
 * Implements the two non-trivial pieces itself:
 *   1. Color quantization: a fixed 6x7x6 = 252-entry RGB palette (a slightly
 *      green-weighted "web-safe"-style cube). A fixed palette shared by all
 *      frames keeps the animation stable (no palette flicker) and lets us map
 *      a pixel to its index with pure arithmetic  -  no per-frame clustering.
 *   2. GIF-LZW compression: variable-width codes, a hash-chained string table,
 *      Clear / End-of-Information codes, code-width growth, and the packed
 *      sub-block bitstream  -  the actual GIF LZW variant.
 *
 * References: GIF89a spec (CompuServe, 1990); the LZW-for-GIF appendix.
 * ========================================================================== */
#include "cathode/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- fixed palette: 6 levels R, 7 levels G, 6 levels B = 252 colors ---- */
#define PR 6
#define PG 7
#define PB 6
#define PAL_N (PR*PG*PB)   /* 252 */

struct GifWriter {
    FILE *f;
    i32 w, h;
    i32 delay_cs;       /* inter-frame delay, centiseconds */
    u8 palette[256*3];
    int ok;
};

static inline int quant_channel(f32 v, int levels){
    /* v is 0..1 (already tonemapped); map to a palette level with rounding */
    int q = (int)(v*(levels-1) + 0.5f);
    if (q<0) q=0; if (q>levels-1) q=levels-1;
    return q;
}

static void build_palette(u8 *pal){
    int idx=0;
    for (int r=0;r<PR;++r)
      for (int g=0;g<PG;++g)
        for (int b=0;b<PB;++b){
            pal[idx*3+0]=(u8)(r*255/(PR-1));
            pal[idx*3+1]=(u8)(g*255/(PG-1));
            pal[idx*3+2]=(u8)(b*255/(PB-1));
            ++idx;
        }
    /* pad the rest of the 256-entry table to black */
    for (; idx<256; ++idx){ pal[idx*3+0]=pal[idx*3+1]=pal[idx*3+2]=0; }
}

static inline u8 rgb_to_index(f32 r, f32 g, f32 b){
    int ri=quant_channel(r,PR), gi=quant_channel(g,PG), bi=quant_channel(b,PB);
    return (u8)((ri*PG + gi)*PB + bi);
}

/* ---- little-endian helpers ---- */
static void put_u8(FILE*f,u8 v){ fputc(v,f); }
static void put_u16(FILE*f,u16 v){ fputc(v&0xff,f); fputc((v>>8)&0xff,f); }

/* ============================ LZW bitstream ============================ *
 * GIF packs LZW codes LSB-first into a byte stream, which is then chopped into
 * sub-blocks of <=255 bytes each (length-prefixed). */
typedef struct {
    FILE *f;
    u8   block[255];
    int  block_len;
    u32  bitbuf;
    int  bitcnt;
} BitWriter;

static void bw_flush_block(BitWriter*b){
    if (b->block_len>0){
        put_u8(b->f,(u8)b->block_len);
        fwrite(b->block,1,b->block_len,b->f);
        b->block_len=0;
    }
}
static void bw_emit_byte(BitWriter*b,u8 v){
    b->block[b->block_len++]=v;
    if (b->block_len==255) bw_flush_block(b);
}
static void bw_put_code(BitWriter*b,int code,int width){
    b->bitbuf |= ((u32)code) << b->bitcnt;
    b->bitcnt += width;
    while (b->bitcnt>=8){
        bw_emit_byte(b,(u8)(b->bitbuf&0xff));
        b->bitbuf>>=8;
        b->bitcnt-=8;
    }
}
static void bw_finish(BitWriter*b){
    if (b->bitcnt>0){ bw_emit_byte(b,(u8)(b->bitbuf&0xff)); b->bitbuf=0; b->bitcnt=0; }
    bw_flush_block(b);
    put_u8(b->f,0);   /* block terminator */
}

/* LZW string table via a hash of (prefix_code<<8 | next_byte). */
#define LZW_HASH_SIZE 5003
static void lzw_compress(FILE*f, const u8*indices, i32 npix, int min_code_bits){
    BitWriter bw; memset(&bw,0,sizeof(bw)); bw.f=f;
    int clear_code = 1<<min_code_bits;
    int eoi_code   = clear_code+1;
    int code_width = min_code_bits+1;
    int next_code  = eoi_code+1;

    /* hash table: maps key -> code, -1 if empty */
    static int hcode[LZW_HASH_SIZE];
    static long hkey[LZW_HASH_SIZE];
    for (int i=0;i<LZW_HASH_SIZE;++i){ hcode[i]=-1; hkey[i]=-1; }

    put_u8(f,(u8)min_code_bits);   /* LZW minimum code size byte */
    bw_put_code(&bw, clear_code, code_width);

    int prefix = indices[0];
    for (i32 i=1;i<npix;++i){
        int k = indices[i];
        long key = ((long)prefix<<8) | (unsigned)k;
        /* hash probe */
        int hp = (int)(( (key>>12) ^ key ) % LZW_HASH_SIZE);
        if (hp<0) hp+=LZW_HASH_SIZE;
        int step = (hp==0)?1:(LZW_HASH_SIZE-hp);
        int found=-1;
        for (;;){
            if (hcode[hp]==-1) break;
            if (hkey[hp]==key){ found=hcode[hp]; break; }
            hp-=step; if (hp<0) hp+=LZW_HASH_SIZE;
        }
        if (found>=0){ prefix=found; continue; }
        /* emit prefix, add new string */
        bw_put_code(&bw, prefix, code_width);
        if (next_code < 4096){
            hcode[hp]=next_code; hkey[hp]=key;
            next_code++;
            if (next_code > (1<<code_width) && code_width<12) code_width++;
        } else {
            /* table full: emit clear and reset */
            bw_put_code(&bw, clear_code, code_width);
            for (int j=0;j<LZW_HASH_SIZE;++j){ hcode[j]=-1; hkey[j]=-1; }
            code_width = min_code_bits+1;
            next_code = eoi_code+1;
        }
        prefix = k;
    }
    bw_put_code(&bw, prefix, code_width);
    bw_put_code(&bw, eoi_code, code_width);
    bw_finish(&bw);
}

/* ============================ public API ============================ */
GifWriter *gif_begin(const char *path, i32 w, i32 h, i32 delay_cs, int loop){
    GifWriter *g=(GifWriter*)calloc(1,sizeof(GifWriter));
    if (!g) return NULL;
    g->f=fopen(path,"wb");
    if (!g->f){ free(g); return NULL; }
    g->w=w; g->h=h; g->ok=1;
    g->delay_cs = delay_cs>0 ? delay_cs : 4;
    build_palette(g->palette);

    /* header */
    fwrite("GIF89a",1,6,g->f);
    /* logical screen descriptor: w,h, packed(global color table, 8-bit, 256), bg, aspect */
    put_u16(g->f,(u16)w); put_u16(g->f,(u16)h);
    put_u8(g->f, 0xF7);        /* GCT present, color res 8, 2^(7+1)=256 entries */
    put_u8(g->f, 0);           /* background color index */
    put_u8(g->f, 0);           /* pixel aspect ratio */
    /* global color table (256*3) */
    fwrite(g->palette,1,256*3,g->f);

    /* NETSCAPE2.0 looping extension */
    if (loop){
        put_u8(g->f,0x21); put_u8(g->f,0xFF); put_u8(g->f,11);
        fwrite("NETSCAPE2.0",1,11,g->f);
        put_u8(g->f,3); put_u8(g->f,1);
        put_u16(g->f,0);          /* loop count 0 = forever */
        put_u8(g->f,0);
    }
    return g;
}

int gif_add_frame(GifWriter *g, const Framebuffer *fb){
    if (!g || !g->ok) return 1;
    i32 w=g->w, h=g->h;
    u8 *idx=(u8*)malloc((size_t)w*h);
    if (!idx) return 1;
    /* tonemap (Reinhard + sRGB) then quantize to palette index */
    for (i32 i=0;i<w*h;++i){
        f32 r=fb->px[i*3+0], gg=fb->px[i*3+1], b=fb->px[i*3+2];
        if (r<0)r=0; if(gg<0)gg=0; if(b<0)b=0;
        r=r/(1.0f+r); gg=gg/(1.0f+gg); b=b/(1.0f+b);               /* Reinhard */
        r=(r<=0.0031308f)?12.92f*r:1.055f*powf(r,1/2.4f)-0.055f;    /* sRGB */
        gg=(gg<=0.0031308f)?12.92f*gg:1.055f*powf(gg,1/2.4f)-0.055f;
        b=(b<=0.0031308f)?12.92f*b:1.055f*powf(b,1/2.4f)-0.055f;
        idx[i]=rgb_to_index(r,gg,b);
    }

    /* graphic control extension (delay) */
    put_u8(g->f,0x21); put_u8(g->f,0xF9); put_u8(g->f,4);
    put_u8(g->f,0);                        /* no transparency/disposal */
    put_u16(g->f,(u16)g->delay_cs);
    put_u8(g->f,0);                        /* transparent index */
    put_u8(g->f,0);                        /* block terminator */
    /* image descriptor */
    put_u8(g->f,0x2C);
    put_u16(g->f,0); put_u16(g->f,0);      /* left, top */
    put_u16(g->f,(u16)w); put_u16(g->f,(u16)h);
    put_u8(g->f,0);                        /* no local color table */
    /* LZW image data (min code size 8 for a 256-color table) */
    lzw_compress(g->f, idx, w*h, 8);

    free(idx);
    return 0;
}

int gif_end(GifWriter *g){
    if (!g) return 1;
    int rc=0;
    if (g->ok){
        put_u8(g->f,0x3B);   /* trailer */
    }
    if (g->f) fclose(g->f);
    free(g);
    return rc;
}
