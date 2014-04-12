obj-m := thinklcdml.o

export CROSS_COMPILE_PREFIX?=arm-xilinx-linux-gnueabi
export CROSS_COMPILE=$(CROSS_COMPILE_PREFIX)-
export ARCH?=arm
export LINUX_HEADERS?=/tools/Xilinx/Boards/Zynq/Linux/linux-xlnx

all:
	make -C $(LINUX_HEADERS) M=$(PWD) modules CC=$(CROSS_COMPILE_PREFIX)-gcc

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
