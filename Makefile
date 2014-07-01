obj-m := thinklcdml_ref.o

export ARCH?=arm
export LINUX_HEADERS?=/tools/Xilinx/Boards/Zynq/Linux/linux-xlnx

NFS_ROOT=/srv/nfs

all: thinklcdml.ko

thinklcdml.ko: thinklcdml.c
	make -C $(LINUX_HEADERS) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) V=1 modules

install:
	make -C $(LINUX_HEADERS) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) INSTALL_MOD_PATH=$(NFS_ROOT) V=1 modules_install
	cp thinklcdml.ko $(NFS_ROOT)/lib/modules/3.8.0-xilinx-trd-g4ee4371-dirty

clean:
	make -C $(LINUX_HEADERS) M=$(PWD) clean
