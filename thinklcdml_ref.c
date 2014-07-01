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
    unsigned long vbl_offset_address;        /* The phys framebuffer address in case we must pan on vblank */
    unsigned long vbl_layer_baseaddr; /* The baseaddress of the layer regfile. */
};

/* Per framebuffer tlcdml specific data */
struct tlcdml_fb_par {
    unsigned long phys_screenbase_reg;
};

/* Global driver data */
struct tlcdml_drvdata {
    struct fb_info** infos;	/* A stack of info pointers */
    u16 s_top;

    struct tlcdml_irq_data* irq_data;
};

/* Pinning of the memory. */
static u32 fb_hard;
static u32 physical_regs_base = TLCD_PHYSICAL_BASE;
static u32 register_file_size = TLCD_MMIOALLOC;
static u16 max_alloc_layers;

static void __iomem * virtual_regs_base;
static void __iomem * pll_virtual_regs_base;

#define pll_write(reg, value) iowrite32((value), (unsigned *)(pll_virtual_regs_base+(reg)))
#define pll_read(reg)   ioread32((unsigned *)(pll_virtual_regs_base+(reg)))

#define tlcdml_read(reg) fb_readl((u32 __iomem *)((u32)virtual_regs_base + (reg)))
#define tlcdml_write(reg, val) fb_writel(val, (u32 __iomem *)((unsigned long)(virtual_regs_base) + (reg)))

/* Printks */
#define PRINT_E(args...)	printk(KERN_ERR     "ThinkLCDML: " args)
#define PRINT_I(args...)	printk(KERN_INFO    "ThinkLCDML: " args)
#define PRINT_W(args...)	printk(KERN_WARNING "ThinkLCDML: " args)

#ifdef TLCD_DEBUG
#   define PRINT_D(fmt, args...) printk(KERN_ERR "ThinkLCDML: " fmt "\n", ##args)
#else
#   define PRINT_D(args...)	do { } while(0)
#endif


/* Declarations */
static int  thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
// static int thinklcdml_set_par(struct fb_info *info)
// static int thinklcdml_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info;)
// static int thinklcdml_mmap(struct fb_info *info, struct vm_area_struct *vma)
// static int thinklcdml_blank(int blank_mode, struct fb_info *info)
static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr);
static int thinklcdml_vsync(struct fb_info *info);
static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info);
static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg);
static int  thinklcdml_probe(struct platform_device *device);
static int __init thinklcdml_init(void);
static void thinklcdml_platform_release(struct device *device);
static int thinklcdml_remove(struct platform_device *device);
static void __exit thinklcdml_exit(void);

static struct fb_ops thinklcdml_ops = {
    .owner	        = THIS_MODULE,
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


static int tlcdml_default_fb_size(void)
{
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

static int tlcdml_hard_layer(struct fb_info *info, unsigned long hard_addr, unsigned long size)
{
    if (!request_mem_region(hard_addr, size, dev_name(info->dev))) {
	PRINT_E("Request mem region failed\n");
	return -ENOMEM;
    }

    if (!(info->screen_base = ioremap_nocache(hard_addr, size))) {
	PRINT_E("Failed to remap memory\n");
	release_mem_region(hard_addr, size);
	return -ENOMEM;
    }

    return 0;
}

#ifdef USE_CMA
static int tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    if ((info->screen_base = (char* __iomem)__get_free_pages(GFP_DMA | GFP_KERNEL, get_order(size))))
	return 0;

    return -ENOMEM;

}

static void tlcdml_dealloc_layer(struct fb_info *info)
{
    free_pages((unsigned long)info->screen_base, get_order(info->fix.smem_len));
}

#else

static int tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    if ((info->screen_base = dma_alloc_coherent(NULL, info->fix.smem_len, &info->fix.smem_start, GFP_KERNEL)))
	return 0;

    return -ENOMEM;
}

static void tlcdml_dealloc_layer(struct fb_info *info)
{
    dma_free_coherent(NULL, info->fix.smem_len, info->screen_base, &info->fix.smem_start);
}
#endif	/* USE_CMA */

static void tlcdml_setup(void)
{
}

static int  thinklcdml_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct thinklcdml_par *par = info->par;
    u_long line_length;

    PRINT_PROC_ENTRY;

    PRINT_D("Res: %dx%d\n", info->var.xres, info->var.yres);
    PRINT_D("Margins: L:%d, R:%d, U:%d, D:%d\n", info->var.left_margin, info->var.right_margin, info->var.upper_margin, info->var.lower_margin);
    PRINT_D("bpp: %d\n", info->var.bits_per_pixel);
    PRINT_D("offsets: R:%d, G:%d, B:%d, A:%d\n", info->var.red.offset, info->var.green.offset, info->var.blue.offset, info->var.transp.offset);
    PRINT_D("PixClock: %d\n", info->var.pixclock);

#ifdef USE_ONLY_DEFAULT_FBCONF
    if ( var->xres != DEFAULT_FBCONF.xres || var->yres != DEFAULT_FBCONF.yres ) {
//        PRINT_E("Not suppored resolution!\n");
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
    else {
        printk("Exiting @%d\n", __LINE__);
        return -EINVAL;
    }

#ifdef USE_ONLY_800x600
    if ( var->xres != 800 || var->yres != 600 ) {
//        PRINT_E("Not suppored resolution!\n");
        return -EINVAL;
    }
#endif

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
        if (var->transp.length == 1 && var->red.offset == 11 && var->green.offset == 6 && var->blue.offset == 1) {
            /* RGBA 5551 */
            var->red    = (struct fb_bitfield) { 11, 5, 0 };
            var->green  = (struct fb_bitfield) {  6, 5, 0 };
            var->blue   = (struct fb_bitfield) {  1, 5, 0 };
            var->transp = (struct fb_bitfield) {  0, 1, 0 };
        } else if (var->transp.length == 0 && var->red.offset == 11 && var->green.offset == 5 && var->blue.offset == 0) {
            /* RGB 565 */
            var->red    = (struct fb_bitfield) { 11, 5, 0 };
            var->green  = (struct fb_bitfield) {  5, 6, 0 };
            var->blue   = (struct fb_bitfield) {  0, 5, 0 };
            var->transp = (struct fb_bitfield) {  0, 0, 0 };
        } else {
            PRINT_W("Color mode not supported\n");
            return -EINVAL;
        }
        break;

    case 32:
        if (     /*var->transp.offset == 24 && */ var->red.offset == 16 && var->green.offset == 8 && var->blue.offset == 0) {
            /* ARGB 8888 */
            if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) { 24, 8, 0 };
            var->red    = (struct fb_bitfield) { 16, 8, 0 };
            var->green  = (struct fb_bitfield) {  8, 8, 0 };
            var->blue   = (struct fb_bitfield) {  0, 8, 0 };
        }
        else if (/*var->transp.offset == 24 && */ var->red.offset == 0 && var->green.offset == 8 && var->blue.offset == 16) {
            /* ABGR 8888*/
            if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) { 24, 8, 0 };
            var->red    = (struct fb_bitfield) {  0, 8, 0 };
            var->green  = (struct fb_bitfield) {  8, 8, 0 };
            var->blue   = (struct fb_bitfield) { 16, 8, 0 };
        }
        else if (/*var->transp.offset == 0 && */ var->red.offset == 8 && var->green.offset == 16 && var->blue.offset == 24) {
            /* BGRA 8888*/
            if ( var->transp.length != 0 )
		var->transp = (struct fb_bitfield) {  0, 8, 0 };
            var->red    = (struct fb_bitfield) {  8, 8, 0 };
            var->green  = (struct fb_bitfield) { 16, 8, 0 };
            var->blue   = (struct fb_bitfield) { 24, 8, 0 };
        }
        else if (/*var->transp.offset == 0 && */ var->red.offset == 24 && var->green.offset == 16 && var->blue.offset == 8) {
            /* RGBA 8888 */
            var->red    = (struct fb_bitfield) { 24, 8, 0 };
            var->green  = (struct fb_bitfield) { 16, 8, 0 };
            var->blue   = (struct fb_bitfield) {  8, 8, 0 };
            if ( var->transp.length != 0 )
                var->transp = (struct fb_bitfield) {  0, 8, 0 };
        } else {
            PRINT_W("Color mode not supported\n");
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
	tlcdml_write(data->vbl_layer_baseaddr, data->vbl_offset_address);
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
    struct tlcdml_fb_par *par = info->par;
    struct tlcdml_irq_data *irq_data = ((struct tlcdml_drvdata*)dev_get_drvdata(info->device))->irq_data;
    unsigned long phys_address;
    int ret = 0;

    PRINT_D("mode:%u xoffset:%u yoffset:%u", var->vmode, var->xoffset, var->yoffset);

    /* FIXME: check bounds */
    if (var->vmode & FB_VMODE_YWRAP) {
	PRINT_D("%s failed at 1.\n", __FUNCTION__);
	return -EINVAL;
    }

    if(var->xoffset + var->xres > info->var.xres_virtual) {
	PRINT_D("%s failed at 2.\n", __FUNCTION__);
	return -EINVAL;
    }

    if(var->yoffset + var->yres > info->var.yres_virtual) {
	PRINT_D("%s failed at 3.\n", __FUNCTION__);
	return -EINVAL;
    }

    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;
    /* compute new base address */
    phys_address = info->fix.smem_start +var->yoffset * info->fix.line_length + var->xoffset * (var->bits_per_pixel >> 3);


    /* Handle this at  vblank */
    if ( var->activate == FB_ACTIVATE_VBL ) {
	/* Tell the interrupt callback how to handle the situation */
	irq_data->vbl_layer_baseaddr = par->phys_screenbase_reg;
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
	    /* printk("TooFast\n"); */
	    thinklcdml_vsync(info);
	}
#endif
    }
    else {
	tlcdml_write(par->phys_screenbase_reg, phys_address);
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

	for ( offset = info->screen_base;
	      (unsigned long)offset < (unsigned long)info->screen_base + info->fix.line_length * info->var.yres_virtual;
	      offset += info->fix.line_length ) {
	    for ( i = 0; i < info->fix.line_length; i+=Bpp) {
		*(unsigned*)(offset+i) = color;
	    }
	}
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
		red   = CLAMP255(i+arg);
		green = CLAMP255(i+arg);
		blue  = CLAMP255(i+arg);
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
	PRINT_W("thinklcdml_ioctl: Unknown ioctl 0x%08x has been requested (arg: 0x%08lx)\n", cmd, arg);
	return -EINVAL;
    }
    return 0;
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

static inline int tlcdml_alloc_layers(struct platform_device* device, const unsigned long hard_phys_addr)
{
    struct tlcdml_drvdata* drvdata = platform_get_drvdata(device);
    struct fb_info *info;
    struct tlcdml_fb_par *par;
    unsigned long page;
    int ret = 0, fb_size = tlcdml_default_fb_size();

    if (hard_phys_addr) max_alloc_layers = 1;

    drvdata->infos = kzalloc(sizeof(struct fb_info*) * max_alloc_layers, GFP_KERNEL);
    if (!drvdata->infos) {
	return -ENOMEM;
    }

    do {
	/* Setup framebuffer info */
	info = framebuffer_alloc(sizeof(struct tlcdml_fb_par), &device->dev);
	if (!info) {
	    return -ENOMEM;
	}

	par = info->par;

	if ((ret = register_framebuffer(info)) < 0) {
	    framebuffer_release(info);
	    return ret;
	}

	/* First try hard_phys_addr, if that fails fallback to normal allocation. */
	if ((hard_phys_addr && (ret = tlcdml_hard_layer(info, hard_phys_addr, fb_size)) < 0) ||
	    ((ret = tlcdml_alloc_layer(info, fb_size)) < 0)) {
	    unregister_framebuffer(info);
	    framebuffer_release(info);
	    return ret;
	}

	info->fix.smem_start = virt_to_phys(info->screen_base);
	info->fix.smem_len = fb_size;

	/* Avoid page migration. */
	for (page = (unsigned long)info->screen_base;
	     page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size);
	     page += PAGE_SIZE)
	    SetPageReserved(virt_to_page((void *)page));

	memset(info->screen_base, 0, info->fix.smem_len);

	/* Setup tlcdml sepcific parameters */
	par->phys_screenbase_reg = TLCD_REG_LAYER_BASEADDR(drvdata->s_top);
    } while (tlcdml_push_info(info, drvdata));

    return ret;
}

static inline void tlcdml_dealloc_layers(struct platform_device* device)
{
    struct tlcdml_drvdata *drvdata = platform_get_drvdata(device);
    struct fb_info *info;
    unsigned long page;

    while((info = tlcdml_pop_info(drvdata))) {
	if (fb_hard) {
	    iounmap((void *)info->fix.smem_start);
	    release_mem_region(info->fix.smem_start, info->fix.smem_len);
	} else {
	    /* Unpin pages */
	    for (page = (unsigned long)info->screen_base;
		 page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size);
		 page += PAGE_SIZE)
		ClearPageReserved(virt_to_page((void*)page));

	    /* Deallocate framebuffer */
	    tlcdml_dealloc_layer(info);
	}

	unregister_framebuffer(info);
	framebuffer_release(info);
    }
}

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

static int __init thinklcdml_probe(struct platform_device *device)
{
    int ret = 0;
    struct tlcdml_drvdata *drvdata;

    /* DrvData */
    drvdata = kzalloc(sizeof(struct tlcdml_fb_par), GFP_KERNEL);
    if (!drvdata)
	return -ENOMEM;

    platform_set_drvdata(device, drvdata);

    /* Framebuffer setup */
    if ((ret = tlcdml_alloc_layers(device, fb_hard) < 0))
	goto layers_free;

    /* Registers */
    if (!request_mem_region(physical_regs_base, register_file_size, device->name)) {
	PRINT_E("Request for MMIO for register file was negative.\n");
	goto layers_free;
    }

    virtual_regs_base = ioremap_nocache(physical_regs_base, register_file_size);
    if (!virtual_regs_base) {
	PRINT_E("MMIO remap for register file failed\n");
	release_mem_region(physical_regs_base, register_file_size);
	ret = -ENOMEM;
	goto layers_free;
    }

    tlcdml_write(TLCD_REG_MODE, TLCD_MODE);

    /* IRQ */
    init_waitqueue_head(&drvdata->irq_data->irq_wait_vsync);
    if ((ret = request_irq(TLCD_VSYNC_IRQ,
			   thinklcdml_vsync_interrupt,
			   IRQF_DISABLED, "thinklcdml vsync",
			   drvdata->irq_data)) < 0)
	goto regs_free;

    /* PLL */
    if ((ret = tlcdml_setup_pll_pixclock(device) < 0))
	goto irq_free;

    return ret;

    /* Cleanup */
irq_free:
    free_irq(TLCD_VSYNC_IRQ, drvdata);

regs_free:
    iounmap(virtual_regs_base);
    release_mem_region(physical_regs_base, register_file_size);

layers_free:
    tlcdml_dealloc_layers(device);
    kfree(drvdata);

    return ret;

}

static int __init thinklcdml_init(void)
{
    int ret = 0;

    tlcdml_setup();

    if ((ret = platform_driver_probe(&thinklcdml_driver, thinklcdml_probe)))
	goto err_driver;

    if ((ret = platform_device_register(&thinklcdml_device)))
	goto err_device;

    return 0;

err_driver:
    PRINT_E("Failed to register platform driver %s\n", thinklcdml_driver.driver.name);
    return ret;

err_device:
    PRINT_E("Failed to register platform device %s\n", thinklcdml_device.name);
    return ret;
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
    free_irq(TLCD_VSYNC_IRQ, drvdata);

    iounmap(virtual_regs_base);
    release_mem_region(physical_regs_base, register_file_size);

    tlcdml_dealloc_layers(device);

    return 0;
}

static void __exit thinklcdml_exit(void)
{
    platform_driver_unregister(&thinklcdml_driver);
}

module_init(thinklcdml_init);
module_exit(thinklcdml_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Think Silicon Ltd");
MODULE_DESCRIPTION("ThinkLCDML device driver");
