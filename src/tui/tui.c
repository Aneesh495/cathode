/* ==========================================================================
 * tui.c — truecolor terminal presenter using the Unicode UPPER HALF BLOCK.
 *
 * Each character cell displays two vertically-stacked pixels:
 *   set foreground = TOP pixel color, background = BOTTOM pixel color,
 *   print U+2580 '▀'. A framebuffer of height H shows in H/2 rows.
 *
 * A diff renderer keeps a shadow of the last-presented cells and only emits
 * escape codes for cells that changed — huge bandwidth savings on a mostly
 * static frame. The core cell-encoding is a PURE function (tui_render_to_buf)
 * that needs no terminal, so it is unit-testable headless.
 * ========================================================================== */
#include "cathode/tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

typedef struct { u8 fr,fg,fb, br,bg,bb; int set; } Cell;

/* Sum-of-absolute-channel-difference below which two cells count as identical.
 * 6 channels, so ~4 per channel — under a JND on screen, but enough to absorb
 * the CRT chain's dither/noise so the diff renderer can actually skip cells. */
#define TUI_CELL_EPS 24
#define ABSD(a,b) ((a)>(b) ? (int)((a)-(b)) : (int)((b)-(a)))

/* Where the image is drawn inside the window (set by tui_set_origin). Painting a
 * smaller, centred region is how the demo trades screen area for frame rate: the
 * emulator's cost is per painted cell. */
static i32 g_origin_x = 0, g_origin_y = 0;

/* write(2) is allowed to transfer FEWER bytes than requested — on a pty or a
 * pipe a full frame (tens of KB) routinely short-writes. Ignoring that silently
 * drops the tail of the frame, which showed up as only the first couple of rows
 * being painted (a smear of colored blocks at the top of the screen). Always
 * loop until the whole buffer is out, retrying on EINTR/EAGAIN. */
static void write_all(int fd, const char *buf, size_t n){
    size_t off=0;
    while (off<n){
        ssize_t w=write(fd, buf+off, n-off);
        if (w>0){ off+=(size_t)w; continue; }
        if (w<0 && (errno==EINTR)) continue;
        if (w<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            /* stdout is non-blocking (or the tty buffer is full): wait for it
             * to drain rather than losing the rest of the frame. */
            struct timespec ts={0, 1000000};   /* 1 ms */
            nanosleep(&ts,NULL);
            continue;
        }
        break;   /* real error (EPIPE etc.) — give up on this frame */
    }
}

struct Tui {
    int is_tty;
    struct termios saved;
    int raw_active;
    Cell *shadow;          /* last presented cells */
    i32   shadow_cols, shadow_rows;
    char *out;             /* output byte buffer */
    size_t out_cap;
    int   force;           /* force full redraw next present */
};

/* ---- sRGB tonemap (Reinhard + gamma), matching image module conventions ---- */
static inline u8 tonemap_channel(f32 c){
    if (c<0) c=0;
    c = c/(1.0f+c);                     /* Reinhard */
    f32 s = (c<=0.0031308f) ? 12.92f*c : 1.055f*powf(c,1.0f/2.4f)-0.055f;
    int v=(int)(s*255.0f+0.5f);
    if(v<0)v=0; if(v>255)v=255;
    return (u8)v;
}

/* Encode a framebuffer into `buf` using half-block cells, honoring `prev`
 * (may be NULL for a full redraw). Updates prev to the new state if non-NULL.
 * Returns number of bytes written. This is the pure, testable core. */
/* Zoomed variant: each framebuffer cell is painted as a zoom x zoom block of
 * terminal cells. Runs of identical cells within a row share one SGR, so the
 * byte cost per PIXEL falls roughly linearly with zoom. */
static size_t tui_render_zoom_to_buf(const Framebuffer *fb, Cell *prev,
                                     i32 cols, i32 rows, i32 zoom,
                                     char *buf, size_t cap){
    size_t n=0;
    #define EMITZ(...) do{ \
        if (n >= cap) break; \
        int _w = snprintf(buf+n, cap-n, __VA_ARGS__); \
        if (_w < 0) break; \
        size_t _u = (size_t)_w; \
        n += (_u < cap-n) ? _u : (cap-n); \
    }while(0)
    int last_fr=-1,last_fg=-1,last_fb=-1,last_br=-1,last_bg=-1,last_bb=-1;
    for (i32 row=0; row<rows; ++row){
        /* which framebuffer cell-row this terminal row samples */
        i32 src_row = row / zoom;
        i32 ty = src_row*2, by = src_row*2+1;
        int cursor_x=-1;
        for (i32 x=0; x<cols; ++x){
            i32 sx = x / zoom;
            Color3 top = (sx<fb->w && ty<fb->h) ? fb_get(fb,sx,ty) : col3(0,0,0);
            Color3 bot = (sx<fb->w && by<fb->h) ? fb_get(fb,sx,by) : col3(0,0,0);
            Cell c;
            c.fr=tonemap_channel(top.r); c.fg=tonemap_channel(top.g); c.fb=tonemap_channel(top.b);
            c.br=tonemap_channel(bot.r); c.bg=tonemap_channel(bot.g); c.bb=tonemap_channel(bot.b);
            c.set=1;
            size_t ci=(size_t)row*cols+x;
            int changed = 1;
            if (prev){
                Cell *p=&prev[ci];
                if (!p->set) changed=1;
                else {
                    int d = ABSD(p->fr,c.fr)+ABSD(p->fg,c.fg)+ABSD(p->fb,c.fb)
                          + ABSD(p->br,c.br)+ABSD(p->bg,c.bg)+ABSD(p->bb,c.bb);
                    changed = (d > TUI_CELL_EPS);
                }
            }
            if (!changed) continue;
            if (cursor_x!=x){
                EMITZ("\x1b[%d;%dH", row+1+g_origin_y, x+1+g_origin_x);
                last_fr=last_fg=last_fb=last_br=last_bg=last_bb=-1;
            }
            int need_fg=(c.fr!=last_fr||c.fg!=last_fg||c.fb!=last_fb);
            int need_bg=(c.br!=last_br||c.bg!=last_bg||c.bb!=last_bb);
            if (need_fg && need_bg)
                EMITZ("\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm", c.fr,c.fg,c.fb, c.br,c.bg,c.bb);
            else if (need_fg) EMITZ("\x1b[38;2;%d;%d;%dm", c.fr,c.fg,c.fb);
            else if (need_bg) EMITZ("\x1b[48;2;%d;%d;%dm", c.br,c.bg,c.bb);
            if (need_fg){ last_fr=c.fr; last_fg=c.fg; last_fb=c.fb; }
            if (need_bg){ last_br=c.br; last_bg=c.bg; last_bb=c.bb; }
            EMITZ("\xe2\x96\x80");
            cursor_x=x+1;
            if (prev) prev[ci]=c;
        }
    }
    #undef EMITZ
    return n;
}

static size_t tui_render_to_buf(const Framebuffer *fb, Cell *prev,
                                i32 cols, i32 rows, char *buf, size_t cap){
    size_t n=0;
    /* Append with hard truncation safety: snprintf returns the length it WOULD
     * have written, so advancing `n` by that unconditionally lets `n` run past
     * `cap` and silently drops the rest of the frame (this used to truncate the
     * image to the first few rows). Only advance by what actually fit, and stop
     * emitting once the buffer is full. */
    #define EMIT(...) do{ \
        if (n >= cap) break; \
        int _w = snprintf(buf+n, cap-n, __VA_ARGS__); \
        if (_w < 0) break; \
        size_t _u = (size_t)_w; \
        n += (_u < cap-n) ? _u : (cap-n); \
    }while(0)
    int last_fr=-1,last_fg=-1,last_fb=-1,last_br=-1,last_bg=-1,last_bb=-1;
    int cursor_x=-1, cursor_y=-1;

    for (i32 row=0; row<rows; ++row){
        i32 ty = row*2;       /* top pixel row */
        i32 by = row*2+1;     /* bottom pixel row */
        /* Never rely on the terminal's auto-wrap to advance to the next line:
         * a single full-width run would wrap (or scroll at the last row) and
         * every subsequent row would land one column off, collapsing the whole
         * image into a smear at the top of the screen. Force the cursor model
         * to "unknown" at each row start so the first emitted cell of the row
         * always carries an explicit ESC[row;colH. */
        cursor_x = -1; cursor_y = -1;
        for (i32 x=0; x<cols; ++x){
            /* sample fb (guard against fb smaller than the cell grid) */
            Color3 top = (x<fb->w && ty<fb->h) ? fb_get(fb,x,ty) : col3(0,0,0);
            Color3 bot = (x<fb->w && by<fb->h) ? fb_get(fb,x,by) : col3(0,0,0);
            Cell c;
            c.fr=tonemap_channel(top.r); c.fg=tonemap_channel(top.g); c.fb=tonemap_channel(top.b);
            c.br=tonemap_channel(bot.r); c.bg=tonemap_channel(bot.g); c.bb=tonemap_channel(bot.b);
            c.set=1;

            size_t ci=(size_t)row*cols+x;
            int changed = 1;
            if (prev){
                Cell *p=&prev[ci];
                if (!p->set) changed = 1;
                else {
                    /* Perceptual dead-zone. The CRT chain adds per-pixel noise and
                     * dot-crawl, so a strict != comparison marks essentially EVERY
                     * cell dirty every frame and the diff renderer saves nothing —
                     * that is what pinned the frame rate at ~3 fps (≈330 KB of
                     * escapes per frame). Ignoring changes below a just-noticeable
                     * threshold lets static regions be skipped while motion still
                     * redraws immediately. */
                    int d = ABSD(p->fr,c.fr) + ABSD(p->fg,c.fg) + ABSD(p->fb,c.fb)
                          + ABSD(p->br,c.br) + ABSD(p->bg,c.bg) + ABSD(p->bb,c.bb);
                    changed = (d > TUI_CELL_EPS);
                }
            }
            if (!changed) continue;

            /* move cursor if not already contiguous */
            if (cursor_y!=row || cursor_x!=x){
                EMIT("\x1b[%d;%dH", row+1, x+1);
                last_fr=last_fg=last_fb=last_br=last_bg=last_bb=-1; /* SGR may reset after move */
            }
            /* Emit the colors. Combining fg+bg into ONE SGR sequence (they can
             * share a single ESC[...m) saves ~9 bytes per cell versus two
             * separate escapes — a ~25% cut in total frame bytes, which is the
             * binding constraint on frame rate here. */
            int need_fg = (c.fr!=last_fr||c.fg!=last_fg||c.fb!=last_fb);
            int need_bg = (c.br!=last_br||c.bg!=last_bg||c.bb!=last_bb);
            if (need_fg && need_bg){
                EMIT("\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm",
                     c.fr,c.fg,c.fb, c.br,c.bg,c.bb);
            } else if (need_fg){
                EMIT("\x1b[38;2;%d;%d;%dm", c.fr,c.fg,c.fb);
            } else if (need_bg){
                EMIT("\x1b[48;2;%d;%d;%dm", c.br,c.bg,c.bb);
            }
            if (need_fg){ last_fr=c.fr; last_fg=c.fg; last_fb=c.fb; }
            if (need_bg){ last_br=c.br; last_bg=c.bg; last_bb=c.bb; }
            /* upper half block U+2580 -> UTF-8 E2 96 80 */
            EMIT("\xe2\x96\x80");
            cursor_y=row; cursor_x=x+1;

            if (prev) prev[ci]=c;
        }
    }
    #undef EMIT
    return n;
}

Tui *tui_create(void){
    Tui *t=(Tui*)calloc(1,sizeof(Tui));
    t->is_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    t->force=1;
    if (t->is_tty){
        tcgetattr(STDIN_FILENO,&t->saved);
        struct termios raw=t->saved;
        raw.c_lflag &= ~(ECHO|ICANON|ISIG|IEXTEN);
        raw.c_iflag &= ~(IXON|ICRNL|BRKINT|INPCK|ISTRIP);
        raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
        tcsetattr(STDIN_FILENO,TCSANOW,&raw);
        t->raw_active=1;
        /* non-blocking stdin */
        int fl=fcntl(STDIN_FILENO,F_GETFL,0);
        fcntl(STDIN_FILENO,F_SETFL,fl|O_NONBLOCK);
        /* alt screen + hide cursor + DISABLE AUTO-WRAP (?7l) + clear.
         * Auto-wrap off matters: we paint full-width rows, and with wrap on the
         * last column of the bottom row would scroll the whole screen up by one
         * line every frame. */
        const char *init="\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J";
        write_all(STDOUT_FILENO, init, strlen(init));
    }
    return t;
}

void tui_destroy(Tui *t){
    if(!t) return;
    if (t->is_tty){
        const char *fin="\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";   /* restore wrap too */
        write_all(STDOUT_FILENO, fin, strlen(fin));
        if (t->raw_active) tcsetattr(STDIN_FILENO,TCSANOW,&t->saved);
    }
    free(t->shadow); free(t->out); free(t);
}

void tui_query_size(Tui *t, i32 *cols, i32 *rows){
    struct winsize ws;
    if (t->is_tty && ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>0){
        if(cols)*cols=ws.ws_col; if(rows)*rows=ws.ws_row;
    } else {
        if(cols)*cols=80; if(rows)*rows=24;
    }
}

void tui_force_redraw(Tui *t){ if(t) t->force=1; }

void tui_clear(Tui *t){
    if(!t) return;
    if (t->is_tty){
        /* home cursor + clear entire screen so no stale cells remain after a
         * window resize (the source of the "small image + garbage" artifact) */
        const char *clr="\x1b[H\x1b[2J";
        write_all(STDOUT_FILENO, clr, strlen(clr));
    }
    t->force=1;
    /* invalidate the shadow so the next present repaints every cell */
    if (t->shadow) memset(t->shadow, 0, (size_t)t->shadow_cols*t->shadow_rows*sizeof(Cell));
}

static void ensure_shadow(Tui *t, i32 cols, i32 rows){
    if (t->shadow_cols!=cols || t->shadow_rows!=rows){
        free(t->shadow);
        t->shadow=(Cell*)calloc((size_t)cols*rows, sizeof(Cell));
        t->shadow_cols=cols; t->shadow_rows=rows;
        t->force=1;
    }
}

void tui_set_origin(Tui *t, i32 x, i32 y){ (void)t; g_origin_x = x<0?0:x; g_origin_y = y<0?0:y; }

void tui_present_zoom(Tui *t, const Framebuffer *fb, i32 zoom){
    if (zoom < 1) zoom = 1;
    /* Painting `zoom` terminal cells per framebuffer cell keeps the picture
     * full-screen while cutting the number of cells (and therefore escape bytes
     * and emulator glyph work) by zoom^2 — this is what makes a maximized window
     * run smoothly instead of at a few frames per second.
     *
     * CLAMP to the real terminal size: the framebuffer's cell-rows times zoom can
     * exceed the window height (a fb of 34 cell-rows at zoom 3 wants 102 rows on
     * a 52-row terminal), and painting the overflow wastes more than half of the
     * frame's bytes on cells nobody can see. */
    i32 cols=fb->w * zoom;
    i32 rows=((fb->h+1)/2) * zoom;
    i32 tcols=0, trows=0;
    tui_query_size(t,&tcols,&trows);
    if (tcols>0 && cols>tcols-g_origin_x) cols=tcols-g_origin_x;
    if (trows>1 && rows>trows-1-g_origin_y) rows=trows-1-g_origin_y;  /* HUD row */
    if (cols<1) cols=1; if (rows<1) rows=1;
    ensure_shadow(t,cols,rows);
    /* NOTE on cost: zooming shrinks the FRAMEBUFFER (less scene + CRT work) but
     * the number of TERMINAL cells painted is fixed by the window — the emulator
     * still renders cols*rows glyphs. What zoom buys on the wire is that adjacent
     * cells within a zoom-block are identical, so they share one SGR escape and
     * cost ~3 bytes each instead of ~25. That is the difference between ~300 KB
     * and ~120 KB per frame at a maximized window. The remaining floor is the
     * emulator's own glyph throughput, which is why very large windows are
     * inherently slower; use a smaller window or --quality for higher fps. */
    size_t need=(size_t)cols*rows*64 + (size_t)rows*16 + 256;
    if (t->out_cap<need){ t->out=(char*)realloc(t->out,need); t->out_cap=need; }
    if (t->force) memset(t->shadow,0,(size_t)cols*rows*sizeof(Cell));
    size_t n = tui_render_zoom_to_buf(fb, t->shadow, cols, rows, zoom,
                                      t->out, t->out_cap);
    t->force=0;
    if (t->is_tty && n>0) write_all(STDOUT_FILENO, t->out, n);
}

void tui_present(Tui *t, const Framebuffer *fb){
    i32 cols=fb->w;
    i32 rows=(fb->h+1)/2;
    ensure_shadow(t,cols,rows);
    /* Worst case per cell: cursor move "\x1b[999;999H" (10) + fg SGR
     * "\x1b[38;2;255;255;255m" (19) + bg SGR (19) + the 3-byte block = 51.
     * Round up to 64 and add slack for the per-row moves and the trailing NUL
     * that snprintf always wants. Under-sizing this silently truncated the
     * frame to a few rows. */
    size_t need=(size_t)cols*rows*64 + (size_t)rows*16 + 256;
    if (t->out_cap<need){ t->out=(char*)realloc(t->out,need); t->out_cap=need; }

    Cell *prev = t->force ? NULL : t->shadow;
    if (t->force){
        /* clear shadow so subsequent diffs are correct after the full redraw */
        memset(t->shadow,0,(size_t)cols*rows*sizeof(Cell));
    }
    size_t n = tui_render_to_buf(fb, t->force?t->shadow:prev, cols, rows, t->out, t->out_cap);
    /* if we passed NULL prev on force we still need shadow filled; render_to_buf
       updates shadow only when prev!=NULL. On force we passed t->shadow, good. */
    t->force=0;
    if (t->is_tty && n>0) write_all(STDOUT_FILENO, t->out, n);
}

void tui_hud(Tui *t, const char *scene_name, f32 fps, i32 frame, const char *stats){
    if (!t->is_tty) return;
    char line[256];
    snprintf(line,sizeof(line),
        "\x1b[%d;1H\x1b[0m\x1b[48;2;20;20;30m\x1b[38;2;120;255;160m"
        " CATHODE  scene:%-10s  fps:%5.1f  frame:%-6d  %s \x1b[0m\x1b[K",
        t->shadow_rows+1, scene_name?scene_name:"?", (double)fps, frame, stats?stats:"");
    write_all(STDOUT_FILENO, line, strlen(line));
}

/* Centered bordered overlay box for help / scene menu.
 *
 * Drawn with cursor-addressed writes ON TOP of the presented frame. Two things
 * are essential and were previously wrong, which made the box flicker and
 * refuse to disappear:
 *
 *  1. The diff presenter's shadow still believes those cells hold scene pixels,
 *    so on the next frame it only repaints the ones that "changed" — leaving the
 *    box half-overwritten (the flashing). We therefore INVALIDATE the shadow
 *    cells the box covers, so the presenter unconditionally repaints that region
 *    every frame: the box is redrawn cleanly on top while it's open, and the
 *    scene fills straight back in on the frame after it closes.
 *  2. The output buffer must be sized from the actual box dimensions (a wide box
 *    on a large terminal overflowed the old fixed 8 KB buffer, truncating the
 *    escape stream mid-sequence and corrupting the display). */
void tui_overlay(Tui *t, const char *title, const char **lines, i32 nlines){
    if (!t || !t->is_tty) return;
    i32 cols=t->shadow_cols, rows=t->shadow_rows;
    if (cols<20 || rows<6) return;
    if (nlines<0) nlines=0;
    /* box width = widest line (+padding), capped to terminal */
    i32 w=(i32)strlen(title);
    for (i32 i=0;i<nlines;++i){ i32 l=(i32)strlen(lines[i]); if(l>w)w=l; }
    w+=4; if (w>cols-2) w=cols-2; if (w<8) w=8;
    /* border(1) + title(1) + blank/content(nlines) + border(1) */
    i32 h=nlines+3; if (h>rows-1) h=rows-1; if (h<4) h=4;
    i32 ox=(cols-w)/2, oy=(rows-h)/2;
    if (ox<0) ox=0; if (oy<0) oy=0;

    /* worst case per row: cursor move + 2 SGR + w chars + a couple of attrs */
    size_t cap=(size_t)h*((size_t)w + 96) + 64;
    char *buf=(char*)malloc(cap); if(!buf) return;
    size_t n=0;
    #define OUT(...) do{ \
        if (n>=cap) break; \
        int _w=snprintf(buf+n, cap-n, __VA_ARGS__); \
        if (_w<0) break; \
        size_t _u=(size_t)_w; n += (_u < cap-n) ? _u : (cap-n); \
    }while(0)
    for (i32 r=0;r<h;++r){
        OUT("\x1b[%d;%dH\x1b[48;2;12;14;24m\x1b[38;2;120;220;255m", oy+r+1, ox+1);
        if (r==0 || r==h-1){                       /* top / bottom border */
            OUT("+"); for(i32 c=1;c<w-1;++c) OUT("-"); OUT("+");
        } else if (r==1){                          /* centered title */
            i32 tl=(i32)strlen(title); if (tl>w-2) tl=w-2;
            i32 pad=(w-2-tl)/2; if(pad<0)pad=0;
            OUT("|\x1b[1m"); for(i32 c=0;c<pad;++c)OUT(" ");
            OUT("%.*s", (int)tl, title);
            for(i32 c=pad+tl;c<w-2;++c)OUT(" "); OUT("\x1b[22m|");
        } else {
            i32 li=r-2;
            const char *s=(li>=0&&li<nlines)?lines[li]:"";
            OUT("| %-*.*s |", w-4, w-4, s);
        }
    }
    OUT("\x1b[0m");
    #undef OUT
    write_all(STDOUT_FILENO, buf, n);
    free(buf);

    /* Invalidate the covered shadow cells (see note 1 above). */
    if (t->shadow){
        for (i32 r=0;r<h;++r){
            i32 sy=oy+r; if (sy<0||sy>=rows) continue;
            for (i32 cx=0;cx<w;++cx){
                i32 sx=ox+cx; if (sx<0||sx>=cols) continue;
                t->shadow[(size_t)sy*cols+sx].set=0;
            }
        }
    }
}

Key tui_poll(Tui *t){
    if (!t->is_tty) return KEY_NONE;
    unsigned char ch;
    ssize_t r=read(STDIN_FILENO,&ch,1);
    if (r<=0) return KEY_NONE;
    if (ch=='\x1b'){
        unsigned char seq[2];
        if (read(STDIN_FILENO,&seq[0],1)<=0) return KEY_QUIT; /* lone ESC = quit */
        if (seq[0]=='['){
            if (read(STDIN_FILENO,&seq[1],1)<=0) return KEY_OTHER;
            switch(seq[1]){
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_OTHER;
    }
    switch(ch){
        case 'q': case 'Q': return KEY_QUIT;
        case ' ': return KEY_SPACE;
        case 'n': return KEY_NEXT;
        case 'p': return KEY_PREV;
        case '+': case '=': return KEY_PLUS;
        case '-': case '_': return KEY_MINUS;
        case '\t': return KEY_TAB;
        case '\r': case '\n': return KEY_ENTER;
        case 'r': case 'R': return KEY_R;
        case 'h': case 'H': return KEY_H;
        case '?': case '/': return KEY_HELP;
        case '1': return KEY_1; case '2': return KEY_2; case '3': return KEY_3;
        case '4': return KEY_4; case '5': return KEY_5; case '6': return KEY_6;
        case '7': return KEY_7; case '8': return KEY_8; case '9': return KEY_9;
        case '0': return KEY_0;
    }
    return KEY_OTHER;
}

f64 tui_now(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec*1e-9;
}
void tui_sleep(f64 seconds){
    if (seconds<=0) return;
    struct timespec ts; ts.tv_sec=(time_t)seconds; ts.tv_nsec=(long)((seconds-ts.tv_sec)*1e9);
    nanosleep(&ts,NULL);
}

/* ---- testing hook: expose the pure renderer ---- */
size_t tui_test_render(const Framebuffer *fb, void *prev_cells,
                       i32 cols, i32 rows, char *buf, size_t cap){
    return tui_render_to_buf(fb, (Cell*)prev_cells, cols, rows, buf, cap);
}
size_t tui_test_cell_size(void){ return sizeof(Cell); }
