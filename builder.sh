#!/bin/bash

while [[ $# -gt 0 ]]; do
    case $1 in
	"--root") shift; XLNX_BOOTSTRAP+="$1";;
	*)
	    echo "Unrecognized option '$1'"
	    exit 1;;
    esac
    shift
done

function fail {
    echo "[FAIL] $1"
    exit 2
}

make ARCH=arm CROSS_COMPILE=arm-xilinx-linux-gnueabi- XLNX_BOOTSTRAP=$XLNX_BOOTSTRAP
