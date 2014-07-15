/*
 * copyright 2014 Think-Silicon Ltd.
 * Author: Chris Perivolaropoulos
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.  You should
 * have received a copy of the GNU General Public License along with
 * this program.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

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


#ifdef BUILD_DATE
#define TLCDML_BUILD_DATE BUILD_DATE
#else
#define TLCDML_BUILD_DATE "<unknown>"
#endif

/* for CMA */
#ifdef USE_CMA
#include <linux/dma-mapping.h>
#endif

#define CLAMP255(x) ((x) > 255 ? 255 : (x))
#define FB_STACK_SIZE 3

/* Data passed to the irq interrupt callback */
struct tlcdml_irq_data {
    wait_queue_head_t irq_wait_vsync;
    int vbl_count;

    int vbl_active;	    /* Wether we should pan on vblank */
    struct fb_info* info;
    unsigned long vbl_offset_address;
};

/* Per framebuffer tlcdml specific data */
struct tlcdml_fb_par {
    int index;
    u32 mode;
};

/* Global driver data */
struct tlcdml_drvdata {
    struct fb_info** infos;	/* A stack of info pointers */
    u16 s_top;

    struct tlcdml_irq_data* irq_data;
};

/* Variables. */

static struct fb_fix_screeninfo thinklcdml_fix = {
    .id	       = "TSi ThinkLCDML",
    .type      =  FB_TYPE_PACKED_PIXELS,//FB_TYPE_PLANES,
    .xpanstep  = 1,
    .ypanstep  = 1,
    .ywrapstep = 0,
    .accel     = TLCD_ACCEL,
};

/* Pinning of the memory. */
static unsigned long fb_hard = 0;
static int fb_memsize = 0;

static struct fb_var_screeninfo default_var;

static char* module_options = NULL;
static u32 default_color_mode = TLCD_MODE_ARGB8888;

static u32 physical_regs_base = TLCD_PHYSICAL_BASE;
static u32 register_file_size = TLCD_MMIOALLOC;
static u16 max_alloc_layers;

static void __iomem * virtual_regs_base;

#ifdef USE_PLL
static void __iomem * pll_virtual_regs_base;
#endif

#define OL(info) (((struct tlcdml_fb_par*)(info)->par)->index)

#define XY16TOREG32(x, y) ((x) << 16 | ((y) & 0xffff))

#define pll_write(reg, value) iowrite32((value), (unsigned *)(pll_virtual_regs_base+(reg)))
#define pll_read(reg)   ioread32((unsigned *)(pll_virtual_regs_base+(reg)))

#define TLCDML_DUMMY
#ifdef TLCDML_DUMMY
#define tlcdml_read(reg) 0
#define tlcdml_write(reg, val) do {} while(0)
#else
#define tlcdml_read(reg) fb_readl((u32 __iomem *)((u32)(virtual_regs_base) + (reg)))
#define tlcdml_write(reg, val) fb_writel((val), (u32 __iomem *)((unsigned long)(virtual_regs_base) + (reg)))
#endif

/* Printks */
#define PRINT_E(fmt, args...)	printk(KERN_ERR     "[E] ThinkLCDML: " fmt "\n", ##args)
#define PRINT_I(fmt, args...)	printk(KERN_INFO    "[I] ThinkLCDML: " fmt "\n", ##args)
#define PRINT_W(fmt, args...)	printk(KERN_WARNING "[W] ThinkLCDML: " fmt "\n", ##args)

#define TLCD_DEBUG
#ifdef TLCD_DEBUG
#   define PRINT_D(fmt, args...) printk(KERN_ERR "[D] ThinkLCDML: " fmt "\n", ##args)
#else
#   define PRINT_D(args...)	do { } while(0)
#endif


/* Declarations */
static int  thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
static int thinklcdml_set_par(struct fb_info *info);
static int thinklcdml_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info);
static int thinklcdml_mmap(struct fb_info *info, struct vm_area_struct *vma);
static int thinklcdml_blank(int blank_mode, struct fb_info *info);
static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr);
static int thinklcdml_vsync(struct fb_info *info);
static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info);
static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg);
static int __init  thinklcdml_probe(struct platform_device *device);
static int __init thinklcdml_init(void);
static void thinklcdml_platform_release(struct device *device);
static int thinklcdml_remove(struct platform_device *device);
static void __exit thinklcdml_exit(void);

static struct fb_ops thinklcdml_ops = {
    .owner                          = THIS_MODULE,
    .fb_check_var                   = thinklcdml_check_var,
    .fb_set_par                     = thinklcdml_set_par,
    .fb_setcolreg                   = thinklcdml_setcolreg,
    .fb_pan_display                 = thinklcdml_pan_display,
    .fb_fillrect                    = cfb_fillrect,
    .fb_copyarea                    = cfb_copyarea,
    .fb_imageblit                   = cfb_imageblit,
    .fb_mmap                        = thinklcdml_mmap,
    .fb_blank                       = thinklcdml_blank,
    .fb_ioctl                       = thinklcdml_ioctl,
};

static struct platform_driver thinklcdml_driver = {
    .driver = {
	.owner = THIS_MODULE,
	.name  = "thinklcdml",
    },

    .remove = thinklcdml_remove,
};

static struct platform_device thinklcdml_device = {
    .name = "thinklcdml",
    .id	  = 0,
    .dev  = {
	.release = thinklcdml_platform_release,
    }
};


#ifdef USE_PLL
static inline int tlcdml_setup_pll_pixclock (const struct platform_device* device)
{
    struct tlcdml_drvdata

	/* Request MMIO for PixClkPll */
	if (!request_mem_region(PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC, device->name)) {
	    return -ENOMEM;
	}

    if ( pll_virtual_regs_base == NULL) {
	PRINT_D("Performing ioremap for PixClkPll registers (physical base: 0x%08lx, len: 0x%08x).", PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC);
	pll_virtual_regs_base = ioremap_nocache(PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC);
	if (!pixclkpll_v_regs_base) {
	    release_mem_region(PIXCLK_BASEADDR, PIXCLKPLL_MMIOALLOC);
	    return -ENOMEM;
	}
    }

    PRINT_I("PLL MAGIC = 0x%x (== 0xc350)", pll_read(0x210));

    pll_write(PIXCLKPLL_RESET, 0xa);
    pll_write(PIXCLKPLL_CLK0DIV, 0x00000032);
    udelay(1000000);
    pll_write(PIXCLKPLL_GLOBMULDIV, 0x00002801);
    udelay(1000000);
    pll_write(PIXCLKPLL_LOAD, 0x7);
    udelay(1000000);
    pll_write(PIXCLKPLL_LOAD, 0x2);
    udelay(1000000);

    return 0;
}

static inline int tlcdml_release_pll_pixclock (const struct platform_device* device)
{
    release_mem_region(PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC);
    iounmap(pll_virtual_regs_base);
}
#else
#define tlcdml_release_pll_pixclock()
#define tlcdml_setup_pll_pixclock(d) 0
#endif

static int __inline__ tlcdml_default_fb_size(void)
{
    if (fb_memsize)
	return fb_memsize;

    return 800*600*4;
}

static struct fb_info* tlcdml_pop_info(struct tlcdml_drvdata* drvdata)
{
    if (drvdata->s_top) {
	return drvdata->infos[--drvdata->s_top];
    }

    return NULL;
}

static int tlcdml_push_info(struct fb_info* info, struct tlcdml_drvdata* drvdata)
{
    if (drvdata->s_top >= max_alloc_layers)
	return 0;

    drvdata->infos[drvdata->s_top++] = info;
    return 1;
}

/* tlcdml_*_layer - allocate and populate

   - info->fix.smem_start
   - info->fix.smem_len
   - info->screen_base
*/
static int tlcdml_hard_layer(struct fb_info *info, unsigned long hard_addr, unsigned long size)
{
    info->fix.smem_len = info->screen_size = size;
    if (!request_mem_region(hard_addr, size, dev_name(info->dev))) {
	PRINT_E("Request mem region failed for fb hard addr %p", (void*)hard_addr);
	return -ENOMEM;
    }

    if (!(info->screen_base = ioremap_nocache(hard_addr, size))) {
	PRINT_E("Failed to remap memory");
	release_mem_region(hard_addr, size);
	return -ENOMEM;
    }

    info->fix.smem_start = virt_to_phys(info->screen_base);

    return 0;
}

#ifdef USE_CMA

static int tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    info->fix.smem_len = info->screen_size = size;
    if ((info->screen_base = dma_alloc_coherent(NULL, size, (dma_addr_t*)(&info->fix.smem_start), GFP_KERNEL)) == NULL)
	return -ENOMEM;

    return 0;
}

static void tlcdml_dealloc_layer(struct fb_info *info)
{
    kfree(info->pseudo_palette);
    dma_free_coherent(NULL, info->fix.smem_len, info->screen_base, (dma_addr_t)info->fix.smem_start);
}
#else
static  tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    info->fix.smem_len = info->screen_size = size;
    if ((info->screen_base = (char* __iomem)__get_free_pages(GFP_DMA | GFP_KERNEL, get_order(size)))) {
	return -ENOMEM;
    }

    return 0;
}

static void tlcdml_dealloc_layer(struct fb_info *info)
{
    kfree(info->pseudo_palette);
    free_pages((unsigned long)info->screen_base, get_order(info->fix.smem_len));
}
#endif	/* USE_CMA */


/* Module options:
   if custom:
   <defaults>#<pixclock>#<xres>#<xres_virtual>#<right_margin>#<hsync_len>#<left_margin>#<yres>#<yres_virtual>#<lower_margin>#<vsync_len>#<color_mode>

   <defaults> in {1024x768, 800x600, 640x480, 800x480, 1024x600, custom}
   and sets the initial values.

   Where # is eithe ',' or ':' depending on wether you are passing
   options on kernel or loading it as module respectively. You can stop
   at any point providing.
*/
static int __init tlcdml_setup(void)
{
    char *this_opt;
    int custom = 0;
    int count = 0;
#ifndef MODULE
    char *options = NULL, *separator = ",";
#else
    char *options = module_options, *separator = ":";
#endif

    if (!options || !*options) {
	default_var = DEFAULT_FBCONF;
	default_var.bits_per_pixel = 32;
	default_var.red.offset = 16;
	default_var.red.length = 8;
	default_var.green.offset = 8;
	default_var.green.length = 8;
	default_var.blue.offset = 0;
	default_var.blue.length = 8;
	default_var.transp.length = 24;
	default_var.transp.offset = 0;
	default_color_mode = TLCD_MODE_ARGB8888;

	/* Default to low resolution, Add video=thinklcdml:... to kernel command line */
	PRINT_I("No user setup options: Defaulting to %dx%d, bpp: %d, color mode: 0x%x", default_var.xres, default_var.yres, default_var.bits_per_pixel, default_color_mode);
	return 1;
    }

    while ((this_opt = strsep(&options, separator)) != NULL) {
	if (!*this_opt)
	    continue;

	if (custom) {
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
		if (!strcmp(this_opt, "LUT8")) {
		    default_var.bits_per_pixel = 8, default_var.red.offset = 0;
		    default_color_mode = TLCD_MODE_LUT8;
		} else if (!strcmp(this_opt, "RGB16")) {
		    default_var.bits_per_pixel = 16;
		    default_var.red.offset = 11;
		    default_var.red.length = 5;
		    default_var.green.offset = 6;
		    default_var.green.length = 5;
		    default_var.blue.offset = 1;
		    default_var.blue.length = 5;
		    default_var.transp.length = 1;
		    default_var.transp.offset = 0;
		    default_color_mode = TLCD_MODE_RGBA5551;

		} else if (!strcmp(this_opt, "RGB32")) {
		    /* default_var.bits_per_pixel = 32; */
		    /* default_var.red.offset = 24; */
		    /* default_var.red.length = 8; */
		    /* default_var.green.offset = 16; */
		    /* default_var.green.length = 8; */
		    /* default_var.blue.offset = 8; */
		    /* default_var.blue.length = 8; */
		    /* default_var.transp.length = 8; */
		    /* default_var.transp.offset = 0; */
		    /* default_color_mode = TLCD_MODE_RGBA8888; */

		    default_var.bits_per_pixel = 32;
		    default_var.red.offset = 16;
		    default_var.red.length = 8;
		    default_var.green.offset = 8;
		    default_var.green.length = 8;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 8;
		    default_var.transp.length = 24;
		    default_var.transp.offset = 0;
		    default_color_mode = TLCD_MODE_ARGB8888;
		} else if (!strcmp(this_opt, "RGB332")) {
		    default_var.bits_per_pixel = 8;
		    default_var.red.offset = 5;
		    default_var.red.length = 3;
		    default_var.green.offset = 2;
		    default_var.green.length = 3;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 2;
		    default_color_mode = TLCD_MODE_RGB332;
		} else if (!strcmp(this_opt, "RGBA565")) {
		    default_var.bits_per_pixel = 16;
		    default_var.red.offset = 11;
		    default_var.red.length = 5;
		    default_var.green.offset = 5;
		    default_var.green.length = 6;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 5;
		    default_color_mode = TLCD_MODE_RGBA565;
		} else if (!strcmp(this_opt, "ARGB8888")) {
		    default_var.bits_per_pixel = 32;
		    default_var.red.offset = 16;
		    default_var.red.length = 8;
		    default_var.green.offset = 8;
		    default_var.green.length = 8;
		    default_var.blue.offset = 0;
		    default_var.blue.length = 8;
		    default_color_mode = TLCD_MODE_ARGB8888;
		} else if (!strcmp(this_opt, "L8"))
		    default_var.bits_per_pixel = 8, default_var.grayscale = 1, default_color_mode = TLCD_MODE_L8;
		else {
		    PRINT_W("Unknown format '%s', defaulting to 8-bit palette mode", this_opt);
		    default_var.bits_per_pixel = 8, default_var.red.offset = 0;
		}
		break;

	    case 10: fb_memsize = PAGE_ALIGN(simple_strtoul(this_opt, NULL, 0)); break;
	    case 11: fb_hard = simple_strtoul(this_opt, NULL, 0); custom = 0; break;
	    }
	} else if (!strcmp(this_opt, "1024x768")) {
	    default_var = m1024x768_60;
	    custom = 1;
	    count = 9;
	} else if (!strcmp(this_opt, "800x600")) {
	    default_var = m800x600_60;
	    custom = 1;
	    count = 9;
	} else if (!strcmp(this_opt, "640x480")) {
	    default_var = m640x480_60;
	    custom = 1;
	    count = 9;
	} else if (!strcmp(this_opt, "800x480")) {
	    default_var = m800x480_60;
	    custom = 1;
	    count = 9;
	} else if (!strcmp(this_opt, "1024x600")) {
	    default_var = m1024x600_60;
	    custom = 1;
	    count = 9;
	} else if (!strcmp(this_opt, "custom")) {
	    custom = 1;
	    PRINT_I("Custom mode set");
	} else {
	    PRINT_W("Unknown mode '%s', defaulting to 1024x768 16-bit palette mode", this_opt);
	    return 1;
	}

    }

    return 1;
}

/**
 * Computes the surface pitch based on a resolution and bpp
 */
static __inline__ u_long tlcdml_get_line_length(int xres_virtual, int bpp)
{
    return ((xres_virtual * bpp + 31) & ~31) >> 3;
}

static __inline__ void tlcdml_dump_var(const struct fb_info *info)
{
    PRINT_D("FB VAR:");
    PRINT_D("Res: %dx%d", info->var.xres, info->var.yres);
    PRINT_D("Margins: L:%d, R:%d, U:%d, D:%d", info->var.left_margin, info->var.right_margin, info->var.upper_margin, info->var.lower_margin);
    PRINT_D("bpp: %d", info->var.bits_per_pixel);
    PRINT_D("offsets: R:%d, G:%d, B:%d, A:%d", info->var.red.offset, info->var.green.offset, info->var.blue.offset, info->var.transp.offset);
    PRINT_D("PixClock: %d", info->var.pixclock);
    PRINT_D("right_margin = %d", info->var.right_margin);
    PRINT_D("lower_margin = %d", info->var.lower_margin);
    PRINT_D("hsync_len = %d", info->var.hsync_len);
    PRINT_D("vsync_len = %d", info->var.vsync_len);
}

#ifdef USE_PLL
static __inline__ void tlcdml_setup_pll()
{
    if ( pixclkpll_v_regs_base == 0) {
	PRINT_D("Performing ioremap for PixClkPll registers (physical base: 0x%08lx, len: 0x%08x).", PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC);
	pixclkpll_v_regs_base = (unsigned long)ioremap_nocache(PIXCLKPLL_BASEADDR, PIXCLKPLL_MMIOALLOC);
	if (!pixclkpll_v_regs_base) {
	    PRINT_E("MMIO remap for PixClkPll register file failed");
	} else
	    PRINT_I("MMIO for PixClkPll register file PA:0x%08lx -> VA:0x%08lx len:%u", physical_register_base, virtual_regs_base, TLCD_MMIOALLOC);
    }

    PRINT_D("PLL MAGIC = 0x%x (== 0xc350)", pll_reg_read(0x210));
    PRINT_I("Programming PLL:");
    PRINT_I("1/5");
    pll_reg_write(PIXCLKPLL_RESET,        0xa);

    //  while ( pll_reg_read(PIXCLKPLL_STATUS)) ) ;
    volatile int delay = 1000000;
    PRINT_D("PIXCLKPLL_STATUS = 0x%08x ", pll_reg_read(PIXCLKPLL_STATUS));
    PRINT_I("2/5");
    PRINT_D("PIXCLKPLL_CLK0DIV = 0x%08x ", pll_reg_read(PIXCLKPLL_CLK0DIV));
    pll_reg_write(PIXCLKPLL_CLK0DIV,      0x00000032);
    delay = 1000000; while( delay-- );
    PRINT_I("3/5");
    PRINT_D("PIXCLKPLL_GLOBMULDIV = 0x%08x", pll_reg_read(PIXCLKPLL_GLOBMULDIV));

    u32 glmuldiv = (((u32) (1000000 / info->var.pixclock)) <<  8) | 0x01;
    PRINT_D("NEW PIXCLKPLL_GLOBMULDIV = 0x%08x", glmuldiv);

    pll_reg_write(PIXCLKPLL_GLOBMULDIV,   glmuldiv);
    delay = 1000000; while( delay-- );
    PRINT_I("4/5");
    pll_reg_write(PIXCLKPLL_LOAD,         0x7);
    delay = 1000000; while( delay-- );
    PRINT_I("5/5");
    pll_reg_write(PIXCLKPLL_LOAD,         0x2);
    delay = 1000000; while( delay-- );
    PRINT_I("Done Programming PLL!");
}
#else
#define tlcdml_setup_pll()
#endif	/* USE_PLL */

/**
 * tlcdml_setup_color_mode - Setup info and par to fit the color mode
 */
static int __inline__ tlcdml_setup_color_mode(struct fb_info* info)
{
    u32* mode = &((struct tlcdml_fb_par*)info->par)->mode;

    switch(info->var.bits_per_pixel) {
    case  8:
	info->fix.visual = info->var.red.offset ? FB_VISUAL_TRUECOLOR : FB_VISUAL_PSEUDOCOLOR;
	*mode = info->var.grayscale ? TLCD_MODE_L8 : (info->var.red.offset ? TLCD_MODE_RGB332 : TLCD_MODE_LUT8);
	break;
    case 16:
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	*mode = info->var.transp.length == 1 ? TLCD_MODE_RGBA5551 : TLCD_MODE_RGBA565;

	break;
    case 32:
	info->fix.visual = FB_VISUAL_TRUECOLOR;

	if (info->var.red.offset == 16 &&			\
	    info->var.green.offset == 8 &&			\
	    info->var.blue.offset == 0) {
	    *mode = TLCD_MODE_ARGB8888;
	    PRINT_I("Switching to ARGB8888");
	}
	else if (info->var.red.offset == 0  &&			\
		 info->var.green.offset == 8  &&		\
		 info->var.blue.offset == 16) {
	    *mode = TLCD_MODE_ABGR8888;
	    PRINT_I("Switching to ABGR8888");
	}
	else if (info->var.red.offset == 24 &&			\
		 info->var.green.offset == 16 &&		\
		 info->var.blue.offset == 8) {
	    *mode = TLCD_MODE_RGBA8888;
	    PRINT_I("Switching to RGBA8888");
	}
	else if ( info->var.red.offset == 8  && info->var.green.offset == 16 && info->var.blue.offset == 24 ) {
	    *mode = TLCD_MODE_BGRA8888;
	    PRINT_I("Switching to BGRA8888");
	}
	else {
	    PRINT_W("Unsupported mode: R:%d, G:%d, B:%d, A:%d. Switching to RGBA8888",
		    info->var.red.offset, info->var.green.offset,
		    info->var.blue.offset, info->var.transp.offset);
	    *mode = TLCD_MODE_RGBA8888;
	}
	break;
    default:
	PRINT_W("Unable to determine bits per pixel or color...");
	return -EINVAL;
    }

    return 0;
}

/* Allocate the memory and setup info. Does not touch registers. If
 * info_to_free is non-NULL just free that instead. of setting
 * anything up. */
static struct fb_info*
tlcdml_add_remove_info(struct platform_device* device, const unsigned long hard_phys_addr,
		       struct fb_info* info_to_free)
{
    struct tlcdml_drvdata* drvdata = platform_get_drvdata(device);
    struct fb_info *info;
    struct tlcdml_fb_par *par;
    unsigned long page;
    int ret = 0, fb_size = tlcdml_default_fb_size();

    if (info_to_free) {
	info = info_to_free;
	goto free_fb;
    }
    if (hard_phys_addr) max_alloc_layers = 1;

    /* Setup framebuffer info */
    PRINT_D("Setup framebuffer info");
    info = framebuffer_alloc(sizeof(struct tlcdml_fb_par), &device->dev);
    if (!info) {
	PRINT_E("Failed to allocate framebuffer info.");
	return NULL;
    }

    par			  = info->par;
    info->var             = default_var;
    info->fbops           = &thinklcdml_ops;
    info->fix             = thinklcdml_fix;
    info->fix.mmio_start  = physical_regs_base; /* TLCD_PHYSICAL_BASE; */
    info->fix.mmio_len    = TLCD_MMIOALLOC;
    info->fix.line_length = tlcdml_get_line_length(info->var.xres_virtual, info->var.bits_per_pixel);
    info->device          = &device->dev;
    par->index            = drvdata->s_top;

    PRINT_D("Register framebuffer");
    if ((ret = register_framebuffer(info)) < 0) {
	PRINT_E("Failed to register framebuffer info. (errno: %d)", ret);
	goto release_fb;
    }

    /* First try hard_phys_addr, if that fails fallback to normal allocation. */
    PRINT_D("First try hard_phys_addr, if that fails fallback to normal allocation.");
    if ((hard_phys_addr &&
	 tlcdml_hard_layer(info, hard_phys_addr, fb_size) < 0) ||
	(tlcdml_alloc_layer(info, fb_size) < 0)) {
	PRINT_E("Failed to allocate framebuffer memory.");
	goto unregister_fb;
    }

    /* Avoid page migration. Notice that this is done even on hard
     * address.*/
    for (page = (unsigned long)info->screen_base;
	 page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size);
	 page += PAGE_SIZE)
	SetPageReserved(virt_to_page((void *)page));

    memset(info->screen_base, 0, info->fix.smem_len);

    if (fb_alloc_cmap(&info->cmap, 256, 0) < 0) {
	PRINT_E("Failed to allocate color map.");
	goto unreserve_pg;
    }

    /* Setup info */
    PRINT_D("Setup info");
    if ((info->pseudo_palette = kzalloc(sizeof(u32) * TLCD_PALETTE_COLORS, GFP_KERNEL)) == NULL) {
	PRINT_E("Failed to allocate pseudo pallete.");
	goto free_cmap;
    }

    if ((ret = thinklcdml_check_var(&default_var, info))) {
	/* The parent will clean up after us. */
	PRINT_E("Failed to check default var.");
	goto free_palette;
    }

    return info;

free_fb:			/* Jump here to just free everything. */
    PRINT_D("Freing layer");
free_palette:
    PRINT_D("Freing palette.");
    kfree(info->pseudo_palette);

free_cmap:
    PRINT_D("Freing cmap.");
    fb_dealloc_cmap(&info->cmap);

unreserve_pg:
    PRINT_D("Unreserving pages.");
    for (page = (unsigned long)info->screen_base;
	 page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size);
	 page += PAGE_SIZE)
	ClearPageReserved(virt_to_page((void*)page));

    if (hard_phys_addr) {
	iounmap((void *)info->fix.smem_start);
	release_mem_region(info->fix.smem_start, info->fix.smem_len);
    } else
	tlcdml_dealloc_layer(info);

unregister_fb:
    PRINT_D("Unregistgering framebuffer.");
    unregister_framebuffer(info);

release_fb:
    PRINT_D("Framebuffer release.");
    framebuffer_release(info);

    return NULL;
}


/**
 * tlcdml_alloc_layers - Allocate and setup framebuffers.
 *
 * @device: The platform device to be set as parent.
 * @hard_phys_addr: The physical address to pin the (single) framebuffer or NULL.
 *
 * If @hard_phys_addr is not NULL then max_alloc_layers is set to 1.
 *
 */

static int __inline__ tlcdml_alloc_layers(struct platform_device* device, const unsigned long hard_phys_addr)
{
    struct tlcdml_drvdata *drvdata = platform_get_drvdata(device);
    struct fb_info *info;

    do {
	/* Try to allocate a layer. */
	if ((info = tlcdml_add_remove_info(device, hard_phys_addr, NULL)) == NULL) {
	    PRINT_E("Failed to add layer %d.", drvdata->s_top);
	    return -ENOMEM;
	}
	/* Setup registers */
	tlcdml_write(TLCD_REG_LAYER_SCALEY(OL(info)),  0x4000);
	tlcdml_write(TLCD_REG_LAYER_SCALEX(OL(info)),  0x4000);
	tlcdml_write(TLCD_REG_LAYER_BASEADDR(OL(info)), info->fix.smem_start);
	tlcdml_write(TLCD_REG_LAYER_MODE(OL(info)),
		     ((OL(info) ? 0 : TLCD_CONFIG_ENABLE) | TLCD_MODE | default_color_mode ));
    } while (tlcdml_push_info(info, drvdata));

    return 0;
}


static inline void tlcdml_dealloc_layers(struct platform_device* device)
{
    struct tlcdml_drvdata *drvdata = platform_get_drvdata(device);
    struct fb_info *info;

    while((info = tlcdml_pop_info(drvdata)))
	tlcdml_add_remove_info(device, fb_hard, info);
}

static int thinklcdml_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info)
{
    u32 out_val;

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

	out_val = (red << info->var.red.offset) |			\
	    (green << info->var.green.offset) |				\
	    (blue << info->var.blue.offset);
    } else {
	red    >>= 8;
	green  >>= 8;
	blue   >>= 8;
	transp >>= 8;

	out_val = (red << 16) | (green << 8) | (blue << 0);
    }

    /* the pseudo_palette expects color values in screen format, computed as seen above */
    ((u32*)info->pseudo_palette)[regno] = out_val;

    /* the hardware always expects an RGB888 value */
    tlcdml_write(TLCD_PALETTE_OFFSET + regno * 4, out_val); // (red << 16) | (green << 8) | (blue << 0));
    return 0;
}

/**
   Apply the registers according to the par of this info.
   `thinklcdml_check_var' does something like this only it populates
   var instead of setting up the hw.
*/
static int thinklcdml_set_par(struct fb_info *info)
{
    int resx, resy, frontporchx, frontporchy, blankx, blanky, backporchx, backporchy;
    u32 i, mask;
    u8 red, green, blue;
    u32* mode = &((struct tlcdml_fb_par*)info->par)->mode;

    tlcdml_dump_var(info);

    mask        = info->var.bits_per_pixel == 8 ? ~0 : ~TLCD_MODE_LUT8;
    resx        = info->var.xres;
    resy        = info->var.yres;
    frontporchx = resx + info->var.right_margin;
    frontporchy = resy + info->var.lower_margin;
    blankx      = frontporchx + info->var.hsync_len;
    blanky      = frontporchy + info->var.vsync_len;
    backporchx  = blankx + info->var.left_margin;
    backporchy  = blanky + info->var.upper_margin;

    tlcdml_write(TLCD_REG_LAYER_STARTXY(OL(info)), 0x00000000 );
    tlcdml_write(TLCD_REG_LAYER_RESXY(OL(info)),  XY16TOREG32(resx, resy));
    tlcdml_write(TLCD_REG_RESXY,                  XY16TOREG32(resx, resy));
    tlcdml_write(TLCD_REG_LAYER_SIZEXY(OL(info)), XY16TOREG32(resx, resy));
    tlcdml_write(TLCD_REG_FRONTPORCHXY,           XY16TOREG32(frontporchx, frontporchy));
    tlcdml_write(TLCD_REG_BLANKINGXY,             XY16TOREG32(blankx, blanky));
    tlcdml_write(TLCD_REG_BACKPORCHXY,            XY16TOREG32(backporchx, backporchy));

    info->fix.line_length = tlcdml_get_line_length(info->var.xres_virtual, info->var.bits_per_pixel);

    /* Decide on color mode */
    if (tlcdml_setup_color_mode(info))
	return -EINVAL;

    if (unlikely(info->var.bits_per_pixel == 8 && (*mode & 0x7) == TLCD_MODE_L8)) {
	/* The odds of it actually being grayscale or 332 are quite
	 * slim... so do this anyway */
	for(i = 0; i < 256; i++) {
	    red = green = blue = CLAMP255(i);
	    tlcdml_write(TLCD_PALETTE_OFFSET + i * 4,
			 (red << 16) | (green << 8) | (blue << 0));
	}
	tlcdml_write(TLCD_REG_MODE, *mode | TLCD_CONFIG_GAMMA);
    }

    /* Get reg mode */
    /* XXX: mode 80000000, front proch */
    *mode |= (tlcdml_read(TLCD_REG_MODE) & mask) | TLCD_CONFIG_ENABLE | (0xff << 16);

    tlcdml_write(TLCD_REG_MODE , TLCD_DEFAULT_MODE );
    tlcdml_write(TLCD_REG_CLKCTRL, TLCD_CLKCTRL);
    tlcdml_write(TLCD_REG_BGCOLOR , TLCD_BGCOLOR);

    PRINT_D("Actually enabling fb%d", OL(info));
    /* Enable, global full alpha, color mode as defined. */
    tlcdml_write(TLCD_REG_LAYER_MODE(OL(info)), *mode);
    tlcdml_write(TLCD_REG_LAYER_STRIDE(OL(info)), info->fix.line_length);
    /* tlcdml_write(TLCD_REG_LAYER_STRIDE(OL(info)), 0x4ec0); */

    tlcdml_setup_pll();
    return 0;
}

static int thinklcdml_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    if (vma->vm_pgoff)
	return -EINVAL;

    if ((vma->vm_end - vma->vm_start) > info->fix.smem_len) {
	PRINT_E("Failed to mmap, %lu bytes requested", vma->vm_end - vma->vm_start);
	return -EINVAL;
    }

    /* Masks page protection with non-cached if there is mmu going
     * on. */
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vma->vm_flags |= VM_IO | VM_MAYSHARE | VM_SHARED;

    if (io_remap_pfn_range(vma, vma->vm_start, info->fix.smem_start >> PAGE_SHIFT, vma->vm_end - vma->vm_start, vma->vm_page_prot))
	return -EAGAIN;

    return 0;
}

/* REFACTOR */
/* This does not clear the framebuffer, just the screen. */
static int
thinklcdml_blank(int blank_mode, struct fb_info *info)
{
    u32 mode_reg;

    PRINT_D("blank: %d", blank_mode);

    /* XXX: nothing is being done (nor in the original) */
    /* blank out the screen by setting or clearing TLCD_MODE bit 31 */
    mode_reg = tlcdml_read(TLCD_REG_MODE);

    if (blank_mode == 0)
	; //tlcdml_write(par->regs, TLCD_REG_MODE, mode_reg | 1<<19);
    else if (blank_mode == 1)
	; //tlcdml_write(par->regs, TLCD_REG_MODE, mode_reg & ~(1<<19));
    else
	return -EINVAL;

    return 0;
}



static int  thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    u_long line_length;

    PRINT_D("Res: %dx%d", info->var.xres, info->var.yres);
    PRINT_D("Margins: L:%d, R:%d, U:%d, D:%d",
	    info->var.left_margin, info->var.right_margin,
	    info->var.upper_margin, info->var.lower_margin);
    PRINT_D("bpp: %d", info->var.bits_per_pixel);
    PRINT_D("offsets: R:%d, G:%d, B:%d, A:%d",
	    info->var.red.offset, info->var.green.offset,
	    info->var.blue.offset, info->var.transp.offset);
    PRINT_D("PixClock: %d", info->var.pixclock);

#ifdef DEFAULT_FBCONF
    if ( var->xres != DEFAULT_FBCONF.xres || var->yres != DEFAULT_FBCONF.yres ) {
	return -EINVAL;
    }

    var->pixclock       = DEFAULT_FBCONF.pixclock;
    var->left_margin    = DEFAULT_FBCONF.left_margin;
    var->right_margin   = DEFAULT_FBCONF.right_margin;
    var->upper_margin   = DEFAULT_FBCONF.upper_margin;
    var->lower_margin   = DEFAULT_FBCONF.lower_margin;
    var->hsync_len      = DEFAULT_FBCONF.hsync_len;
    var->vsync_len      = DEFAULT_FBCONF.vsync_len;
#endif


    /*  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
     *  as FB_VMODE_SMOOTH_XPAN is only used internally */
    if (var->vmode & FB_VMODE_CONUPDATE) {
	/* var->vmode |= FB_VMODE_YWRAP; */
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
	PRINT_W("Tried to set zeero bits per pixel.");

    /* Sanity check bpp */
    if (var->bits_per_pixel <= 8)
	var->bits_per_pixel = 8;
    else if (var->bits_per_pixel <= 16)
	var->bits_per_pixel = 16;
    else if (var->bits_per_pixel <= 32)
	var->bits_per_pixel = 32;
    else {
	printk("Exiting @%d", __LINE__);
	return -EINVAL;
    }

    /* Virtual resolution setup. */
    if (var->xres_virtual < var->xoffset + var->xres)
	var->xres_virtual = var->xoffset + var->xres;
    if (var->yres_virtual < var->yoffset + var->yres)
	var->yres_virtual = var->yoffset + var->yres;

    /* Check the new memory size. */
    line_length = tlcdml_get_line_length(var->xres_virtual, var->bits_per_pixel);
    if (line_length * var->yres_virtual > info->screen_size) {
	PRINT_W("Bad mode: out of memory (virtual:%ux%u bpp:%u, allocated: 0x%lx)",
		var->xres_virtual, var->yres_virtual, var->bits_per_pixel, info->screen_size);
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
	if (var->transp.length == 1 &&					\
	    var->red.offset == 11 &&					\
	    var->green.offset == 6 &&					\
	    var->blue.offset == 1) {
	    /* RGBA 5551 */
	    var->red    = (struct fb_bitfield) { 11, 5, 0 };
	    var->green  = (struct fb_bitfield) {  6, 5, 0 };
	    var->blue   = (struct fb_bitfield) {  1, 5, 0 };
	    var->transp = (struct fb_bitfield) {  0, 1, 0 };
	} else if (var->transp.length == 0 &&				\
		   var->red.offset == 11 &&				\
		   var->green.offset == 5 &&				\
		   var->blue.offset == 0) {
	    /* RGB 565 */
	    var->red    = (struct fb_bitfield) { 11, 5, 0 };
	    var->green  = (struct fb_bitfield) {  5, 6, 0 };
	    var->blue   = (struct fb_bitfield) {  0, 5, 0 };
	    var->transp = (struct fb_bitfield) {  0, 0, 0 };
	} else {
	    PRINT_W("Color mode not supported");
	    return -EINVAL;
	}
	break;

    case 32:
	if (/*var->transp.offset == 24 && */				\
	    var->red.offset == 16 &&					\
	    var->green.offset == 8 &&					\
	    var->blue.offset == 0) {
	    /* ARGB 8888 */
	    if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) { 24, 8, 0 };

	    var->red    = (struct fb_bitfield) { 16, 8, 0 };
	    var->green  = (struct fb_bitfield) {  8, 8, 0 };
	    var->blue   = (struct fb_bitfield) {  0, 8, 0 };
	}
	else if (/*var->transp.offset == 24 && */			\
	    var->red.offset == 0 &&					\
	    var->green.offset == 8 &&					\
	    var->blue.offset == 16) {
	    /* ABGR 8888*/
	    if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) { 24, 8, 0 };

	    var->red    = (struct fb_bitfield) {  0, 8, 0 };
	    var->green  = (struct fb_bitfield) {  8, 8, 0 };
	    var->blue   = (struct fb_bitfield) { 16, 8, 0 };
	}
	else if (/*var->transp.offset == 0 && */			\
	    var->red.offset == 8 &&					\
	    var->green.offset == 16 &&					\
	    var->blue.offset == 24) {
	    /* BGRA 8888*/
	    if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) {  0, 8, 0 };

	    var->red    = (struct fb_bitfield) {  8, 8, 0 };
	    var->green  = (struct fb_bitfield) { 16, 8, 0 };
	    var->blue   = (struct fb_bitfield) { 24, 8, 0 };
	}
	else if (/*var->transp.offset == 0 && */			\
	    var->red.offset == 24 &&					\
	    var->green.offset == 16 &&					\
	    var->blue.offset == 8) {
	    /* RGBA 8888 */
	    var->red    = (struct fb_bitfield) { 24, 8, 0 };
	    var->green  = (struct fb_bitfield) { 16, 8, 0 };
	    var->blue   = (struct fb_bitfield) {  8, 8, 0 };

	    if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) {  0, 8, 0 };
	} else {
	    PRINT_W("Color mode not supported");
	    //            return -EINVAL;
	}

	break;
    }

    return 0;
}

#define SET_COLOR_MODE(var, c1, c2, c3, c4, l1, l2, l3, l4) do {	\
	(var)->#c4 = (struct fb_bitfield) {  0, (l4), 0 };		\
	(var)->#c3 = (struct fb_bitfield) {  (l4), (l3), 0 };		\
	(var)->#c2 = (struct fb_bitfield) {  (l4) + (l3), (l2), 0 };	\
	(var)->#c1 = (struct fb_bitfield) {  (l4) + (l3) + (l2), (l1), 0 }; \
    } while (0)

static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr)
{
    struct tlcdml_irq_data* data = (struct tlcdml_irq_data*)ptr;

    /* clear the interrupt */
    tlcdml_write(TLCD_REG_INTERRUPT, 0);

    /* update stats, also needed as a condition to unblock */
    data->vbl_count++;

    if ( data->vbl_active == 1 ) {
	tlcdml_write(TLCD_REG_LAYER_BASEADDR(OL(data->info)), data->vbl_offset_address);
	data->vbl_active = 0;
    }

    /* wake up any threads waiting */
    wake_up_interruptible(&data->irq_wait_vsync);

    return IRQ_HANDLED;
}

/* Force vsync to happen. */
static int thinklcdml_vsync(struct fb_info *info)
{
    struct tlcdml_drvdata* drvdata = dev_get_drvdata(info->device);
    struct tlcdml_irq_data* irq_data = drvdata->irq_data;
    u64 count;

    /* enable vsync interrupt; it will be cleared on arrival */
    count = irq_data->vbl_count;

    /* Raise the interrupt and wait */
    tlcdml_write(TLCD_REG_INTERRUPT, 0x1);
    if (!wait_event_interruptible_timeout(irq_data->irq_wait_vsync, count != irq_data->vbl_count, HZ / 10)) {
	return -ETIMEDOUT;
    }

    return 0;
}

static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct tlcdml_irq_data *irq_data = ((struct tlcdml_drvdata*)dev_get_drvdata(info->device))->irq_data;
    unsigned long phys_address;
    int ret = 0;

    PRINT_D("mode:%u xoffset:%u yoffset:%u", var->vmode, var->xoffset, var->yoffset);

    /* FIXME: check bounds */
    if (var->vmode & FB_VMODE_YWRAP) {
	PRINT_E("%s failed. vmode is FB_VMODE_YWRAP.", __FUNCTION__);
	return -EINVAL;
    }

    if(var->xoffset + var->xres > info->var.xres_virtual) {
	PRINT_E("%s failed. Screen out of virtual on x.", __FUNCTION__);
	return -EINVAL;
    }

    if(var->yoffset + var->yres > info->var.yres_virtual) {
	PRINT_E("%s failed. Screen out of virtual on y.", __FUNCTION__);
	return -EINVAL;
    }

    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;

    /* compute new base address */
    phys_address = info->fix.smem_start + var->yoffset * info->fix.line_length + \
	var->xoffset * (var->bits_per_pixel >> 3);

    /* Handle this at vblank */
    if ( var->activate == FB_ACTIVATE_VBL ) {
	/* Tell the interrupt callback how to handle the situation */
	irq_data->info = info;
	irq_data->vbl_offset_address = phys_address;

	if (irq_data->vbl_active != 1) {
	    irq_data->vbl_active = 1;
	    tlcdml_write(TLCD_REG_INTERRUPT, 0x1);
	}
#ifdef SYNC_ON_HIFPS
	else {
	    /* Very High FPS - Normally, the two back buffers should
	       be swapped but this function is not supported by
	       fbdev... so just wait for vsync */
	    /* printk("TooFast"); */
	    thinklcdml_vsync(info);
	}
#endif
    }
    else {
	tlcdml_write(TLCD_REG_LAYER_BASEADDR(par->index), phys_address);
    }

    return ret;
}


static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    void* offset;
    unsigned Bpp, color, ret;
    int red, green, blue, i,mode;

    switch (cmd) {
    case TLCDML_SET_REGISTER:
	tlcdml_write(*((unsigned long*)arg), *(((unsigned long*)arg)+1));
	break;

    case TLCDML_GET_REGISTER:
	((unsigned long*)arg)[1] = tlcdml_read(arg);
	break;

    case TLCDML_COLOR_CLEAR:
	Bpp = info->var.bits_per_pixel/8;
	color = arg << (8*( sizeof(unsigned long) - Bpp));

	memset(info->screen_base, 0, info->fix.line_length * info->var.yres_virtual );

	for (offset = info->screen_base;
	     (unsigned long)offset < (unsigned long)info->screen_base + \
		 info->fix.line_length * info->var.yres_virtual;
	     offset += info->fix.line_length)
	    for ( i = 0; i < info->fix.line_length; i+=Bpp)
		*(unsigned*)(offset+i) = color;

	break;

    case TLCDML_GET_INFO:
	memcpy((void*)arg, &info->var, sizeof(struct fb_var_screeninfo));
	break;

    case FBIO_WAITFORVSYNC:
	ret = thinklcdml_vsync(info);
	return ret;
	break;

    case FBIO_RAMP_SET:
	mode = tlcdml_read(TLCD_REG_MODE);

	if(arg==0)
	    tlcdml_write(TLCD_REG_MODE, mode & ~TLCD_CONFIG_GAMMA);
	else {
	    for(i=0; i<256; i++) {
		red = green = blue = CLAMP255(i+arg);
		tlcdml_write(TLCD_PALETTE_OFFSET + i * 4, (red << 16) | (green << 8) | (blue << 0));
	    }
	    tlcdml_write(TLCD_REG_MODE, mode | TLCD_CONFIG_GAMMA);
	}
	return 1;

    case TLCDML_SWAP_BUFFERS:
	PRINT_D("ioctl: TLCDML_SWAP_BUFFERS");

	if(BUFFERS_PER_LAYER == 1)
	    return 0;

	/* XXX: Prone to bugs. I think we'll be ok though */
	info->var.yoffset += info->var.yres;
	info->var.yoffset %= info->fix.line_length;

	if(!thinklcdml_pan_display(&info->var, info))
	    /* Return at which framebuffer we are at. */
	    return info->var.yoffset / info->fix.line_length;
	else
	    return -1;

    case TLCDML_GET_CURRENT_BUFFER:
	PRINT_D("ioctl: TLCDML_GET_CURRENT_BUFFER");
	return info->var.yoffset / info->var.yres;

    case FBIO_I2C_W:
	PRINT_D("ioctl: FBIO_I2C_W");
	return 0;

    case FBIO_I2C_R:
	PRINT_D("ioctl: FBIO_I2C_R");
	return 0;

    case FBIO_SIF_W:
	PRINT_D("ioctl: FBIO_SIF_W");
	return 0;

    case FBIO_SIF_R:
	PRINT_D("ioctl: FBIO_SIF_R");
	return 0;

    default:
	PRINT_W("thinklcdml_ioctl: Unknown ioctl 0x%08x has been requested (arg: 0x%08lx)", cmd, arg);
	return -EINVAL;
    }
    return 0;
}

static int __init thinklcdml_probe(struct platform_device *device)
{
    int ret = 0;
    struct tlcdml_drvdata *drvdata;

    PRINT_D("Probing %s", device->name);

    /* DrvData */
    PRINT_D("DrvData");
    drvdata = kzalloc(sizeof(struct tlcdml_fb_par), GFP_KERNEL);
    if (!drvdata) {
	PRINT_E("Failed to allocate driver data for %s", device->name);
	return -ENOMEM;
    }

    drvdata->irq_data = kzalloc(sizeof(struct tlcdml_irq_data), GFP_KERNEL);
    if (!drvdata->irq_data) {
	PRINT_E("Failed to allocate driver irq data for %s", device->name);
	kfree(drvdata);
	return -ENOMEM;
    }
    platform_set_drvdata(device, drvdata);

    /* Framebuffer setup */
    PRINT_D("Framebuffer setup");
    if ((ret = tlcdml_alloc_layers(device, fb_hard) < 0)) {
	PRINT_E("Layer allocation failed.");
	goto drv_free;
    }

    /* Allocate registers */
    PRINT_D("Allocate registers");
    if (!request_mem_region(physical_regs_base, register_file_size, device->name)) {
	PRINT_E("Request for MMIO for register file was negative.");
	goto layers_free;
    }

    virtual_regs_base = ioremap_nocache(physical_regs_base, register_file_size);
    if (!virtual_regs_base) {
	PRINT_E("MMIO remap for register file failed");
	release_mem_region(physical_regs_base, register_file_size);
	ret = -ENOMEM;
	goto regs_release;
    }

    tlcdml_write(TLCD_REG_MODE, TLCD_MODE);

    /* IRQ */
    PRINT_D("IRQ");
    init_waitqueue_head(&drvdata->irq_data->irq_wait_vsync);
    if ((ret = request_irq(TLCD_VSYNC_IRQ,
			   thinklcdml_vsync_interrupt,
			   IRQF_DISABLED, "thinklcdml vsync",
			   drvdata->irq_data)) < 0) {
	PRINT_E("IRQ request failed.");
	goto regs_free;
    }
    /* PLL */
    PRINT_D("PLL");
    if ((ret = tlcdml_setup_pll_pixclock(device) < 0)) {
	PRINT_E("Failerd pll pixclock setup.");
	goto irq_free;
    }

    /* Setup Registers */
    return ret;

    /* Cleanup */
irq_free:
    PRINT_D("Freing irq.");
    free_irq(TLCD_VSYNC_IRQ, drvdata->irq_data);

regs_free:
    PRINT_D("Freing regs.");
    iounmap(virtual_regs_base);

regs_release:
    PRINT_D("Releasing regs.");
    release_mem_region(physical_regs_base, register_file_size);

layers_free:
    PRINT_D("Freing layers.");
    tlcdml_dealloc_layers(device);

drv_free:
    PRINT_D("Freing driver data.");
    kfree(drvdata->irq_data);
    kfree(drvdata);

    return ret;
}

static int __init thinklcdml_init(void)
{
    int ret = 0;

    PRINT_D("Build: %s", TLCDML_BUILD_DATE );
    tlcdml_setup();

    if ((ret = platform_device_register(&thinklcdml_device))) {
	PRINT_E("Failed to register platform device %s (errno: %d)", thinklcdml_device.name, ret);
	return ret;
    }

    if ((ret = platform_driver_probe(&thinklcdml_driver, thinklcdml_probe))){
	PRINT_E("Failed to register platform driver %s (errno: %d)", thinklcdml_driver.driver.name, ret);
	platform_device_unregister(&thinklcdml_device);
	return ret;
    }

    return 0;
}


/* REMOVAL */
static void thinklcdml_platform_release(struct device *device)
{
    /* this is called when the reference count goes to zero */
}

static int thinklcdml_remove(struct platform_device *device)
{
    struct tlcdml_drvdata* drvdata = platform_get_drvdata(device);

    tlcdml_release_pll_pixclock();

    tlcdml_write(TLCD_REG_INTERRUPT, 0);
    free_irq(TLCD_VSYNC_IRQ, drvdata->irq_data);

    iounmap(virtual_regs_base);
    release_mem_region(physical_regs_base, register_file_size);

    tlcdml_dealloc_layers(device);
    kfree(drvdata->irq_data);
    kfree(drvdata);

    return 0;
}

static void __exit thinklcdml_exit(void)
{
    platform_device_unregister(&thinklcdml_device);
    platform_driver_unregister(&thinklcdml_driver);
}

module_init(thinklcdml_init);
module_exit(thinklcdml_exit);

module_param(fb_memsize, int, 0644);
MODULE_PARM_DESC(fb_memsize, "Framebuffer memory size");
module_param(fb_hard, ulong, 0644);
MODULE_PARM_DESC(fb_hard, "Framebuffer memory base.");
module_param(module_options, charp, 0000);
MODULE_PARM_DESC(module_options, "Options just like in boot time.");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Think Silicon Ltd");
MODULE_DESCRIPTION("ThinkLCDML device driver");
