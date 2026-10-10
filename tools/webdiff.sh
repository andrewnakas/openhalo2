#!/bin/bash
# webdiff.sh <web report dir> <native out.log>: first kernel call where the browser
# run and a native run differ (run both with RECOMP_KERNEL_LOG_BUDGET=<n>).
W=$1; N=$2
grep -v "raw\[\|GS 00\|DMA_PUT" $W/console.txt | cut -c7- | grep -a "KERNEL\] #\|returned\|PATH\]\|FILE\]" | sed 's/esp=.*//' > $W/web.k
grep -a "KERNEL\] #\|returned\|PATH\]\|FILE\]" $N | sed 's/esp=.*//' > $W/nat.k
echo "web $(grep -c 'KERNEL\] #' $W/web.k) calls, native $(grep -c 'KERNEL\] #' $W/nat.k) calls"
diff $W/web.k $W/nat.k | head -${3:-14}
