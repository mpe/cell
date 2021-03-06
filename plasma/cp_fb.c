// cp_fb.c 
//
// Copyright (c) 2006, Mike Acton <macton@cellperformance.com>
//
// Modified for compilation on SPU by Jonathan Adamczewski <jonathan@brnz.org>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the "Software"), to deal in the Software without restriction, including without
// limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial
// portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
// EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
// AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

// NOTES:
// From Geert Uytterhoeven 2007-01-26 04:50:44, 
//      http://patchwork.ozlabs.org/linuxppc/patch?id=9143
//
//     "As the actual graphics hardware cannot be accessed directly by Linux,
//     ps3fb uses a virtual frame buffer in main memory. The actual screen image is
//     copied to graphics memory by the GPU on every vertical blank, by making a
//     hypervisor call."
//

// suppress a warning
#define __BIG_ENDIAN__

#ifdef DEBUG
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <linux/vt.h>
#include <linux/kd.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <asm/ps3fb.h>
#include "cp_fb.h"

#include <spu_intrinsics.h>
#include <spu_mfcio.h>


unsigned long long mmap_eaddr(unsigned long long start, size_t length, int
				prot, int flags, int fd, off_t offset);
unsigned long long munmap_eaddr(unsigned long long start, size_t length);
int ioctl_eaddr(unsigned int d, unsigned int request, void* lsa, void* ea, int size);

#ifdef DEBUG
static inline const char*
select_error_str( int existing_error, const char* const existing_error_str, int new_error, const char* const new_error_str )
{
  // Only report the first error found - any error that follows is probably just a cascading effect.
  const char* error_str = (char*)( (~(intptr_t)existing_error & (intptr_t)new_error & (intptr_t)new_error_str)
                                 |  ((intptr_t)existing_error & (intptr_t)existing_error_str) );

  return (error_str);
}
#endif

// These will point to two integers stored in main mem to facilitate flipping :|
static vector unsigned int *cnst[2];

int cp_fb_open( cp_fb* const restrict fb, void* space)
{
    // Define a struct to facilitate ioctl's writing to memory and our retrieval
    // __attribute((aligned)) overkill :P
    struct cp_fb_store {
        struct fb_vblank vblank __attribute__((aligned(16)));
        struct ps3fb_ioctl_res res __attribute__((aligned(16)));
        vector unsigned int cnst[2] __attribute__((aligned(16)));
    } __attribute__((aligned(16)));

    { int asserter[128 - sizeof(struct cp_fb_store)]; (void)asserter; }

    struct cp_fb_store *store = (struct cp_fb_store*)space;

#ifdef DEBUG
    const char*    error_str      = NULL;
    int            error          = 0;
#endif

    // Open framebuffer device

    const int   fb_fd             = open( "/dev/fb0", O_RDWR );
#ifdef DEBUG
    const int   open_fb_error     = (fb_fd >> ((sizeof(int)<<3)-1));
    const char* open_fb_error_str = "Could not open /dev/fb0. Check permissions.";
  
    error_str = select_error_str( error, error_str, open_fb_error, open_fb_error_str );
    error     = error | open_fb_error;
#endif

    // Check for vsync

    struct fb_vblank vblank;

    const int   get_vblank_error     = ioctl_eaddr(fb_fd, FBIOGET_VBLANK, &vblank, &store->vblank, (sizeof(vblank)+15)&~0xf);

#ifdef DEBUG
    const char* get_vblank_error_str = "Could not get vblank info (FBIOGET_VBLANK)";

    error_str = select_error_str( error, error_str, get_vblank_error, get_vblank_error_str );
    error     = error | get_vblank_error;
#endif

    const int   has_vsync            = vblank.flags & FB_VBLANK_HAVE_VSYNC;
#ifdef DEBUG
    const int   has_vsync_error      = (~(-has_vsync|has_vsync))>>((sizeof(int)<<3)-1);
    const char* has_vsync_error_str  = "No vsync available (FB_VBLANK_HAVE_VSYNC)";

    error_str = select_error_str( error, error_str, has_vsync_error, has_vsync_error_str );
    error     = error | has_vsync_error;
#endif

    // Get screen resolution and frame count

    struct ps3fb_ioctl_res res;

    const int   screeninfo_error     = ioctl_eaddr(fb_fd, PS3FB_IOCTL_SCREENINFO, &res, &store->res, (sizeof(res)+15)&~0xf);

#ifdef DEBUG
    const char* screeninfo_error_str = "Could not get screen info (PS3_IOCTL_SCREENINFO)";

    error_str = select_error_str( error, error_str, screeninfo_error, screeninfo_error_str );
    error     = error | screeninfo_error;
#endif

    const int   has_at_least_double_buffer           = (res.num_frames - 2) >> ((sizeof(res.num_frames)<<3)-1);
#ifdef DEBUG
    const int   has_at_least_double_buffer_error     = ~has_at_least_double_buffer;
    const char* has_at_least_double_buffer_error_str = "Could not get screen info (PS3_IOCTL_SCREENINFO)";

    error_str = select_error_str( error, error_str, has_at_least_double_buffer_error, has_at_least_double_buffer_error_str );
    error     = error | has_at_least_double_buffer_error;
#endif

    const uint32_t bpp                      = 4; // This is fixed for PS3 fb, and there's not a test for it.
    const uint32_t frame_size               = res.xres * res.yres * bpp;
    const uint32_t double_buffer_frame_size = frame_size * 2;

    // const uint32_t frame_top_margin_size    = res.xres * res.yoff * bpp;
    // const uint32_t frame_bottom_margin_size = frame_top_margin_size;
    // const uint32_t frame_size               = frame_full_size; /* - ( frame_top_margin_size + frame_bottom_margin_size ); */
    // const uint32_t double_buffer_frame_size = frame_size * 2;

    const uintptr_t fb_addr           = (uintptr_t)mmap_eaddr(0, double_buffer_frame_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
#ifdef DEBUG
    const int       fb_mmap_error     = fb_addr >> ((sizeof(uintptr_t)<<3)-1);
    const char*     fb_mmap_error_str = "Could not get mmap frame buffer";

    error_str = select_error_str( error, error_str, fb_mmap_error, fb_mmap_error_str );
    error     = error | fb_mmap_error;
#endif

    // Take control of frame buffer from kernel
    ioctl(fb_fd, PS3FB_IOCTL_ON, 0);

    // yoff is the number of lines that cannot be copied to the CRT before the vblank. For the most part this represents
    // unusable frame buffer space. While it is possible to draw to the area if you draw in the opposite frame buffer's
    // offset space, which will (due to poor draw timing by ps3fb) be the thing that is actually drawn, it's very 
    // difficult to work with in practice. So:
    //
    //     (1)  The y offset area will be treated as "off limits".
    //     (2)  An equivalent border will be created at the bottom, so the frame looks balanced even though it is
    //          not entirely full screen. 

    // xoff is the number of lines that cannot be copied to the CRT before the hblank.
    // Similar to the y offset space, the x offset space is displayed on the wrong (previous) line. So:
    //
    //     (1)  The x offset area will be treated as "off limits".
    //     (2)  An equivalent border will be created at the right, so the frame looks balanced even though it is
    //          not entirely full screen. 

    uintptr_t draw_start_addr = fb_addr;
    uintptr_t draw_next_addr  = draw_start_addr + ( res.yres * res.xres * bpp );
    uintptr_t drawable_h      = res.yres - ( 2 * res.yoff );
    uintptr_t drawable_w      = res.xres - ( 2 * res.xoff );

    // xoff is the number of lines that cannot be copied to the CRT before the hblank. This area is much easier to use. 
    // Similar to the y offset space, the x offset space is displayed on the wrong (previous) line. So:
    // In principle, it is possible to steal back the x offset space by shifting back the line address to the 
    // start of the border of the previous line. Like so:
    //
    //     (1)  One additional line will be taken from the height so the a complete horizontal line can be started
    //          early.
    //     (2)  The frame buffer address returned in cp_fb will be offset by (xres-xoff) in order for the remaining
    //          space to represent a rectangular area of drawable memory.
    //
    //     i.e. 
    //     uintptr_t draw_start_addr = fb_addr + ( ( res.xres - res.xoff ) * bpp );
    //     uintptr_t draw_next_addr  = draw_start_addr + ( res.yres * res.xres * bpp );
    //     uintptr_t drawable_h      = res.yres - 1 - ( 2 * res.yoff );
    //     uintptr_t drawable_w      = res.xres;
    //
    //     But I wouldn't recommend it, since on some CRTs the effect of this would be that the frame does not appear
    //     square.

    fb->stride        = res.xres;
    fb->w             = drawable_w;
    fb->h             = drawable_h;
    fb->fd            = fb_fd;
    fb->start_addr    = fb_addr;
    fb->size          = double_buffer_frame_size;
    fb->draw_addr[0]  = draw_start_addr;
    fb->draw_addr[1]  = draw_next_addr;

    // Clear out the whole buffer. Any unused space is black. It's also convinient to start with a cleared frame
    // buffer for the user.

    // skip this for now.
    // memset((void*)fb_addr, 0x00, double_buffer_frame_size );

    // write constants for wait and flip commands
    vector unsigned int zero = {0};
    vector unsigned int one = {1};
    spu_mfcdma32(&zero, (long)&store->cnst[0], 16, 0, MFC_PUT_CMD);
    spu_mfcdma32(&one, (long)&store->cnst[1], 16, 0, MFC_PUT_CMD);
    cnst[0] = &store->cnst[0];
    cnst[1] = &store->cnst[1];
#ifdef DEBUG
    return (error);
#else
	return 0;
#endif
}

void
cp_fb_close( const cp_fb* const restrict fb )
{
    // Give frame buffer control back to the kernel
    ioctl(fb->fd, PS3FB_IOCTL_OFF, 0);

    munmap_eaddr( fb->start_addr, fb->size );

    close(fb->fd);
}

void
cp_fb_wait_vsync( cp_fb* const restrict fb )
{
    ioctl(fb->fd, FBIO_WAITFORVSYNC, cnst[0] );
}

void
cp_fb_flip( cp_fb* const restrict fb, unsigned long field_ndx )
{
    ioctl(fb->fd, PS3FB_IOCTL_FSEL,  cnst[field_ndx] );
}
