obj-m := thinklcdml.o thinklcdml_old.o
CFLAGS_thinklcdml.o := '-DBUILD_DATE="$(shell date)"'

export ARCH?=arm
export LINUX_HEADERS?=/tools/Xilinx/Boards/Zynq/Linux/linux-xlnx

THINKLCDML_DIR?=$(PWD)

all: thinklcdml.ko thinklcdml_old.ko

%.ko: %.c
	$(MAKE) -C $(LINUX_HEADERS) M=$(THINKLCDML_DIR) CROSS_COMPILE=$(CROSS_COMPILE)  V=1 modules

install:
	$(MAKE) -C $(LINUX_HEADERS) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) V=1 modules_install

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean

check-syntax:
	gcc -o nul -S ${CHK_SOURCES}
