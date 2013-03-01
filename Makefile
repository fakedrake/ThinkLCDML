obj-m := thinklcdml.o
# nemaweaver-y := nema.o

export CROSS_COMPILER:=/homes/cperivol/CodeSourcery/Sourcery_CodeBench_Lite_for_Xilinx_GNU_Linux/bin/arm-xilinx-linux-gnueabi
export LINUX_HEADERS:=/homes/cperivol/Projects/Nema/ZYNQ/linux-xlnx/
# LINUX_HEADERS=/usr/src/linux-headers-$(shell uname -r)

all:
	make -C $(LINUX_HEADERS) M=$(PWD) modules CC=$(CROSS_COMPILER)-gcc

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
