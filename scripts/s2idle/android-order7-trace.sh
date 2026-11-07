#!/system/bin/sh
# 抓挂起/恢复期间 order>=7 的页分配及调用栈（kmem:mm_page_alloc + stacktrace），用来 A/B patches/kernel/wifi/0001
# 用法（root，先把 android-ath11k-s2loop.sh 也推到 /data/local/tmp/）：setsid nohup sh android-order7-trace.sh <标签> <PEER> &
TAG=$1; PEER=$2; T=/sys/kernel/tracing; D=/data/local/tmp/mhi-test; O=$D/order7-$TAG.txt
[ -d $T/events ] || mount -t tracefs tracefs $T
echo 0 > $T/tracing_on; echo > $T/trace; echo 16384 > $T/buffer_size_kb
echo 'order >= 7' > $T/events/kmem/mm_page_alloc/filter
echo 'stacktrace if order >= 7' > $T/events/kmem/mm_page_alloc/trigger
echo 1 > $T/events/kmem/mm_page_alloc/enable
echo 1 > $T/tracing_on
sh /data/local/tmp/android-ath11k-s2loop.sh 2 1 $PEER
echo 0 > $T/tracing_on
cat $T/trace > $O
echo 0 > $T/events/kmem/mm_page_alloc/enable
echo '!stacktrace if order >= 7' > $T/events/kmem/mm_page_alloc/trigger
echo 0 > $T/events/kmem/mm_page_alloc/filter
cp $D/log $D/log-order7-$TAG
echo "TRACEDONE $(grep -c mm_page_alloc: $O) allocs, mhi frames: $(grep -c -E 'mhi_alloc_bhie_table|mhi_alloc_bhi_buffer|mhi_load_image' $O)" >> $D/log
sync
