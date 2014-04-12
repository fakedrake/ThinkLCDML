#!/bin/bash

if [ $# == 0 ]; then
	make
elif [ $# == 1 ]; then
	make LINUX_HEADERS=$1
else
	make LINUX_HEADERS=$1 CROSS_COMPILE_PREFIX=$2
fi

