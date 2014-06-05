/*
 * Copyright 2014 Think-Silicon Ltd.
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

#define FB_STACK_SIZE 3

struct tlcdml_irq_data {
    wait_queue_head_t wait_vsync;
    int vblank_count;

    int vbl_activate;	    /* Wether we should pan on vblank */
    unsigned long long vbl_address;        /* The phys framebuffer address in case we must pan on vblank */
    unsigned long long vbl_layer_baseaddr; /* The baseaddress of the layer regfile. */
}

struct tlcdml_drvdata {
    struct fb_info** infos;	/* A stack of info pointers */
    u16 stop;

    struct tlcdml_irq_data* irq_data;
};

/* Pinning of the memory. */
static u32 fb_hard;
static u16 max_alloc_layers;

static void __iomem * virtual_regs_base;
static void __iomem * pll_virtual_regs_base;

#define pll_write(reg, value) iowrite32((value), (unsigned *)(pll_virtual_regs_base+(reg)))
#define pll_read(reg)   ioread32((unsigned *)(pll_virtual_regs_base+(reg)))

#define tlcdml_read(reg) fb_readl((u32 __iomem *)((virtual_regs_base) + (offset)))
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

static struct fb_info* tlcdml_pop_info(struct tcdml_drvdata* drvdata)
{
    if (drvdata->s_top) {
	return drvdata->infos[--drvdata->s_top];
    }

    return NULL;
}

static int tlcdml_push_info(struct fb_info* info, tcdml_drvdata* drvdata)
{
    if (drvdata->s_top >= max_alloc_layers)
	return 0;

    drvdata->infos[drvdata->s_top++] = info;
    return 1;
}

#ifdef USE_CMA
/* Fill in to info the fix.smem_len. Returns -ENOMEM on failure to
 * allocate. */
static int tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    info->fix.screen_base = (unsigned long) __get_free_pages(GFP_DMA | GFP_KERNEL, get_order(size));
    if (!info->screen_base) {
	return -ENOMEM;
    }

    info->fix.smem_start = virt_to_phys(info->screen_base);
    info->fix.smem_len = size;

    return 0;
}

static void tlcdml_dealloc_layer(struct fb_info *info)
{
    free_pages(info->screen_base, get_order(info->fix.smem_len));
}

#else

static int tlcdml_alloc_layer(struct fb_info *info, unsigned long size)
{
    info->screen_base = dma_alloc_coherent(NULL, info->fix.smem_len, &info->fix.smem_start, GFP_KERNEL);
    if (info->screen_base)
	return 0;

    info->fix.smem_start = NULL;
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

static irqreturn_t thinklcdml_vsync_interrupt(int irq, void *ptr)
{
    struct tlcdml_irq_data* data = *ptr;

    /* clear the interrupt */
    tlcdml_write(TLCD_REG_INTERRUPT, 0);

    /* update stats, also needed as a condition to unblock */
    data->vblank_count++;

    if ( data->activate_vbl == 1 ) {
        think_writel(virtual_regs_base, data->vbl_layer_baseaddr, data->avbl_address);
        data->activate_vbl = 0;
    }

    /* wake up any threads waiting */
    wake_up_interruptible();

    return IRQ_HANDLED;
}

static int thinklcdml_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    unsigned long phys_address;
    int ret = 0;

    PRINT_PROC_ENTRY;
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


    if ( var->activate == FB_ACTIVATE_VBL ) {

	info->var.activate == FB_ACTIVATE_VBL;
        // activate on next vbl
        activate_vbl_layer_baseaddr = TLCD_REG_LAYER_BASEADDR(OL(info));
        activate_vbl_address = phys_address;

        if (activate_vbl != 1) {
            activate_vbl = 1;
            // enable interrupts for next vsync
            tlcdml_write(TLCD_REG_INTERRUPT, 0x1);
        }
#ifdef SYNC_ON_HIFPS
        else {
            // Very High FPS - Normally, the two back buffers should be swapped
            // but this function is not supported by fbdev... so just wait for vsync
//            printk("TooFast\n");
            thinklcdml_vsync(info);
        }
#endif
    }
    else {
        think_writel(par->regs, TLCD_REG_LAYER_BASEADDR(OL(info)), phys_address);
    }

    return ret;
}


static int thinklcdml_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    void* offset;
    unsigned Bpp, color, ret;
    int red, green, blue, i,mode;
    struct tlcdml_drvdata *drvdata = platform_get_drvdata(info->device);
    struct thinklcdml_par *par = info->par;

    switch (cmd) {
    case TLCDML_SET_REGISTER:
        tlcdml_write(*((unsigned long*)arg), *(((unsigned long*)arg)+1));
        break;

    case TLCDML_GET_REGISTER:
        *(((unsigned long*)arg)+1) = tlcdml_read( par->regs, *((unsigned long*)arg));
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

    case TLCDML_DUMP_REGS:
	dump_regs(par, arg);
        break;

    case FBIO_WAITFORVSYNC:
        ret = thinklcdml_vsync(info);
        return ret;
        break;

    case FBIO_RAMP_SET:
        mode = think_readl(par->regs, TLCD_REG_MODE);

        if(arg==0)
            think_writel(par->regs, TLCD_REG_MODE, mode & ~TLCD_CONFIG_GAMMA);
        else {
            for(i=0; i<256; i++) {
                red   = CLAMP255(i+arg);
                green = CLAMP255(i+arg);
                blue  = CLAMP255(i+arg);
                tlcdml_write(par->regs, TLCD_PALETTE_OFFSET + i * 4, (red << 16) | (green << 8) | (blue << 0));
            }
            think_writel(par->regs, TLCD_REG_MODE, mode | TLCD_CONFIG_GAMMA);
        }
        return 1;

    case TLCDML_SWAP_BUFFERS:
        PRINT_D("ioctl: TLCDML_SWAP_BUFFERS");

        if(BUFFERS_PER_LAYER == 1)
            return 0;

	/* XXX: Prone to bugs. I think well be ok though */
        info->var.yoffset += info->var.yres;
	info->var.yoffset %= info->fix.yres_virtual;

        if(!thinklcdml_pan_display(&info->var, info))
            return drvdata->currentBuffer[OL(info)];
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


static inline int tlcdml_dealloc_layers(struct tlcdml_drvdata* stack)
{
    struct fb_info *info;
    int ret = 0;

    drvdata->infos = kzalloc(sizeof(struct fb_info*) * max_alloc_layers, GFP_KERNEL);
    if (!drvdata->infos){
	return -ENOMEM;
    }

    do {
	/* Setup framebuffer info */
	info = framebuffer_alloc(sizeof(struct tlcdml_drvdata), device->dev);
	if (!info) {
	    return -ENOMEM;
	}

	if (ret = register_framebuffer(info) < 0) {
	    framebuffer_release(info);
	    return ret;
	}

	if (ret = tlcdml_alloc_layer(info, fb_size) < 0) {
	    unregister_framebuffer(info);
	    framebuffer_release(info);
	    return ret;
	}

	memset(info->screen_base, 0, info->fix.smem_len);
    } while (tlcdml_push_info(info, drvdata));

    return ret;
}

static inline void tlcdml_dealloc_layers(struct tlcdml_drvdata* stack)
{
    while(info = tlcdml_pop_info(stack)) {
	if (fb_hard) {
	    iounmap((void *)info->fix.smem_start);
	    release_mem_region(info->fix.smem_start, info->fix.smem_len);
	} else {
	    /* Unpin pages */
	    for (page = (unsigned long)info->screen_base;
		 page < PAGE_ALIGN((unsigned long)info->screen_base + info->screen_size);
		 page += PAGE_SIZE)
		ClearPageReserved(page);

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

static inline int tlcdml_setup_pll_pixclock (const struct platform_device* device)
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
    struct tlcdml_fb_data *drvdata;
    u32 fb_size = 0;
    struct fb_info *info;

    /* DrvData */
    drvdata = kzmalloc(sizeof(struct tlcdml_fb_data), GFP_KERNEL);
    if (!drvdata)
	return -ENOMEM;
    platform_set_drvdata(device, drvdata);

    /* Framebuffer setup */
    if (fb_hard) {
	if (!request_mem_region(fb_hard, fb_size, device->name)) {
            PRINT_E("Request mem region failed\n");
            ret = -ENOMEM;
	    goto drvdata_free;
        }

	/* Can't have more than one hard addressed framebuffers. */
        alloc_layers = 1;

        /* and finally remap that memory to kernel's address space */
        virtual_start[0] = (unsigned long) ioremap_nocache(fb_hard, fb_size);
        if (!virtual_start[0]) {
            PRINT_E("Failed to remap memory\n");
	    iounmap(fb_hard);
            ret = -ENOMEM;
	    goto drvdata_free;
        }
    } else {
	if (ret = tlcdml_alloc_layers(drvdata) < 0)
	    goto layers_free;
    }

    /* Registers */
    if (!request_mem_region(physical_register_base, TLCD_MMIOALLOC, device->name)) {
	PRINT_E("Request for MMIO for register file was negative.\n");
	goto layers_free;
    }

    virtual_regs_base = ioremap_nocache(physical_register_base, TLCD_MMIOALLOC);
    if (!virtual_regs_base) {
	PRINT_E("MMIO remap for register file failed\n");
	release_mem_region(physical_regs_base, TLCD_MMIOALLOC);
	ret = -ENOMEM;
	goto layers_free;
    }

    tlcdml_write(virtual_regs_base, TLCD_REG_MODE, TLCD_MODE);

    /* IRQ */
    init_waitqueue_head(&drvdata->irq_data->wait_vsync);
    if (ret = request_irq(TLCD_VSYNC_IRQ,
			  thinklcdml_vsync_interrupt,
			  IRQF_DISABLED, "thinklcdml vsync",
			  drvdata->irq_data) < 0)
	goto regs_free;

    /* PLL */
    if (ret = tlcdml_setup_pixclock(device) < 0)
	goto irq_free;

    return ret;

    /* Cleanup */
irq_free:
    free_irq(TLCD_VSYNC_IRQ, drvdata);

regs_free:
    iounmap(virtual_regs_base);
    release_mem_region(physical_regs_base, TLCD_MMIOALLOC);

layers_free:
    tlcdml_dealloc_layers(info);

drvdata_free:
    kfree(drvdata);

    return ret;

}

static int __init thinklcdml_init(void)
{
    int ret = 0;

    tlcdml_setup();

    if (ret = platform_driver_probe(&thinklcdml_driver, thinklcdml_probe))
	goto err_driver;

    if (ret = platform_device_register(&thinklcdml_device))
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
    struct fb_info* info;
    struct tlcdml_fb_data* drvdata  = platform_get_drvdata(device);
    unsigned long page;
    unsigned i;
    list_head *p, *n;

    tlcdml_relase_pll_pixclock();

    tlcdml_write(TLCD_REG_INTERRUPT, 0);
    free_irq(TLCD_VSYNC_IRQ, drvdata);

    iounmap(virtual_regs_base);
    release_mem_region(physical_regs_base, register_file_size);

    tlcdml_dealloc_layers(drvdata);

}

static void __exit thinklcdml_exit(void)
{
    int i;
    PRINT_PROC_ENTRY;

    // XXX: Unregister devices
    platform_driver_unregister(&thinklcdml_driver);
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


module_init(thinklcdml_init);
module_exit(thinklcdml_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Think Silicon Ltd");
MODULE_DESCRIPTION("ThinkLCDML device driver");
