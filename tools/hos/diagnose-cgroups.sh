#!/system/bin/sh
# Read-only. Run using an existing privileged Android shell; optionally pass
# a currently failing target PID. Does not write task IDs or change policies.
printf '\n== identity and kernel ==\n'
id
getenforce
uname -a
cat /proc/cgroups
printf '\n== effective mounts ==\n'
cat /proc/self/mountinfo | grep -E 'cgroup|cpuset|cpuctl|blkio'
printf '\n== controller permissions ==\n'
for cg in /dev/cpuctl /dev/cpuset /dev/blkio; do
    for group in "$cg" "$cg/foreground" "$cg/background" "$cg/top-app"; do
        [ -d "$group" ] || continue
        ls -ldZ "$group"
        ls -lZ "$group/tasks" "$group/cgroup.procs" 2>/dev/null
        for leaf in cpus mems cpuset.cpus cpuset.mems cgroup.type cgroup.controllers cgroup.subtree_control; do
            [ -f "$group/$leaf" ] || continue
            printf '%s: ' "$group/$leaf"
            cat "$group/$leaf"
        done
    done
done
printf '\n== system_server and target credentials/membership ==\n'
for cg_pid in $(pidof system_server) "$@"; do
    case "$cg_pid" in ''|*[!0-9]*) continue ;; esac
    [ -d "/proc/$cg_pid" ] || continue
    printf '\nPID %s\n' "$cg_pid"
    cat "/proc/$cg_pid/status"
    cat "/proc/$cg_pid/cgroup"
    cat "/proc/$cg_pid/attr/current"
    ls -l "/proc/$cg_pid/ns/user" "/proc/$cg_pid/ns/cgroup" "/proc/$cg_pid/ns/mnt"
    # Cached target descriptors identify which controller actually failed.
    ls -l "/proc/$cg_pid/fd" 2>/dev/null | grep -E 'cpuset|cpuctl|blkio|cgroup'
done
printf '\n== loaded configuration candidates ==\n'
for cfg in /system/etc/cgroups.json /vendor/etc/cgroups.json /system/etc/task_profiles.json /vendor/etc/task_profiles.json /system/etc/task_profiles/cgroups_*.json /system/etc/task_profiles/task_profiles_*.json; do
    [ -f "$cfg" ] || continue
    printf '\nFILE %s\n' "$cfg"
    cat "$cfg"
done
printf '\n== native main/system buffers and kernel denials ==\n'
logcat -b main -b system -d -v threadtime -s libprocessgroup:V auditd:V
dmesg | grep -E 'avc:|cgroup|cpuset' | tail -n 100
