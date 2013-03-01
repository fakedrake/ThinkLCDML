#!/bin/sh

module="tlcdml"
device="tlcdml"
mode="664"

/sbin/insmod $module.ko $* || exit 1

echo "Running tests..."
nosetests .
