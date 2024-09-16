/* test_tui.c  -  headless test of the half-block diff renderer (no real tty). */
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* exposed internal testing hooks */
size_t tui_test_render(const Framebuffer *fb, void *prev_cells,
                       i32 cols, i32 rows, char *buf, size_t cap);
size_t tui_test_cell_size(void);

static int failures=0, checks=0;
static void ok(const char*n,int c){ checks++; if(c) printf("  ok   %s\n",n); else {printf("  FAIL %s\n",n);failures++;} }

/* does haystack (len n) contain the byte pattern? */
static int contains(const char*h, size_t n, const char*pat, size_t pn){
    if(pn>n) return 0;
    for(size_t i=0;i+pn<=n;++i) if(memcmp(h+i,pat,pn)==0) return 1;
    return 0;
}

int main(void){
    printf("== CATHODE TUI half-block diff renderer ==\n");
    int W=16,H=16; i32 cols=W, rows=H/2;
    Framebuffer*fb=fb_create(W,H);
    /* colorful gradient */
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)
        fb_set(fb,x,y,col3((f32)x/W,(f32)y/H,0.5f));

    size_t cap=1<<16; char*buf=malloc(cap);

    /* (a) full render contains half-block UTF-8 and a truecolor FG sequence */
    size_t n1 = tui_test_render(fb, NULL, cols, rows, buf, cap);
    ok("emits half-block U+2580", contains(buf,n1,"\xe2\x96\x80",3));
    ok("emits truecolor FG (38;2)", contains(buf,n1,"\x1b[38;2;",7));
    /* The encoder folds fg+bg into ONE SGR ("ESC[38;2;r;g;b;48;2;r;g;bm") to cut
     * ~9 bytes per cell, so the background may appear either as a standalone
     * "ESC[48;2;" escape or as the ";48;2;" tail of a combined one. Accept both. */
    ok("emits truecolor BG (48;2)",
       contains(buf,n1,"\x1b[48;2;",7) || contains(buf,n1,";48;2;",6));
    printf("  full render: %zu bytes for %dx%d cells\n", n1, cols, rows);

    /* (b) diff: render same fb twice through shadow -> second is near-empty */
    void *shadow = calloc((size_t)cols*rows, tui_test_cell_size());
    size_t d1 = tui_test_render(fb, shadow, cols, rows, buf, cap);  /* fills shadow */
    size_t d2 = tui_test_render(fb, shadow, cols, rows, buf, cap);  /* nothing changed */
    printf("  diff pass1=%zu pass2=%zu bytes\n", d1, d2);
    ok("unchanged frame emits far fewer bytes", d2 < d1/4);
    ok("unchanged frame ~ empty", d2 == 0);

    /* (c) change ONE pixel -> a small bounded diff */
    fb_set(fb, 5, 4, col3(1.0f,0.0f,0.0f));   /* cell (row2,col5) top pixel */
    size_t d3 = tui_test_render(fb, shadow, cols, rows, buf, cap);
    printf("  one-pixel change diff = %zu bytes\n", d3);
    ok("single change emits a small diff", d3>0 && d3<200);

    /* (d) COMPLETENESS: a full render must address EVERY row and emit exactly
     * one half-block per cell. This is the regression guard for two real bugs:
     *   - relying on terminal auto-wrap instead of an explicit per-row cursor
     *     move (the image collapsed into a smear at the top of the screen);
     *   - a too-small output buffer / EMIT overflow silently truncating the
     *     frame after a few rows. */
    {
        Framebuffer *f2=fb_create(W,H);
        for (i32 y=0;y<H;++y) for (i32 x=0;x<W;++x)
            fb_set(f2,x,y,col3((f32)x/W,(f32)y/H,(f32)(x^y)/64.0f));
        /* generous buffer so this checks the renderer, not the cap */
        size_t cap2=(size_t)cols*rows*80+4096; char*b2=malloc(cap2);
        size_t n2=tui_test_render(f2,NULL,cols,rows,b2,cap2);
        /* count half-blocks */
        int blocks=0;
        for (size_t i=0;i+2<n2;++i)
            if ((unsigned char)b2[i]==0xe2 && (unsigned char)b2[i+1]==0x96
             && (unsigned char)b2[i+2]==0x80) blocks++;
        char msg[96];
        snprintf(msg,sizeof(msg),"full render emits one block per cell (%d == %d)",
                 blocks, cols*rows);
        ok(msg, blocks == cols*rows);
        /* every row 1..rows must appear in a cursor-position escape */
        int missing=0;
        for (i32 r=1;r<=rows;++r){
            char pat[24]; int pl=snprintf(pat,sizeof(pat),"\x1b[%d;",(int)r);
            if (!contains(b2,n2,pat,(size_t)pl)) missing++;
        }
        snprintf(msg,sizeof(msg),"every row addressed explicitly (%d missing)",missing);
        ok(msg, missing==0);
        free(b2); fb_destroy(f2);
    }

    /* (e) a too-small buffer must TRUNCATE SAFELY, never report more than cap */
    {
        char small[128];
        size_t ns=tui_test_render(fb,NULL,cols,rows,small,sizeof(small));
        ok("undersized buffer never overruns cap", ns<=sizeof(small));
    }

    free(buf); free(shadow); fb_destroy(fb);
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
