/* test_gif.c — build a small animated GIF and validate it with Python (PIL if
 * present, else a structural parse). Exercises the from-scratch LZW encoder. */
#include "cathode/image.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(void){
    int W=64,H=48,NF=8;
    GifWriter *g=gif_begin("/tmp/_cathode_test.gif", W, H, 6, 1);
    if (!g){ printf("FAIL gif_begin\n"); return 1; }
    Framebuffer *fb=fb_create(W,H);
    for (int f=0; f<NF; ++f){
        /* moving diagonal color gradient so frames differ */
        for (int y=0;y<H;++y)for(int x=0;x<W;++x){
            f32 u=(f32)(x+f*4)/W, v=(f32)y/H;
            fb_set(fb,x,y, col3(u, v, 0.5f+0.5f*sinf((x+y+f*6)*0.2f)));
        }
        if (gif_add_frame(g,fb)!=0){ printf("FAIL add_frame %d\n",f); return 1; }
    }
    gif_end(g);
    fb_destroy(fb);
    printf("wrote /tmp/_cathode_test.gif (%d frames, %dx%d)\n", NF, W, H);

    /* validate with python: parse header + count frames via PIL, or structurally */
    int rc = system(
      "python3 - <<'PY'\n"
      "import sys\n"
      "d=open('/tmp/_cathode_test.gif','rb').read()\n"
      "assert d[:6] in (b'GIF89a',b'GIF87a'), 'bad signature'\n"
      "w=d[6]|(d[7]<<8); h=d[8]|(d[9]<<8)\n"
      "assert (w,h)==(64,48), f'bad size {w}x{h}'\n"
      "# count image descriptors (0x2C) as a rough frame count\n"
      "frames=d.count(b'\\x2c')\n"
      "assert d[-1]==0x3b, 'no trailer'\n"
      "try:\n"
      "    from PIL import Image, ImageSequence\n"
      "    im=Image.open('/tmp/_cathode_test.gif')\n"
      "    n=sum(1 for _ in ImageSequence.Iterator(im))\n"
      "    assert n==8, f'PIL saw {n} frames'\n"
      "    print(f'PY PASS: PIL decoded {n} frames {im.size}')\n"
      "except ImportError:\n"
      "    print(f'PY PASS: structural (sig ok, {w}x{h}, trailer ok, ~{frames} descriptors)')\n"
      "PY");
    if (rc!=0){ printf("FAIL: python validation rc=%d\n", rc); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
