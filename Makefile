obj-m := thinklcdml.o
# nemaweaver-y := nema.o

export CROSS_COMPILER:=arm-xilinx-linux-gnueabi
#export LINUX_HEADERS:=/home/filippakoc/Projects/xilinx-zynq-bootstrap-master/linux-xlnx3
export LINUX_HEADERS:=/tools/Xilinx/Boards/Zynq/Linux/linux-xlnx
# LINUX_HEADERS=/usr/src/linux-headers-$(shell uname -r)

all:
	make -C $(LINUX_HEADERS) M=$(PWD) modules CC=$(CROSS_COMPILER)-gcc

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
