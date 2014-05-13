#ifndef __THINKLCDML_H__
#define __THINKLCDML_H__

#define TLCDML_LAYERS_NUMBER    1
#define BUFFERS_PER_LAYER       3

#define USE_ONLY_800x600

#define ZC702
// Use this with DirectFB
#define LARGE_MEM

// When (var->activate == FB_ACTIVATE_VBL) and FPS are high wait for vsync
#define SYNC_ON_HIFPS

#define USE_CMA

/// PIXCLK_PLL hardware constants
//#define USE_PLL
#ifdef USE_PLL
#define PIXCLKPLL_BASEADDR	    0x43c10000
#define PIXCLKPLL_MMIOALLOC     0x260
#define PIXCLKPLL_RESET		    0x0
#define PIXCLKPLL_STATUS	    0x4
#define PIXCLKPLL_GLOBMULDIV    0x200
#define PIXCLKPLL_CLK0DIV       0x208
#define PIXCLKPLL_LOAD          0x25c
#endif

// ThinkLCD hardware constants
#define LCDBASEADDRESS		0x79000000
#define TLCD_PHYSICAL_BASE	0x79000000	                                ///< Memory mapped IO base address
#define TLCD_MMIOALLOC		0x100		                                ///< Register file allocation length in bytes
#define TLCD_PALETTE_OFFSET	0x400		                                ///< Palette offset in bytes
#define TLCD_PALETTE_COLORS	256		                                    ///< Number of palette colors
#define TLCD_VSYNC_IRQ		91		                                    ///< ThinkLCD vsync irq number
#define TLCD_ACCEL		    0x54736930	                                ///< TSi accelerator code for use in device drivers
#define TLCD_MODE	        (TLCD_CONFIG_ENDIAN | TLCD_CONFIG_AHBLOCK)	///< Default mode bits

// ThinkLCD cursor
#define TLCD_CLKCTRL        0x00000401
#define TLCD_BGCOLOR        0xFFFF0000

// ThinkLCD modes
#define TLCD_MODE_LUT8		(0x00 | (1<<20))
#define TLCD_MODE_RGBA5551	0x01
#define TLCD_MODE_RGBA8888	0x02
#define TLCD_MODE_TEST		0x03
#define TLCD_MODE_RGB332	0x04
#define TLCD_MODE_RGBA565	0x05
#define TLCD_MODE_ARGB8888	0x06
#define TLCD_MODE_ABGR8888	0x0d
#define TLCD_MODE_BGRA8888	0x0e
#define TLCD_MODE_L8		0x07

// ThinkLCD register file
#define TLCD_REG_MODE         	 0x00 /* 0x80000000 */
#define TLCD_REG_CLKCTRL    	 0x04 /* 0x00000402 */
#define TLCD_REG_BGCOLOR      	 0x08 /* 0xFFFF0000 */
#define TLCD_REG_RESXY        	 0x0c /* 0x04000300 */
#define TLCD_REG_STRIDE       	 0x10
#define TLCD_REG_FRONTPORCHXY 	 0x14 /* 0x04200303 */
#define TLCD_REG_BLANKINGXY   	 0x18 /* 0x04680306 */
#define TLCD_REG_BACKPORCHXY  	 0x1c /* 0x04F80320 */
#define TLCD_REG_CURSORXY	     0x20
#define TLCD_REG_CONFIG		     0xf0

#define TLCD_REG_LAYER0_MODE     0x30 /* 0x88ff0102 */
#define TLCD_REG_LAYER0_STARTXY  0x34 /* 0x04000300 */
#define TLCD_REG_LAYER0_SIZEXY   0x38
#define TLCD_REG_LAYER0_BASEADDR 0x3c
#define TLCD_REG_LAYER0_STRIDE   0x40
#define TLCD_REG_LAYER0_RESXY    0x44
#define TLCD_REG_LAYER0_SCALEX   0x48
#define TLCD_REG_LAYER0_SCALEY   0x4c

#define TLCD_REG_LAYER1_MODE     0x50
#define TLCD_REG_LAYER1_STARTXY  0x54
#define TLCD_REG_LAYER1_SIZEXY   0x58
#define TLCD_REG_LAYER1_BASEADDR 0x5c
#define TLCD_REG_LAYER1_STRIDE   0x60
#define TLCD_REG_LAYER1_RESXY    0x64
#define TLCD_REG_LAYER1_SCALEX   0x68
#define TLCD_REG_LAYER1_SCALEY   0x6c

#define TLCD_REG_LAYER2_MODE     0x70
#define TLCD_REG_LAYER2_STARTXY  0x74
#define TLCD_REG_LAYER2_SIZEXY   0x78
#define TLCD_REG_LAYER2_BASEADDR 0x7c
#define TLCD_REG_LAYER2_STRIDE   0x80
#define TLCD_REG_LAYER2_RESXY    0x84
#define TLCD_REG_LAYER2_SCALEX   0x88
#define TLCD_REG_LAYER2_SCALEY   0x8c

#define TLCD_REG_LAYER3_MODE     0x90
#define TLCD_REG_LAYER3_STARTXY  0x94
#define TLCD_REG_LAYER3_SIZEXY   0x98
#define TLCD_REG_LAYER3_BASEADDR 0x9c
#define TLCD_REG_LAYER3_STRIDE   0xa0
#define TLCD_REG_LAYER3_RESXY    0xa4
#define TLCD_REG_LAYER3_SCALEX   0xa8
#define TLCD_REG_LAYER3_SCALEY   0xac

#define TLCD_REG_IDREG           0xf4
#define TLCD_REG_STATUS          0xfc
#define TLCD_REG_INTERRUPT       0xf8

// Layer parametric
#define TLCD_REG_LAYER_MODE(i)      (0x030 + 0x20*(i)) //    ((i)?(0x030 + 0x20*(i-1)):(0x50))
#define TLCD_REG_LAYER_STARTXY(i)   (0x034 + 0x20*(i)) // ((i)?(0x034 + 0x20*(i-1)):(0x54))
#define TLCD_REG_LAYER_SIZEXY(i)    (0x038 + 0x20*(i)) //  ((i)?(0x038 + 0x20*(i-1)):(0x58))
#define TLCD_REG_LAYER_BASEADDR(i)  (0x03c + 0x20*(i)) //((i)?(0x03c + 0x20*(i-1)):(0x5c))
#define TLCD_REG_LAYER_STRIDE(i)    (0x040 + 0x20*(i)) //  ((i)?(0x040 + 0x20*(i-1)):(0x60))
#define TLCD_REG_LAYER_RESXY(i)     (0x044 + 0x20*(i)) //   ((i)?(0x044 + 0x20*(i-1)):(0x64))
#define TLCD_REG_LAYER_SCALEX(i)    (0x048 + 0x20*(i)) //  ((i)?(0x048 + 0x20*(i-1)):(0x68))
#define TLCD_REG_LAYER_SCALEY(i)    (0x04c + 0x20*(i)) //  ((i)?(0x04c + 0x20*(i-1)):(0x6c))

// ThinkLCD mode register bits
#define TLCD_CONFIG_DISABLE	0
#define TLCD_CONFIG_ENABLE	(1 << 31)
#define TLCD_CONFIG_CURSOR	(1 << 30)
#define TLCD_CONFIG_AHBLOCK	(1 << 29)
#define TLCD_CONFIG_NEG_V	(1 << 28)
#define TLCD_CONFIG_NEG_H	(1 << 27)
#define TLCD_CONFIG_NEG_DE	(1 << 26)
#define TLCD_CONFIG_ENDIAN	(1 << 25)
#define TLCD_CONFIG_STEREO	(1 << 4)
#define TLCD_CONFIG_GAMMA	(1 << 20)

#define FBIO_CAMERA         0x32
#define FBIO_LAYER1_SET     0x30
#define FBIO_RAMP_SET       0x31

#define FBIO_I2C_R          0x40
#define FBIO_I2C_W          0x41
#define FBIO_SIF_R          0x42
#define FBIO_SIF_W          0x43

#define TLCDML_SET_REGISTER             0x44
#define TLCDML_GET_REGISTER             0x45
#define TLCDML_COLOR_CLEAR              0x46
#define TLCDML_GET_INFO                 0x47
#define TLCDML_DUMP_REGS                0x48
#define TLCDML_GET_LAYER_NUMBER         0x49
#define TLCDML_GET_NUMBER_OF_BUFFERS    0x50
#define TLCDML_SWAP_BUFFERS             0x51
#define TLCDML_GET_CURRENT_BUFFER       0x52

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC	_IOW('F', 0x20, __u32)
#endif

static struct fb_var_screeninfo m640x480 __initdata = {
    .xres =			640,
    .yres =			480,
    .xres_virtual =		640,
    .yres_virtual =		480,
    .pixclock = 		39682,
    .left_margin =		48,
    .right_margin =		16,
    .upper_margin =		33,
    .lower_margin =		10,
    .hsync_len =		96,
    .vsync_len =		2,
};

/*static struct fb_var_screeninfo m1024x600 __initdata = {
    .xres =			1024,
    .yres =			600,
    .xres_virtual =		1024,
    .yres_virtual =		600,
    .pixclock = 		43750,
    .left_margin =		80,
    .right_margin =		48,
    .upper_margin =		6,
    .lower_margin =		3,
    .hsync_len =		32,
    .vsync_len =		10,
};*/

static struct fb_var_screeninfo m800x480 __initdata = {
    .xres =			800,
    .yres =			480,
    .xres_virtual =		800,
    .yres_virtual =		480,
    .pixclock = 		71420,
    .left_margin =		216,
    .right_margin =		16,
    .upper_margin =		35,
    .lower_margin =		10,
    .hsync_len =		48,
    .vsync_len =		3,
};

static struct fb_var_screeninfo m800x600 __initdata = {
    .xres =			800,
    .yres =			600,
    .xres_virtual =		800,
    .yres_virtual =		600,
    .pixclock =		25000,
    .left_margin =		88,
    .right_margin =		40,
    .upper_margin =		23,
    .lower_margin =		1,
    .hsync_len =		128,
    .vsync_len =		4,
};

static struct fb_var_screeninfo m1024x768 __initdata = {
    .xres =			1024,
    .yres =			768,
    .xres_virtual =		1024,
    .yres_virtual =		768,
    .pixclock =		        48363,
    .left_margin =		160,
    .right_margin =		24,
    .upper_margin =		29,
    .lower_margin =		3,
    .hsync_len =		136,
    .vsync_len =		6,
};

#endif
