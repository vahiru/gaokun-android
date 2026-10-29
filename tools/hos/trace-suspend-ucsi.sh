#!/usr/bin/env bash
# Dedicated trace instance; never changes the global tracer or USB bindings.
set -euo pipefail
action=${1:-status}
serial=${2:-gaokun3}
case "$action" in start|collect|status) ;; *) echo 'Usage: trace-suspend-ucsi.sh start|collect|status [serial]' >&2; exit 2;; esac
adb -s "$serial" shell sh -s -- "$action" <<'DEVICE'
set -eu
grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90
base=/sys/kernel/tracing/instances/hos_suspend_ucsi
snapshot() {
    cat /proc/sys/kernel/random/boot_id
    cat /proc/uptime
    for f in /sys/power/suspend_stats/* /sys/power/pm_wakeup_irq; do
        printf '%s: ' "$f"
        cat "$f"
    done
    cat /proc/interrupts
    ls /sys/class/typec
    for f in /sys/class/udc/*/state; do cat "$f"; done
}
case "$1" in
start)
    [ ! -e "$base" ] || { echo 'Trace instance exists; collect it first.' >&2; exit 1; }
    mkdir "$base"
    # On setup failure, remove only the instance created by this invocation.
    trap 'echo 0 > "$base/tracing_on"; rmdir "$base"' EXIT
    echo 0 > "$base/tracing_on"
    echo 256 > "$base/buffer_size_kb"
    # Idle CPU timekeeping_freeze events can overwrite the actual PM phases.
    # Keep process-context suspend orchestration; idle-loop timing is out of scope.
    echo 'common_pid != 0' > "$base/events/power/suspend_resume/filter"
    echo 'error != 0' > "$base/events/power/device_pm_callback_end/filter"
    for event in power/device_pm_callback_end power/suspend_resume ucsi/ucsi_run_command ucsi/ucsi_reset_ppm ucsi/ucsi_register_port; do
        echo 1 > "$base/events/$event/enable"
    done
    snapshot
    echo 1 > "$base/tracing_on"
    trap - EXIT
    echo 'Trace started; collect after the physical suspend/reconnect test.'
    ;;
collect)
    [ -d "$base" ] || { echo 'No trace instance.' >&2; exit 1; }
    echo 0 > "$base/tracing_on"
    snapshot
    for f in "$base"/per_cpu/cpu*/stats; do
        printf '%s\n' "$f"
        cat "$f"
    done
    cat "$base/trace"
    rmdir "$base"
    ;;
status)
    snapshot
    if [ -d "$base" ]; then cat "$base/tracing_on"; else echo 'Trace inactive'; fi
    ;;
esac
DEVICE
