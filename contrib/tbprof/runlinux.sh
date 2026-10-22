#!/bin/bash
# runlinux.sh QEMU WLSPEC [extra qemu args...]  -> prints WL lines + wall time
Q=$1; W=$2; shift 2
cd $HOME/samisa/guest
t0=$(python3 -c 'import time;print(time.time())')
$Q -M virt -cpu cortex-a72 -m 1G -nographic -monitor none -kernel linux -initrd initrd.cpio -append "console=ttyAMA0 quiet wl=$W" "$@" 2>&1 | grep -E "^WL|INIT|Iterations/Sec"
t1=$(python3 -c 'import time;print(time.time())')
python3 -c "print('WALL %.3f' % ($t1-$t0))"
