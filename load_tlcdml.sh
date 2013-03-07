#!/bin/sh

module="thinklcdml"
device="thinklcdml"
mode="664"

/sbin/insmod $module.ko $* || exit 1

echo "Running tests..."
nosetests .
