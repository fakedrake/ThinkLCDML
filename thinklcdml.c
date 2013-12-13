// -----------------------------------------------------------------------------
// Copyright (c) 2010 Think Silicon Ltd
// Think Silicon Ltd Confidential Proprietary
// -----------------------------------------------------------------------------
//     All Rights reserved - Unpublished -rights reserved under
//         the Copyright laws of the European Union
//
//  This file includes the Confidential information of Think Silicon Ltd
//  The receiver of this Confidential Information shall not disclose
//  it to any third party and shall protect its confidentiality by
//  using the same degree of care, but not less than a reasonable
//  degree of care, as the receiver uses to protect receiver's own
//  Confidential Information. The entire notice must be reproduced on all
//  authorised copies and copies may only be made to the extent permitted
//  by a licensing agreement from Think Silicon Ltd.
//
//  The software is provided 'as is', without warranty of any kind, express or
//  implied, including but not limited to the warranties of merchantability,
//  fitness for a particular purpose and noninfringement. In no event shall
//  Think Silicon Ltd be liable for any claim, damages or other liability, whether
//  in an action of contract, tort or otherwise, arising from, out of or in
//  connection with the software or the use or other dealings in the software.
//
//
//                    Think Silicon Ltd
//                    http://www.think-silicon.com
//                    Patras Science Park
//                    Rion Achaias 26504
//                    Greece
// -----------------------------------------------------------------------------
// FILE NAME  : thinklcdml.c
// KEYWORDS   :
// PURPOSE    : ThinkLCDML kernel module
// DEPARTMENT :
// AUTHOR     : Chris "fakedrake" Perivolaropoulos
// GENERATION :
// RECEIVER   :
// NOTES      :
// filippakoc

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/platform_device.h>

#include "thinklcdml.h"

MODULE_LICENSE("GPL");

//TODO: Does it work as a module?
//TODO: Remove platform device code
//TODO: Add PM facilities?

// Uncomment this to enable polling for vsync
//#define TLCD_POLL_VSYNC

//#define TLCD_DEBUG
//#define TLCD_DEBUG_PROCENTRY

#define OL(info) (MINOR(info->dev->devt))

#define PRINT_E(args...)	printk(KERN_ERR     "ThinkLCDML: " args)
#define PRINT_I(args...)	printk(KERN_INFO    "ThinkLCDML: " args)
#define PRINT_W(args...)	printk(KERN_WARNING "ThinkLCDML: " args)

#define TLCDML_DEBUG_LEVEL 3
#if TLCDML_DEBUG_LEVEL>0
#define LVL_DBG(l, ...)     do {					\
	if (l <= TLCDML_DEBUG_LEVEL) printk("<1>TLCDML DEBUG(level: " #l "): " __VA_ARGS__); } while(0)
#endif

#ifdef TLCD_DEBUG
#define PRINT_D(fmt, args...)	do { printk("ThinkLCDML: " fmt "\n", ##args); } while (0)
#else
#define PRINT_D(args...)	do { } while(0)
#endif

#if defined(TLCD_DEBUG_PROCENTRY) || defined(TLCD_DEBUG)
#define PRINT_PROC_ENTRY	do { printk("ThinkLCDML: calling: %s()\n", __FUNCTION__); } while (0)
#else
#define PRINT_PROC_ENTRY	do {} while (0)
#endif

#define think_readl(base, offset) fb_readl((u32 __iomem *)((base) + (offset)))
#define think_writel(base, offset, val) fb_writel(val, (u32 __iomem *)((unsigned long)(base) + (offset)))
#define think_writel_D(base, offset, val) do {PRINT_D("0x%08lx:0x%08x\n", (base) + (offset), (val)); think_writel(base, offset, val);} while (0)

#define XY16TOREG32(x, y) ((x) << 16 | ((y) & 0xffff))
#define CLAMP255(i) ( ((i)<0) ? 0 : ((i)>255) ? 255 : (i) )

#define PRINT_F(args...)	do { } while(0)

//#include "dpls_sif.c"
//#define SENSOR_IN_ADDR 0x400000



struct thinklcdml_par {
    unsigned long regs;

    u32 pseudo_palette[TLCD_PALETTE_COLORS];
    u32 mode;

    wait_queue_head_t wait_vsync;
    volatile u64 vblank_count;
};



static struct fb_fix_screeninfo thinklcdml_fix __initdata = {
    .id	       = "TSi ThinkLCDML",
    .type      =  FB_TYPE_PACKED_PIXELS,//FB_TYPE_PLANES,
    .xpanstep  = 1,
    .ypanstep  = 1,
    .ywrapstep = 0,
    .accel     = TLCD_ACCEL,
};

struct tlcdml_fb_data {
    unsigned fb_num;
    struct fb_info* infos[TLCDML_LAYERS_NUMBER];
};

static unsigned int fb_memsize __initdata = 3145728;
static unsigned int fb_hard = 0; // fb_hard means: 0, from __get_free_pages. 1, ioremap. 2, no allocation (see thinklcdml_setfbmem)
static unsigned long physical_register_base __initdata = TLCD_PHYSICAL_BASE;
static unsigned long fb_addr __initdata = 0x10000000;
static struct fb_var_screeninfo default_var __initdata;
static unsigned long virtual_regs_base = 0, color_mode = TLCD_MODE_RGBA8888; // color_mode -> 0
static char* module_options __initdata = NULL;

static int thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
static int thinklcdml_set_par(struct fb_info *info);
static int thinklcdml_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info);
static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info);
static int thinklcdml_mmap(struct fb_info *info, struct vm_area_struct *vma);
static int thinklcdml_blank(int blank_mode, struct fb_info *info);
static int thinklcdml_vsync(struct fb_info *info);
static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg);
static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr);
bool LCD_Bpp = false;
int MODE_RES = 0; // 0 = 640x480 | 1 = 800x600 | 2 = 1024x768



/* Debug info of registers. */
static void dump_regs( struct thinklcdml_par *par, int layer)
{
    PRINT_D("*** ThinkLCDML register dump!***");

    /* Global registers. */
    PRINT_D("TLCD_REG_CONFIG=0x%08x",	    think_readl( par->regs, TLCD_REG_CONFIG));
    PRINT_D("TLCD_REG_MODE=0x%08x",	    think_readl( par->regs, TLCD_REG_MODE));
    PRINT_D("TLCD_REG_CLKCTRL=0x%08x", 	    think_readl( par->regs, TLCD_REG_CLKCTRL));
    PRINT_D("TLCD_REG_BGCOLOR=0x%08x",	    think_readl( par->regs, TLCD_REG_BGCOLOR));
    PRINT_D("TLCD_REG_RESXY=0x%08x",	    think_readl( par->regs, TLCD_REG_RESXY));
    PRINT_D("TLCD_REG_STRIDE=0x%08x",	    think_readl( par->regs, TLCD_REG_STRIDE));
    PRINT_D("TLCD_REG_FRONTPORCHXY=0x%08x", think_readl( par->regs, TLCD_REG_FRONTPORCHXY));
    PRINT_D("TLCD_REG_BLANKINGXY=0x%08x",   think_readl( par->regs, TLCD_REG_BLANKINGXY));
    PRINT_D("TLCD_REG_BACKPORCHXY=0x%08x",  think_readl( par->regs, TLCD_REG_BACKPORCHXY));
    PRINT_D("TLCD_REG_CURSORXY=0x%08x",	    think_readl( par->regs, TLCD_REG_CURSORXY));
    PRINT_D("TLCD_REG_IDREG=0x%08x",	    think_readl( par->regs, TLCD_REG_IDREG));
    PRINT_D("TLCD_REG_STATUS=0x%08x",	    think_readl( par->regs, TLCD_REG_STATUS));
    PRINT_D("TLCD_REG_INTERRUPT=0x%08x",    think_readl( par->regs, TLCD_REG_INTERRUPT));

    /* Layer registers. */
    PRINT_D("TLCD_REG_LAYER_MODE(%d)=0x%08x", 	     layer, think_readl( par->regs, TLCD_REG_LAYER_MODE(layer) ));
    PRINT_D("TLCD_REG_LAYER_STARTXY(%d)=0x%08x",     layer, think_readl( par->regs, TLCD_REG_LAYER_STARTXY(layer) ));
    PRINT_D("TLCD_REG_LAYER_SIZEXY(%d)=0x%08x",      layer, think_readl( par->regs, TLCD_REG_LAYER_STARTXY(layer) ));
    PRINT_D("TLCD_REG_LAYER_BASEADDR(%d)(v)=0x%08x", layer, think_readl( par->regs, TLCD_REG_LAYER_BASEADDR(layer) ));
    PRINT_D("TLCD_REG_LAYER_STRIDE(%d)=0x%08x",      layer, think_readl( par->regs, TLCD_REG_LAYER_STRIDE(layer) ));
    PRINT_D("TLCD_REG_LAYER_RESXY(%d)=0x%08x",       layer, think_readl( par->regs, TLCD_REG_LAYER_RESXY(layer) ));
    PRINT_D("TLCD_REG_LAYER_SCALEX(%d)=0x%08x",      layer, think_readl( par->regs, TLCD_REG_LAYER_SCALEX(layer) ));
    PRINT_D("TLCD_REG_LAYER_SCALEY(%d)=0x%08x",      layer, think_readl( par->regs, TLCD_REG_LAYER_SCALEY(layer) ));
}

static struct fb_ops thinklcdml_ops = {
    .owner	    = THIS_MODULE,
    .fb_check_var   = thinklcdml_check_var,
    .fb_set_par	    = thinklcdml_set_par,
    .fb_setcolreg   = thinklcdml_setcolreg,
    .fb_pan_display = thinklcdml_pan_display,
    .fb_fillrect    = cfb_fillrect,
    .fb_copyarea    = cfb_copyarea,
    .fb_imageblit   = cfb_imageblit,
    .fb_mmap	    = thinklcdml_mmap,
    .fb_blank	    = thinklcdml_blank,
    .fb_ioctl	    = thinklcdml_ioctl,


};

/**
 * Computes the surface pitch based on a resolution and bpp
 */
static __inline__ u_long get_line_length(int xres_virtual, int bpp)
{
    return ((xres_virtual * bpp + 31) & ~31) >> 3;
}


/*
  Populate var given the info.
*/
static int thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct thinklcdml_par *par = info->par;
    u_long line_length;

    PRINT_PROC_ENTRY;
    /*  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
     *  as FB_VMODE_SMOOTH_XPAN is only used internally */
    if (var->vmode & FB_VMODE_CONUPDATE)
    {
	//var->vmode |= FB_VMODE_YWRAP;
	var->xoffset = info->var.xoffset;
	var->yoffset = info->var.yoffset;
    }

    /* Sanity check for resolutions. */
    if (!var->xres)
	var->xres = 1;

    if (!var->yres)
	var->yres = 1;

    if (var->xres > var->xres_virtual)
	var->xres_virtual = var->xres;

    if (var->yres > var->yres_virtual)
	var->yres_virtual = var->yres;

    /* Special way to enter test mode. */
    if (var->bits_per_pixel == 0)
	par->mode = TLCD_MODE_TEST;

    /* Sanity check bpp */
    if (var->bits_per_pixel <= 8)
	var->bits_per_pixel = 8;
    else if (var->bits_per_pixel <= 16)
	var->bits_per_pixel = 16;
    else if (var->bits_per_pixel <= 32)
	var->bits_per_pixel = 32;
    else
	return -EINVAL;

    /* Virtual resolution setup. */
    if (var->xres_virtual < var->xoffset + var->xres)
	var->xres_virtual = var->xoffset + var->xres;
    if (var->yres_virtual < var->yoffset + var->yres)
	var->yres_virtual = var->yoffset + var->yres;

    /* Check the new memory size. */
    line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
    if (line_length * var->yres_virtual > info->screen_size) {
	PRINT_W("Bad mode: out of memory (virtual:%ux%u bpp:%u)\n", var->xres_virtual, var->yres_virtual, var->bits_per_pixel);
	return -ENOMEM;
    }

    /* Populate the color mode. */
    switch (var->bits_per_pixel) {
    case 8:
	if (var->grayscale || var->red.offset == 0) {
            /* LUT8 && L8 */
	    var->red    = (struct fb_bitfield) { 0, 8, 0 };
	    var->green  = (struct fb_bitfield) { 0, 8, 0 };
	    var->blue   = (struct fb_bitfield) { 0, 8, 0 };
	    var->transp = (struct fb_bitfield) { 0, 0, 0 };
	} else {
            /* RGB 332*/
	    var->red    = (struct fb_bitfield) { 5, 3, 0 };
	    var->green  = (struct fb_bitfield) { 2, 3, 0 };
	    var->blue   = (struct fb_bitfield) { 0, 2, 0 };
	    var->transp = (struct fb_bitfield) { 0, 0, 0 };
	}
	break;

    case 16:
	if (var->transp.length) {
	    /* RGBA 5551 */
	    var->red    = (struct fb_bitfield) { 11, 5, 0 };
	    var->green  = (struct fb_bitfield) {  6, 5, 0 };
	    var->blue   = (struct fb_bitfield) {  1, 5, 0 };
	    var->transp = (struct fb_bitfield) {  0, 1, 0 };
	} else {
	    /* RGB 565 */
	    var->red    = (struct fb_bitfield) { 11, 5, 0 };
	    var->green  = (struct fb_bitfield) {  5, 6, 0 };
	    var->blue   = (struct fb_bitfield) {  0, 5, 0 };
	    var->transp = (struct fb_bitfield) {  0, 0, 0 };
	}
	break;

    case 32:
	if (var->transp.offset || var->red.offset == 16) {
	    /* ARGB 8888 */
	    var->transp = (struct fb_bitfield) { 24, 8, 0 };
	    var->red    = (struct fb_bitfield) { 16, 8, 0 };
	    var->green  = (struct fb_bitfield) {  8, 8, 0 };
	    var->blue   = (struct fb_bitfield) {  0, 8, 0 };
	} else {
	    /* RGBA 8888 */
	    var->red    = (struct fb_bitfield) { 24, 8, 0 };
	    var->green  = (struct fb_bitfield) { 16, 8, 0 };
	    var->blue   = (struct fb_bitfield) {  8, 8, 0 };
	    var->transp = (struct fb_bitfield) {  0, 8, 0 };
	}

	break;
    }
    return 0;
}


/*
  Apply the registers according to the par of this
  info. `thinklcdml_check_var' does something like this only it
  populates var instead of setting up the hw.
*/
static int thinklcdml_set_par(struct fb_info *info)
{

    struct thinklcdml_par *par = info->par;

    int resx=info->var.xres;
    int resy=info->var.yres;
    int frontporchx = resx +info->var.right_margin;
    int frontporchy = resy +info->var.lower_margin;

    int blankx = frontporchx+ info->var.hsync_len;
    int blanky = frontporchy+ info->var.vsync_len;

    int backporchx = blankx + info->var.left_margin;
    int backporchy = blanky + info->var.upper_margin;


    u32 mode, mode_layer, i, mask = 0xffffffff;
    u8 red, green, blue;

    PRINT_PROC_ENTRY;


    think_writel(par->regs, TLCD_REG_LAYER_STARTXY(OL(info)), 0x00000000 );
    think_writel(par->regs, TLCD_REG_LAYER_RESXY(OL(info)),  XY16TOREG32(resx , resy));
    think_writel(par->regs, TLCD_REG_RESXY,                  XY16TOREG32(resx, resy));
    think_writel(par->regs, TLCD_REG_LAYER_SIZEXY(OL(info)), XY16TOREG32(resx, resy));
    think_writel(par->regs, TLCD_REG_FRONTPORCHXY,           XY16TOREG32(frontporchx, frontporchy));
    think_writel(par->regs, TLCD_REG_BLANKINGXY,             XY16TOREG32(blankx, blanky));
    think_writel(par->regs, TLCD_REG_BACKPORCHXY,            XY16TOREG32(backporchx, backporchy));

    /* Decide on color mode */
    switch(info->var.bits_per_pixel) {
    case  8:
	info->fix.visual = info->var.red.offset ? FB_VISUAL_TRUECOLOR : FB_VISUAL_PSEUDOCOLOR;
	break;
    case 16:
 	info->fix.visual = FB_VISUAL_TRUECOLOR;
	break;
    case 32:
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	break;
    default:
	PRINT_D ("Unable to determine bits per pixel...");
	return -EINVAL;
    }

    info->fix.line_length = get_line_length(info->var.xres_virtual, info->var.bits_per_pixel);

    /* Guess color mode */
    switch (info->var.bits_per_pixel) {
    case  8:
	mode = info->var.grayscale ? TLCD_MODE_L8 : (info->var.red.offset ? TLCD_MODE_RGB332 : TLCD_MODE_LUT8);
	/* The odds of it actually being grayscale or 332 are quite slim... so do this anyway */
	for(i=0;i<256;i++) {
	    red   = CLAMP255(i);
	    green = CLAMP255(i);
	    blue  = CLAMP255(i);
	    think_writel(par->regs, TLCD_PALETTE_OFFSET + i * 4, (red << 16) | (green << 8) | (blue << 0));
	}
	think_writel(par->regs, TLCD_REG_MODE, mode | TLCD_CONFIG_GAMMA);
	break;
    case 16:
	mask &= ~(1<<20);  /* Make sure there is no look up table. */
	mode = info->var.transp.length ? TLCD_MODE_RGBA5551 : TLCD_MODE_RGBA4444;
	break;
    case 32:
	mask &= ~(1<<20);
	mode = info->var.transp.offset || info->var.red.offset == 16 ? TLCD_MODE_ARGB8888 : TLCD_MODE_RGBA8888;
	break;
    default:
	mask &= ~(1<<20);
	PRINT_D("Unable to determine color mode...\n");
	return -EINVAL;
    }
    if (LCD_Bpp){
      mode = 0xd;//0xd;
    }else{
      mode = 0x5;//0xd;
    }


    if (par->mode == TLCD_MODE_TEST)
	PRINT_W("Detected color mode is %u, overriding because we are in test mode!\n", mode);
    else {
	par->mode = mode;
	PRINT_D("Detected color mode is %u", par->mode);
    }

    /* Get reg mode */
    /* XXX: mode 80000000, front proch */
    mode_layer = think_readl(par->regs, TLCD_REG_MODE) & mask;
    mode_layer = TLCD_CONFIG_ENABLE | (mode_layer & ~0x3) | par->mode;

    think_writel (virtual_regs_base, TLCD_REG_MODE , 1<<31);
    think_writel (virtual_regs_base, TLCD_REG_CLKCTRL, TLCD_CLKCTRL);
    think_writel (virtual_regs_base, TLCD_REG_BGCOLOR , TLCD_BGCOLOR);

    /* XXX: Get rid of this one way or another */
    //think_writel(virtual_regs_base, 0x2c , 0x00000000); /* XXX: what register is 0x2c? */
     //think_writel(virtual_regs_base, TLCD_REG_LAYER_MODE(0), 0x88ff0105);

    PRINT_D ("Actually enabling fb%d", OL(info));
    /* Enable, global full alpha, color mode as defined. */
    think_writel(par->regs, TLCD_REG_LAYER_MODE(OL(info)), ((TLCD_CONFIG_ENABLE) | (0xff<<16) | (mode_layer & 0xf)));
    think_writel(par->regs, TLCD_REG_LAYER_STRIDE(OL(info)), info->fix.line_length);
   // think_writel(par->regs, TLCD_REG_LAYER_STRIDE(OL(info)), 0x4ec0);

    dump_regs(par, OL(info));

    return 0;
}

/* Fill in the pallete. */
static int thinklcdml_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info)
{
    struct thinklcdml_par *par = info->par;
    u32 out_val;

    //PRINT_PROC_ENTRY;
    /* PRINT_D("i:%02x = red:%02x green:%02x blue:%02x alpha:%02x", regno, red, green, blue, transp); */

    if (regno >= TLCD_PALETTE_COLORS)
	return -EINVAL;

    /* grayscale = 0.30*R + 0.59*G + 0.11*B */
    if (info->var.grayscale)
	red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;

    if (info->fix.visual == FB_VISUAL_TRUECOLOR || info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
	red    = (red	 >> (8 + 8 - info->var.red.length));
	green  = (green	 >> (8 + 8 - info->var.green.length));
	blue   = (blue	 >> (8 + 8 - info->var.blue.length));
	transp = (transp >> (8 + 8 - info->var.transp.length));

	out_val = (red << info->var.red.offset) | (green << info->var.green.offset) | (blue << info->var.blue.offset);
    } else {
	red    >>= 8;
	green  >>= 8;
	blue   >>= 8;
	transp >>= 8;

	out_val = (red << 16) | (green << 8) | (blue << 0);
    }

    /* the pseudo_palette expectes color values in screen format, computed as seen above */
    par->pseudo_palette[regno] = out_val;

    /* the hardware always expects an RGB888 value */
    think_writel(par->regs, TLCD_PALETTE_OFFSET + regno * 4, out_val); //(red << 16) | (green << 8) | (blue << 0));

    return 0;
}

static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct thinklcdml_par *par = info->par;
    unsigned long address;

    PRINT_PROC_ENTRY;
    PRINT_D("mode:%u xoffset:%u yoffset:%u", var->vmode, var->xoffset, var->yoffset);

    /* check bounds */
    if (var->vmode & FB_VMODE_YWRAP ||
	var->xoffset + var->xres > info->var.xres_virtual ||
	var->yoffset + var->yres > info->var.yres_virtual)
	return -EINVAL;

    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;
    /* compute new base address */
    address = info->fix.smem_start + var->yoffset * info->fix.line_length + var->xoffset * (var->bits_per_pixel >> 3);

    think_writel(par->regs, TLCD_REG_LAYER_BASEADDR(OL(info)), address);

    return 0;
}

static int thinklcdml_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    PRINT_PROC_ENTRY;

    /* think_writel( par->regs, TLCD_REG_LAYER_MODE(OL(info)), mode | TLCD_CONFIG_ENABLE); */

    if (vma->vm_pgoff)
	return -EINVAL;

    /* can only map up to smem_len bytes of video memory */
    if ((vma->vm_end - vma->vm_start) > info->fix.smem_len) {
	PRINT_I("Failed to mmap, %lu bytes requested\n", vma->vm_end - vma->vm_start);
	return -EINVAL;
    }

    /* LEON SPARC note:
     * Due to write through cache AND not hardware acceleration
     * modifying the framebuffer it is OK to cache.
     *
     * On NON-Cache systems uncomment:
     * vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
     */
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vma->vm_flags |= VM_IO | VM_MAYSHARE | VM_SHARED;

    if (io_remap_pfn_range(vma, vma->vm_start, info->fix.smem_start >> PAGE_SHIFT, vma->vm_end - vma->vm_start, vma->vm_page_prot))
	return -EAGAIN;

    return 0;
}




/* This does not clear the framebuffer, just the screen. */
static int thinklcdml_blank(int blank_mode, struct fb_info *info)
{
    struct thinklcdml_par *par = info->par;
    u32 mode_reg;

    PRINT_PROC_ENTRY;
    PRINT_D("blank: %d", blank_mode);

    /* blank out the screen by setting or clearing TLCD_MODE bit 31 */
    mode_reg = think_readl(par->regs, TLCD_REG_MODE);

    if (blank_mode == 0)
	; //think_writel(par->regs, TLCD_REG_MODE, mode_reg | 1<<19);
    else if (blank_mode == 1)
	; //think_writel(par->regs, TLCD_REG_MODE, mode_reg & ~(1<<19));
    else
	return -EINVAL;

    return 0;
}

/* Parse the options.
   XXX: Make this to not be position dependent. */
static int __init thinklcdml_setup(char *options, char* separator)
{
    char *this_opt;
    int custom = 0;
    int count = 0;

    PRINT_PROC_ENTRY;

    if (!options || !*options) {
	/* Default to low resolution, Add video=thinklcdml:... to kernel command line */
	PRINT_I("No user setup options: Defaulting to %dx%d, bpp: %d, color mode: 0x%lx\n", default_var.xres, default_var.yres, default_var.bits_per_pixel, color_mode);
	return 1;
    }

    while ((this_opt = strsep(&options, separator)) != NULL) {

	if (!*this_opt)
	    continue;

	if (custom)
	    switch (count++) {
	    case 0: default_var.pixclock     = simple_strtoul(this_opt, NULL, 0); break;
	    case 1: default_var.xres         = default_var.xres_virtual = simple_strtoul(this_opt, NULL, 0); break;
	    case 2: default_var.right_margin = simple_strtoul(this_opt, NULL, 0); break;
	    case 3: default_var.hsync_len    = simple_strtoul(this_opt, NULL, 0); break;
	    case 4: default_var.left_margin  = simple_strtoul(this_opt, NULL, 0); break;
	    case 5: default_var.yres         = default_var.yres_virtual = simple_strtoul(this_opt, NULL, 0); break;
	    case 6: default_var.lower_margin = simple_strtoul(this_opt, NULL, 0); break;
	    case 7: default_var.vsync_len    = simple_strtoul(this_opt, NULL, 0); break;
	    case 8: default_var.upper_margin = simple_strtoul(this_opt, NULL, 0); break;
	    case 9:
	      if(!strcmp(this_opt, "RGB32")){
		LCD_Bpp = true;
		//printk(KERN_INFO    "filippakoc 32 ------> !!!!!");
	      }else if(!strcmp(this_opt, "RGB16") || !strcmp(this_opt, "RGBA5551")){
		LCD_Bpp =false;
		//printk(KERN_INFO    "filippakoc 16 ------> !!!!!");
	      }
	      if (!strcmp(this_opt, "LUT8")) {
		    default_var.bits_per_pixel = 8, default_var.red.offset = 0;
		    color_mode = TLCD_MODE_LUT8;
		}
		else if (!strcmp(this_opt, "RGB16")) {
		    default_var.bits_per_pixel = 16;
		    default_var.red.offset = 11;
		    default_var.red.length = 5;
		    default_var.green.offset = 6;
		    default_var.green.length = 5;
		    default_var.blue.offset = 1;
		    default_var.blue.length = 5;
		    default_var.transp.length = 1;
		    default_var.transp.offset = 0;
		    color_mode = TLCD_MODE_RGBA5551;
		    printk(KERN_INFO    "!!!!!");
		}
		else if (!strcmp(this_opt, "RGB32")) {
		    /*default_var.bits_per_pixel = 32;
		    default_var.red.offset = 24;
		    default_var.red.length = 8;
		    default_var.green.offset = 16;
		    default_var.green.length = 8;
		    default_var.blue.offset = 8;
		    default_var.blue.length = 8;
		    default_var.transp.length = 8;
		    default_var.transp.offset = 0;
		    color_mode = TLCD_MODE_RGBA8888;
		      printk(KERN_INFO    "!!!!!");*/
		    default_var.bits_per_pixel = 32;
		    default_var.red.offset = 16;
		    default_var.red.length = 8;
		    default_var.green.offset = 8;
		    default_var.green.length = 8;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 8;
		    color_mode = TLCD_MODE_ARGB8888;
		     printk(KERN_INFO    "!!!!!");
		}
		else if (!strcmp(this_opt, "TEST")) {
		    PRINT_W("Warning: Test mode enabled, any mode change will silently fail!\n");
		    color_mode = TLCD_MODE_TEST;
		    default_var.bits_per_pixel = 0, default_var.red.offset = 0;
		}
		else if (!strcmp(this_opt, "RGB332")) {
		    default_var.bits_per_pixel = 8;
		    default_var.red.offset = 5;
		    default_var.red.length = 3;
		    default_var.green.offset = 2;
		    default_var.green.length = 3;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 2;
		    color_mode = TLCD_MODE_RGB332;
		    printk(KERN_INFO    "!!!!!");
		}
		else if (!strcmp(this_opt, "RGBA4444")) {
		    default_var.bits_per_pixel = 16;
		    default_var.red.offset = 11;
		    default_var.red.length = 5;
		    default_var.green.offset = 5;
		    default_var.green.length = 6;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 5;
		    color_mode = TLCD_MODE_RGBA4444;
		     printk(KERN_INFO    "!!!!!");
		}
		else if (!strcmp(this_opt, "ARGB8888")) {
		    default_var.bits_per_pixel = 32;
		    default_var.red.offset = 16;
		    default_var.red.length = 8;
		    default_var.green.offset = 8;
		    default_var.green.length = 8;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 8;
		    color_mode = TLCD_MODE_ARGB8888;
		     printk(KERN_INFO    "!!!!!");
		}
		else if (!strcmp(this_opt, "L8"))
		    default_var.bits_per_pixel = 8, default_var.grayscale = 1, color_mode = TLCD_MODE_L8;
		else {
		    PRINT_W("Unknown format '%s', defaulting to 8-bit palette mode\n", this_opt);
		    default_var.bits_per_pixel = 8, default_var.red.offset = 0;
		}
		break;

	    case 10: fb_memsize = PAGE_ALIGN(simple_strtoul(this_opt, NULL, 0)); break;
	    case 11: fb_addr = simple_strtoul(this_opt, NULL, 0); fb_hard = 1; custom = 0; break;
	    }
	else if (!strcmp(this_opt, "1024x768")) {
	    default_var = m1024x768;
	    MODE_RES = 2;
	    custom = 1;
	    count = 9;
	}
	else if (!strcmp(this_opt, "800x600")) {
	    default_var = m800x600;
	    MODE_RES = 1;
	    custom = 1;
	    count = 9;
	}
	else if (!strcmp(this_opt, "640x480")) {
	    default_var = m640x480;
	    MODE_RES = 0;
	    custom = 1;
	    count = 9;
	     printk(KERN_INFO    "!!640x480!!!");
	}
	else if (!strcmp(this_opt, "800x480")) {
	    default_var = m800x480;
	    custom = 1;
	    count = 9;
	}
	else if (!strcmp(this_opt, "custom")) {
	    custom = 1;
	    PRINT_F("Custom mode set");
	}
	else {
	    PRINT_W("Unknown mode '%s', defaulting to 1024x768 16-bit palette mode\n", this_opt);
	    return 1;
	}
    }

    return 1;
}

static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr)
{
    struct thinklcdml_par* par = ptr;
    PRINT_PROC_ENTRY;

    /* clear the interrupt */
    think_writel(par->regs, TLCD_REG_INTERRUPT, 0);

    /* update stats, also needed as a condition to unblock */
    par->vblank_count++;

    /* wake up any threads waiting */
    wake_up_interruptible(&par->wait_vsync);
    ///////////////////////////////////////////////////
    //if (sync_snapshot==1) SIF_get_a_snapshot();    ////   TOOOO BAD!!!! Waits for the sensor....
    //printk(".\n");                                 ////
    ///////////////////////////////////////////////////

    return IRQ_HANDLED;
}

static int thinklcdml_vsync(struct fb_info *info)
{
    struct thinklcdml_par* par = info->par;
    u64 count;
    PRINT_PROC_ENTRY;

    /* enable vsync interrupt; it will be cleared on arrival */
    count = par->vblank_count;

    //PRINT_E ("Writing: 0x%08lx, 0x%08x, 8", par->regs, TLCD_REG_INTERRUPT);
    think_writel(par->regs, TLCD_REG_INTERRUPT, 4);
    /* wait for it for a while */
    if (!wait_event_interruptible_timeout(par->wait_vsync, count != par->vblank_count, HZ / 10)) {
	return -ETIMEDOUT;
    }

    return 0;
}

static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    struct thinklcdml_par *par = info->par;
    int i,mode;
    int red,green,blue;
    unsigned Bpp, color, ret;
    void* offset;

    PRINT_PROC_ENTRY;

    PRINT_D ("calling ioctl...");

    switch (cmd) {
	/* DirectFB uses this interface to sync to the LCD */
	//------------------------------------------------------------------------
    case TLCDML_SET_REGISTER:
	PRINT_D ("ioctl: TLCDML_SET_REGISTER");
	think_writel( par->regs, *((unsigned long*)arg), *(((unsigned long*)arg)+1));
	break;
    case TLCDML_GET_REGISTER:
	PRINT_D ("ioctl: TLCDML_GET_REGISTER");
	*(((unsigned long*)arg)+1) = think_readl( par->regs, *((unsigned long*)arg));
	break;
    case TLCDML_COLOR_CLEAR:
	PRINT_D ("ioctl: LCDML_COLOR_CLEAR");
	Bpp = info->var.bits_per_pixel/8;
	color = arg << (8*( sizeof(unsigned long) - Bpp));
	PRINT_D ( "clear color: 0x%x (depth: %d, stride: %d, color mode: 0x%lx)", color, info->var.bits_per_pixel, info->fix.line_length, color_mode);

	memset(info->screen_base, 0, info->fix.line_length * info->var.yres_virtual );

	for ( offset = info->screen_base;
	      (unsigned long)offset < (unsigned long)info->screen_base + info->fix.line_length * info->var.yres_virtual;
	      offset += info->fix.line_length ) {
	    for ( i = 0; i < info->fix.line_length; i+=Bpp) {
		*(unsigned*)(offset+i) = color;
	    }
	}
	break;
    case TLCDML_GET_INFO:
	PRINT_D("ioctl: TLCDML_GET_INFO");
	memcpy((void*)arg, &info->var, sizeof(struct fb_var_screeninfo));
	break;
    case TLCDML_DUMP_REGS:
	PRINT_D("ioctl: TLCDML_DUMP_REGS");
	if (arg < TLCDML_LAYERS_NUMBER)
	    dump_regs(par, arg);
	else
	    PRINT_E( "Dumping of registers of layer %d cannot be done (layer number %d)\n", (int)arg, TLCDML_LAYERS_NUMBER);
	break;
    case TLCDML_GET_LAYER_NUMBER:
	PRINT_D("ioctl: TLCDML_GET_LAYER_NUMBER");
	*((unsigned long*)arg) = OL(info);
	break;
    case FBIO_WAITFORVSYNC:
	PRINT_D("ioctl: FBIO_WAITFORVSYNC");
	ret = thinklcdml_vsync(info);
	return ret;
	break;
	//------------------------------------------------------------------------
    case FBIO_RAMP_SET:
	PRINT_D("ioctl: FBIO_RAMP_SET");
	mode = think_readl(par->regs, TLCD_REG_MODE);

	if(arg==0)
	    think_writel(par->regs, TLCD_REG_MODE, mode & ~TLCD_CONFIG_GAMMA);
	else {
	    for(i=0;i<256;i++)
	    {
		red   = CLAMP255(i+arg);
		green = CLAMP255(i+arg);
		blue  = CLAMP255(i+arg);
		think_writel(par->regs, TLCD_PALETTE_OFFSET + i * 4, (red << 16) | (green << 8) | (blue << 0));
	    }
	    think_writel(par->regs, TLCD_REG_MODE, mode | TLCD_CONFIG_GAMMA);
	}
	return 1;
	break;
    case FBIO_I2C_W:
	PRINT_D("ioctl: FBIO_I2C_W");
	break;
	//------------------------------------------------------------------------
    case FBIO_I2C_R:
	PRINT_D("ioctl: FBIO_I2C_R");
	break;
	//------------------------------------------------------------------------
    case FBIO_SIF_W:
	PRINT_D("ioctl: FBIO_SIF_W");
	break;
	//------------------------------------------------------------------------
    case FBIO_SIF_R:
	PRINT_D("ioctl: FBIO_SIF_R");
	break;
	//------------------------------------------------------------------------
    default:
	PRINT_W("thinklcdml_ioctl: Unknown ioctl 0x%08x has been requested (arg: 0x%08lx)\n", cmd, arg);
	return -EINVAL;
    }
    return 0;
}

static int thinklcdml_add_layer(struct platform_device *device, unsigned long physical_start, unsigned long virtual_start)
{
    int retval;
    struct fb_info *info;
    struct thinklcdml_par *par;
    struct tlcdml_fb_data *drvdata = platform_get_drvdata(device);

    PRINT_PROC_ENTRY;

    PRINT_D("Allocating framebuffer infos.");
    /* framebuffer alloc allocates infos and you tell it how much
     * extra space you need for pars. */
    drvdata->infos[drvdata->fb_num] = framebuffer_alloc(sizeof(struct thinklcdml_par), &device->dev);
    if (!drvdata->infos[drvdata->fb_num]) {
	PRINT_E("Failed to allocate memory with framebuffer_alloc\n");
	return 1;
    }
    info = drvdata->infos[drvdata->fb_num];


    if (!virtual_regs_base) {
	PRINT_E ("Registers not mapped correctly\n");
	return 1;
    }
    par = info->par;
    par->regs = virtual_regs_base;

    /* initialize register file */
    PRINT_D("Initializing register file.");
    think_writel (par->regs, TLCD_REG_LAYER_SCALEY(drvdata->fb_num),  0x4000);
    think_writel (par->regs, TLCD_REG_LAYER_SCALEX(drvdata->fb_num),  0x4000);
    think_writel(par->regs, TLCD_REG_LAYER_BASEADDR(drvdata->fb_num), physical_start);
    think_writel(par->regs, TLCD_REG_LAYER_MODE(drvdata->fb_num),      (drvdata->fb_num ? TLCD_MODE : (1<<31)|TLCD_MODE) | color_mode );
    PRINT_D ("Framebuffer no: %d, physical start: 0x%08lx, virtual start 0x%08lx, mode: 0x%08x", drvdata->fb_num, physical_start, virtual_start, think_readl(par->regs, TLCD_REG_LAYER_MODE(drvdata->fb_num)));

    /* fixup default_var; must already have set info->screen_size */
    info->screen_size    = fb_memsize; /* get_line_length(info->var.xres_virtual, info->var.bits_per_pixel) * var.yres_virtual; */

    if (thinklcdml_check_var(&default_var, info)) {
	retval = 1;
	goto err_fb_alloc;
    }

    info->var             = default_var;
    info->screen_base     = (char __iomem *) virtual_start;
    info->fbops           = &thinklcdml_ops;
    info->fix             = thinklcdml_fix;
    info->fix.smem_start  = physical_start;
    info->fix.smem_len    = fb_memsize;
    info->fix.mmio_start  = physical_register_base; /* TLCD_PHYSICAL_BASE; */
    info->fix.mmio_len    = TLCD_MMIOALLOC;
    info->fix.line_length = get_line_length(info->var.xres_virtual, info->var.bits_per_pixel);

    if(!info->fix.line_length)
	PRINT_W ( "attempting to initalize 0 line-length in layer %d", drvdata->fb_num);

    info->pseudo_palette  = par->pseudo_palette;
    //info->flags           = FBINFO_FLAG_DEFAULT;
/*	| FBINFO_HWACCEL_XPAN
	| FBINFO_HWACCEL_YPAN
	| FBINFO_PARTIAL_PAN_OK;
/*
    /* allocate a color map */
    PRINT_D("Allocating color map.");
    retval = fb_alloc_cmap(&info->cmap, 256, 0);
    if (retval < 0) {
	PRINT_E("Failed to allocate memory with fb_alloc_cmap\n");
	goto err_fb_alloc;
    }

    /* finally, register our framebuffer */
    PRINT_D("Register framebuffer.");\
    retval = register_framebuffer(info);
    if (retval < 0) {
	PRINT_E("Failed to register framebuffer\n");
	goto err_cmap_alloc;
    }

    if (OL(info) != drvdata->fb_num) PRINT_E ("Minor numbers seem to be a bit messed up...\n");

    drvdata->fb_num++;

    return 0;

err_cmap_alloc:
    fb_dealloc_cmap(&info->cmap);

err_fb_alloc:
    framebuffer_release(info);

    return retval;
}

static int __init thinklcdml_probe(struct platform_device *device)
{
    int retval = -ENOMEM;
    unsigned long virtual_start[TLCDML_LAYERS_NUMBER];
    unsigned long physical_start[TLCDML_LAYERS_NUMBER];
    unsigned long page;
    struct tlcdml_fb_data *drvdata = kmalloc(sizeof(struct tlcdml_fb_data), GFP_DMA | GFP_KERNEL);
    unsigned i, alloc_layers;

    PRINT_PROC_ENTRY;

    memset(drvdata, 0, sizeof(struct tlcdml_fb_data));
    platform_set_drvdata(device, drvdata);
    // fb_hard should be 0 or 1 here..

    /* Physical start is never used when we have more than one
     * layers. */
    physical_start[0] = fb_addr;


    if (fb_hard) {
	PRINT_D("Static allocation of framebuffers.");

	/* got framebuffer base address from argument list */
	/* In this case support for only one layer. TODO some trickery
	 * to support more layers */
	PRINT_I("Using specificed framebuffer base address: 0x%08lx\n", fb_addr);

	/* ask from the kernel to reserve that physical memory range
	 * for us; not really necessary, but stronly recommended */
	if (!request_mem_region(physical_start[0], fb_memsize, device->name)) {
	    PRINT_E("Request mem region failed\n");
	    return -ENOMEM;
	}

	/* Define it here so that if virtual fails we can clear it
	 * up. */
	alloc_layers = 1;

	/* and finally remap that memory to kernel's address space */
	virtual_start[0] = (unsigned long) ioremap_nocache(physical_start[0], fb_memsize);
	if (!virtual_start[0]) {
	    PRINT_E("Failed to remap memory\n");
	    retval = -ENOMEM;
	    goto err_fb_mem_alloc;
	}

    } else {
	/* allocate our frambuffer memory
	 *
	 * if the following fails, try to configure your kernel with
	 * CONFIG_FORCE_MAX_ZONEORDER; as of writing this, MAX_ORDER
	 * is configured as 11, which allows a maximum alloc of 4MB */
	PRINT_D("Dynamic allocation of framebuffers.");

	for (alloc_layers = 0; alloc_layers < TLCDML_LAYERS_NUMBER; alloc_layers++) {

	    /* GFP_KERNEL means we can do whatever we want with this
	     * memory. There are no real other options uless you are
	     * doing something fancy. */
	    virtual_start[alloc_layers] = (unsigned long) __get_free_pages(GFP_DMA | GFP_KERNEL, get_order(fb_memsize));
	    if (!virtual_start[alloc_layers]) {
		PRINT_E("Unable to allocate framebuffer:%u memory (%u bytes order:%u MAX_ORDER:%u)\n", alloc_layers, fb_memsize, get_order(fb_memsize), MAX_ORDER);
		if (!alloc_layers) {
		    retval = -ENOMEM;
		    goto err_fb_mem_alloc;
		}
	    }
	    PRINT_D("Successfully allocated layers.");
	    //physical_start[alloc_layers]=0x00000000;
	    //virtual_start[alloc_layers]=__va(physical_start[alloc_layers]);
	    physical_start[alloc_layers] = virt_to_phys((void *)virtual_start[alloc_layers]);

	    /* Set page reserved so that mmap will work; this is
	     * necessary since we'll be remapping normal memory */
	    for (page = virtual_start[alloc_layers]; page < PAGE_ALIGN(virtual_start[alloc_layers] + fb_memsize); page += PAGE_SIZE) {
		SetPageReserved(virt_to_page((void *)page));
	    }
	    PRINT_D("Layer pages reserved.");
	}
    }

    /* clear out the screen, try to minimize flickering */
    for (i = 0; i<alloc_layers; i++) {
	PRINT_D("Clearing framebuffer data %d/%d", i, alloc_layers);
	memset((unsigned long *) virtual_start[i], 0, fb_memsize);
	PRINT_I("VRAM fb%u PA:0x%08lx -> VA:0x%lx len:%u\n", i, physical_start[i], virtual_start[i], fb_memsize);
    }

    if (!request_mem_region(physical_register_base, TLCD_MMIOALLOC, device->name)) {
	PRINT_E("Request for MMIO for register file was negative.\n");
	goto err_fb_mem_alloc;
    }

    PRINT_D("Performing ioremap for registers (physical base: 0x%08lx, len: 0x%08x).", physical_register_base, TLCD_MMIOALLOC);
    virtual_regs_base = (unsigned long)ioremap_nocache(physical_register_base, TLCD_MMIOALLOC);
    if (!virtual_regs_base) {
	PRINT_E("MMIO remap for register file failed\n");
	goto err_reg_mem_request;
    } else
	PRINT_I("MMIO for register file PA:0x%08lx -> VA:0x%08lx len:%u\n", physical_register_base, virtual_regs_base, TLCD_MMIOALLOC);

    PRINT_D("Setting default modeL 0x%08x", TLCD_MODE);
    think_writel(virtual_regs_base, TLCD_REG_MODE, TLCD_MODE);

    /* Register the framebuffer */
    for (i=0; i < alloc_layers; i++) {
	PRINT_D("Registering framebuffer %d/%d.", i+1, alloc_layers);
	if (thinklcdml_add_layer(device, physical_start[i], virtual_start[i])) {
	    PRINT_E("Failed to register layer %d\n",i);
	    goto err_reg_map;
	}
    }
    PRINT_I("Registered %d framebuffer devices.\n", i+1);

    /* initialize the wait object for interrupt */
    init_waitqueue_head(&((struct  thinklcdml_par*)drvdata->infos[0]->par)->wait_vsync);

    PRINT_D("Wait object for vsync interrupts initialized.");


    retval = request_irq(TLCD_VSYNC_IRQ, thinklcdml_vsync_interrupt, IRQF_DISABLED, "thinklcdml vsync", drvdata->infos[0]->par);
    if (retval < 0) {
	PRINT_E("Failed to request irq %u (%d)\n", TLCD_VSYNC_IRQ, retval);
	goto err_irq_setup;
    }
    dump_regs((struct  thinklcdml_par*)drvdata->infos[0]->par, 0);
    return 0;

    // XXX: check how error interact with the creation of multiple
    // fbs
err_irq_setup:
    free_irq(TLCD_VSYNC_IRQ, drvdata->infos[0]->par);

err_reg_map:
    iounmap((void *)virtual_regs_base);

err_reg_mem_request:
    release_mem_region(physical_register_base, TLCD_MMIOALLOC);

err_fb_mem_alloc:
    release_mem_region(physical_start[0], fb_memsize);

    if (fb_hard)
    {
	for (i=0; i<alloc_layers; i++) {
	    iounmap((void *)physical_start[i]);
	    release_mem_region(physical_start[i], fb_memsize);
	}
    }
    else
	for (i=0; i<alloc_layers; i++)
	    free_pages(virtual_start[i], get_order(fb_memsize));

    return retval;
}

static int thinklcdml_remove(struct platform_device *device)
{
    struct fb_info* info;
    struct tlcdml_fb_data *drvdata = platform_get_drvdata(device);
    struct thinklcdml_par *par = drvdata->infos[0]->par;
    unsigned long page;
    unsigned i;

    PRINT_PROC_ENTRY;

    /* release the interrupt */
    think_writel(par->regs, TLCD_REG_INTERRUPT, 0);

    PRINT_D("Freeing irq...");
    free_irq(TLCD_VSYNC_IRQ, par);

    PRINT_D("Unmapping register file...");
    iounmap((void *)virtual_regs_base);

    PRINT_D("Releasing register file.");
    release_mem_region(physical_register_base, TLCD_MMIOALLOC);

    for (i = 0; i<drvdata->fb_num; i++) {
	info = drvdata->infos[i];

	if (fb_hard == 1) {
	    iounmap((void *)info->fix.smem_start);
	    release_mem_region(info->fix.smem_start, info->fix.smem_len);
	}
	else if (fb_hard == 0) {
	    /* clear page reserved; this is necessary */
	    for (page = (unsigned long)info->screen_base; page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size); page += PAGE_SIZE)
		ClearPageReserved(virt_to_page((void *)page));

	    /* and free the pages */
	    free_pages((unsigned long)info->screen_base, get_order(info->screen_size));
	}
	unregister_framebuffer(info);
	framebuffer_release(info);
    }
    drvdata->fb_num = 0;
    return 0;
}

static void thinklcdml_platform_release(struct device *device)
{
    /* this is called when the reference count goes to zero */
}


static struct platform_driver thinklcdml_driver = {
    .driver = {
	.owner = THIS_MODULE,
	.name  = "thinklcdml",
    },

    .probe  = thinklcdml_probe,
    .remove = thinklcdml_remove,
};

static struct platform_device thinklcdml_device = {
    .name = "thinklcdml",
    .id	  = 0,
    .dev  = {
	.release = thinklcdml_platform_release,
    }
};

int __init thinklcdml_init(void)
{
    int ret;
#ifndef MODULE
    char *options = NULL, *separator = ",";
#else
    char *options = module_options, *separator = ":";
#endif

    PRINT_PROC_ENTRY;
    printk("ThinkLCDML Multilayer kernel driver\n");


    /* our default mode is 800x480,LUT8 */
    default_var = m800x600;
    default_var.bits_per_pixel = 32;
//     default_var.red.offset = 11;

#ifndef MODULE
    /* get options from kernel command line and setup the driver */
    if (fb_get_options("thinklcdml", &options))
	return -ENODEV;
#endif

    thinklcdml_setup(options, separator);

    /* if the memory size has not been specified in the kernel command line try to allocate as much as we need */
    if (fb_memsize == 0)
	fb_memsize = PAGE_ALIGN(default_var.xres_virtual * default_var.yres_virtual * (default_var.bits_per_pixel >> 3));

    /* finish default mode setup */
    default_var.height   = -1,
	default_var.width    = -1,
	default_var.activate = FB_ACTIVATE_NOW,
	default_var.vmode    = FB_SYNC_VERT_HIGH_ACT;//FB_VMODE_NONINTERLACED;

    if (!(ret = platform_driver_register(&thinklcdml_driver))) {
	if ((ret = platform_device_register(&thinklcdml_device))) {
	    PRINT_W("Failed to register platform device '%s'\n", thinklcdml_device.name);
	    platform_driver_unregister(&thinklcdml_driver);
	}
    }
    else
	PRINT_W("Failed to register platform driver '%s'\n", thinklcdml_driver.driver.name);

    return ret;
}

module_init(thinklcdml_init);

#ifdef MODULE
static void __exit thinklcdml_exit(void)
{
    int i;
    PRINT_PROC_ENTRY;

    for (i=0;i<TLCDML_LAYERS_NUMBER;i++)
	platform_device_unregister(&thinklcdml_device);

    platform_driver_unregister(&thinklcdml_driver);
}

module_exit(thinklcdml_exit);

module_param(fb_hard, int, 0644);
MODULE_PARM_DESC(fb_hard, "Framebuffer memory is given by the user");
module_param(fb_memsize, int, 0644);
MODULE_PARM_DESC(fb_memsize, "Framebuffer memory size");
module_param(fb_addr, ulong, 0644);
MODULE_PARM_DESC(fb_addr, "Framebuffer memory base (only used if fb_hard=1)");
module_param(physical_register_base, ulong, 0644);
MODULE_PARM_DESC(physical_register_base, "Register base.");
module_param(module_options, charp, 0000);
MODULE_PARM_DESC(module_options, "Options just like in boot time.");

MODULE_AUTHOR("Think Silicon Ltd");
MODULE_DESCRIPTION("ThinkLCDML device driver");
#endif
