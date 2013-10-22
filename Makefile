obj-m := thinklcdml.o
# nemaweaver-y := nema.o

XILINX_BOOTSTRAP=/home/fakedrake/Projects/ThinkSilicon/xilinx-zynq-bootstrap
GNU_TOOLS=$(XILINX_BOOTSTRAP)/sources/gnu-tools-archive/GNU_Tools

export PATH:=$(PATH):$(GNU_TOOLS)/bin
export CROSS_COMPILER:=$(GNU_TOOLS)/bin/arm-xilinx-linux-gnueabi
export LINUX_HEADERS:=$(XILINX_BOOTSTRAP)/sources/linux-git/
# LINUX_HEADERS=/usr/src/linux-headers-$(shell uname -r)

all:
	make -C $(LINUX_HEADERS) M=$(PWD) modules CC=$(CROSS_COMPILER)-gcc

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
