#ifndef __THINKLCDML_H__
#define __THINKLCDML_H__

#define TLCDML_LAYERS_NUMBER 2 // a makefile seds this line to make the devices
#define TLCDML_MAJOR_NUMBER 0

// ThinkLCD hardware constants
#define TLCD_PHYSICAL_BASE	0x6c000000	///< Memory mapped IO base address
#define TLCD_MMIOALLOC		0x1000		///< Register file allocation length in bytes
#define TLCD_PALETTE_OFFSET	0x400		///< Palette offset in bytes
#define TLCD_PALETTE_COLORS	256		///< Number of palette colors
#define TLCD_VSYNC_IRQ		5		///< ThinkLCD vsync irq number
#define TLCD_ACCEL		0x54736930	///< TSi accelerator code for use in device drivers
#define TLCD_MODE	(TLCD_CONFIG_ENDIAN | TLCD_CONFIG_AHBLOCK)	///< Default mode bits

// ThinkLCD cursor
#define TLCD_CURSOR		1		///< 0 to disable cursor, 1 to enable
#define TLCD_CURSOR_IMAGE	0x800		///< Cursor image offset in bytes
#define TLCD_CURSOR_CLUT	0xA00		///< Cursor color lookup table in bytes
#define TLCD_CURSOR_WIDTH	32		///< Hardware cursor width
#define TLCD_CURSOR_HEIGHT	32		///< Hardware cursor height
#define TLCD_CURSOR_COLORS	16		///< Number of hardware cursor color lookup table colors

// ThinkLCD modes
#define TLCD_MODE_LUT8		(0x00 | (1<<20))
#define TLCD_MODE_RGBA5551	0x01
#define TLCD_MODE_RGBA8888	0x02
#define TLCD_MODE_TEST		0x03
#define TLCD_MODE_RGB332	0x04
#define TLCD_MODE_RGB565	0x05
#define TLCD_MODE_ARGB8888	0x06
#define TLCD_MODE_L8		0x07

// ThinkLCD register file
#define TLCD_REG_MODE         	 0x000
#define TLCD_REG_CLKCTRL    	 0x004 // BASEADDRESS0
#define TLCD_REG_BGCOLOR      	 0x008
#define TLCD_REG_RESXY        	 0x00c
#define TLCD_REG_STRIDE       	 0x010
#define TLCD_REG_FRONTPORCHXY 	 0x014
#define TLCD_REG_BLANKINGXY   	 0x018
#define TLCD_REG_BACKPORCHXY  	 0x01c
#define TLCD_REG_CURSORXY	     0x020

#define TLCD_REG_LAYER0_MODE     0x030
#define TLCD_REG_LAYER0_STARTXY  0x034
#define TLCD_REG_LAYER0_SIZEXY   0x038
#define TLCD_REG_LAYER0_BASEADDR 0x03c
#define TLCD_REG_LAYER0_STRIDE   0x040
#define TLCD_REG_LAYER0_RESXY    0x044
#define TLCD_REG_LAYER0_SCALEX   0x048
#define TLCD_REG_LAYER0_SCALEY   0x04c

#define TLCD_REG_LAYER1_MODE     0x050
#define TLCD_REG_LAYER1_STARTXY  0x054
#define TLCD_REG_LAYER1_SIZEXY   0x058
#define TLCD_REG_LAYER1_BASEADDR 0x05c
#define TLCD_REG_LAYER1_STRIDE   0x060
#define TLCD_REG_LAYER1_RESXY    0x064
#define TLCD_REG_LAYER1_SCALEX   0x068
#define TLCD_REG_LAYER1_SCALEY   0x06c

#define TLCD_REG_LAYER2_MODE     0x070
#define TLCD_REG_LAYER2_STARTXY  0x074
#define TLCD_REG_LAYER2_SIZEXY   0x078
#define TLCD_REG_LAYER2_BASEADDR 0x07c
#define TLCD_REG_LAYER2_STRIDE   0x080
#define TLCD_REG_LAYER2_RESXY    0x084
#define TLCD_REG_LAYER2_SCALEX   0x088
#define TLCD_REG_LAYER2_SCALEY   0x08c

#define TLCD_REG_LAYER3_MODE     0x090
#define TLCD_REG_LAYER3_STARTXY  0x094
#define TLCD_REG_LAYER3_SIZEXY   0x098
#define TLCD_REG_LAYER3_BASEADDR 0x09c
#define TLCD_REG_LAYER3_STRIDE   0x0a0
#define TLCD_REG_LAYER3_RESXY    0x0a4
#define TLCD_REG_LAYER3_SCALEX   0x0a8
#define TLCD_REG_LAYER3_SCALEY   0x0ac

#define TLCD_REG_IDREG           0x0f4
#define TLCD_REG_STATUS          0x0fc
#define TLCD_REG_INTERRUPT       0x0f8

// Layer parametric
#define TLCD_REG_LAYER_MODE(i) (0x030 + 0x20*(i)) //    ((i)?(0x030 + 0x20*(i-1)):(0x50))
#define TLCD_REG_LAYER_STARTXY(i) (0x034 + 0x20*(i)) // ((i)?(0x034 + 0x20*(i-1)):(0x54))
#define TLCD_REG_LAYER_SIZEXY(i) (0x038 + 0x20*(i)) //  ((i)?(0x038 + 0x20*(i-1)):(0x58))
#define TLCD_REG_LAYER_BASEADDR(i) (0x03c + 0x20*(i)) //((i)?(0x03c + 0x20*(i-1)):(0x5c))
#define TLCD_REG_LAYER_STRIDE(i) (0x040 + 0x20*(i)) //  ((i)?(0x040 + 0x20*(i-1)):(0x60))
#define TLCD_REG_LAYER_RESXY(i) (0x044 + 0x20*(i)) //   ((i)?(0x044 + 0x20*(i-1)):(0x64))
#define TLCD_REG_LAYER_SCALEX(i) (0x048 + 0x20*(i)) //  ((i)?(0x048 + 0x20*(i-1)):(0x68))
#define TLCD_REG_LAYER_SCALEY(i) (0x04c + 0x20*(i)) //  ((i)?(0x04c + 0x20*(i-1)):(0x6c))

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

#define TLCDML_SET_REGISTER 0x44
#define TLCDML_GET_REGISTER 0x45
#define TLCDML_COLOR_CLEAR  0x46
#define TLCDML_GET_INFO     0x47
#define TLCDML_DUMP_REGS    0x48
#define TLCDML_GET_LAYER_NUMBER 0x49

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC	_IOW('F', 0x20, __u32)
#endif

#endif
