obj-m := thinklcdml.o
# nemaweaver-y := nema.o

CROSS_COMPILER:=/homes/cperivol/CodeSourcery/Sourcery_CodeBench_Lite_for_Xilinx_GNU_Linux/bin/arm-xilinx-linux-gnueabi
LINUX_HEADERS:=/homes/cperivol/Projects/Nema/ZYNQ/linux-xlnx/
HOST_LINUX_HEADERS=/usr/src/linux-headers-$(shell uname -r)

all:
	make -C $(LINUX_HEADERS) M=$(PWD) modules CC=$(CROSS_COMPILER)-gcc

host:
	make -C $(HOST_LINUX_HEADERS) M=$(PWD) modules CC=gcc

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
