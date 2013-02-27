#!/bin/sh

module="tlcdml"
device="tlcdml"
mode="664"

/sbin/insmod $module.ko $* || exit 1

major=$(awk "\$2==\"$module\" {print \$1}" /proc/devices)

mknod /dev/${device}0 c $major 0

group="staff"
grep -q "^staff:" /etc/group || group="wheel"

chgrp $group /dev/${device}0
chmod $mode /dev/${device}0

echo "Running tests..."
nosetests .
