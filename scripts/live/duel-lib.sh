# 对拍：同一输入下 shell 版（installer-lib.sh）与 Rust 版（tools/gk3-installer）逐行比较。
# 设计：docs/dev/installer-rust-design.md。被 test-apply.sh 与 test-duel.sh source。
#
#   GK3_TEST_DUEL=<Rust 版的路径>     不设（或空）⇒ 下面的 duel_* 全是空操作，test-apply.sh 照旧
#   duel_call  <标签> [VAR=值 …] <函数> [参数…]   一次调用：两边各跑一遍、比较
#   duel_scene <标签> <盘>                        一块盘的标准一组（探测、预检、三种模式的方案与它们的反例）
#   duel_pure                                     与盘无关的方案计算（test-plan.sh 那几组 + 参数反例）
#   duel_summary                                  打印合计；返回值 = 有没有不一致
#
# 比较的是"协议面"（前端看得见的全部）：
#   * stdout 逐字节
#   * stderr 里的协议行（PROGRESS / ERR / JOB 开头的）逐字节
#   * stderr 里 `!! ` 行的条数（中文说明只进日志，内容允许不同；有没有失败说明必须相同）
#   * 退出码
#   其余 stderr（给人看的日志、Rust 版的"注意："）不比 —— 那是允许不同、而且 Rust 版应该更啰嗦的地方。
#
# ★ 防误报：同一时刻跑两遍 shell 版不一定一样（partprobe 之后 udev 还在建节点、blkid 的缓存）。
#   两边不同时再跑一遍 shell 版：shell 自己前后不一致 ⇒ 环境在变（等 udev、重试，最多 3 轮），
#   计入 UNSTABLE 而不是 FAIL；shell 前后一致而 Rust 不同 ⇒ 真的不一致，FAIL 并打出 diff。
#
# ⚠️ 不对拍会让 shell 版挂住的输入（设计稿 §3.3 的 D1：`gk3_plan --mode` 少一个值 ⇒ shell 版死循环）。

DUEL_PASS=0; DUEL_FAIL=0; DUEL_UNSTABLE=0; DUEL_SKIP=0
DUEL_LIB=${DUEL_LIB:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/installer-lib.sh}
DUEL_DIR=""
DUEL_IMPL=""

duel_on() { [ -n "${GK3_TEST_DUEL:-}" ]; }

duel__init() {
    [ -n "$DUEL_DIR" ] && return 0
    DUEL_DIR=$(mktemp -d /tmp/gk3-duel.XXXX)
    [ -x "$GK3_TEST_DUEL" ] || { echo "  ✗ 对拍：$GK3_TEST_DUEL 不可执行" >&2; DUEL_FAIL=$((DUEL_FAIL+1)); return 1; }
    DUEL_IMPL=" $("$GK3_TEST_DUEL" --list | tr '\n' ' ') "
    echo "  ⇄ 对拍：Rust 版 $("$GK3_TEST_DUEL" --version)（$GK3_TEST_DUEL），已实现：${DUEL_IMPL}"
}

# 跑一边，结果写进 $1.{out,err,proto,rc}。$2=shell|rust，其余 = [VAR=值 …] 函数 参数…
duel__run() {
    local base=$1 side=$2; shift 2
    local -a envs=()
    while [ $# -gt 0 ] && [[ $1 =~ ^[A-Za-z_][A-Za-z0-9_]*= ]]; do envs+=("$1"); shift; done
    if [ "$side" = shell ]; then
        env ${envs[@]+"${envs[@]}"} /bin/bash -c '. "$0" && "$@"' "$DUEL_LIB" "$@" >"$base.out" 2>"$base.err" </dev/null
    else
        env ${envs[@]+"${envs[@]}"} "$GK3_TEST_DUEL" "$@" >"$base.out" 2>"$base.err" </dev/null
    fi
    echo $? > "$base.rc"
    # 协议面：stdout 原样 + stderr 的协议行 + `!!` 的条数 + 退出码
    { cat "$base.out"; echo "── stderr 协议行"; grep -aE '^(PROGRESS|ERR|JOB) ' "$base.err";
      echo "── !! 行数 $(grep -ac '^!! ' "$base.err")"; echo "── 退出码 $(cat "$base.rc")"; } > "$base.proto"
}

duel_call() {
    duel_on || return 0
    duel__init || return 0
    local label=$1; shift
    local fn i=1
    for fn in "$@"; do [[ $fn =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || break; done
    case "$DUEL_IMPL" in *" $fn "*) ;; *) DUEL_SKIP=$((DUEL_SKIP+1)); return 0 ;; esac
    local n=$((DUEL_PASS + DUEL_FAIL + DUEL_UNSTABLE + 1)) b
    b=$DUEL_DIR/$n
    duel__run "$b.s1" shell "$@"; duel__run "$b.r" rust "$@"
    if cmp -s "$b.s1.proto" "$b.r.proto"; then DUEL_PASS=$((DUEL_PASS+1)); return 0; fi
    # 不同：shell 版自己稳不稳？
    while [ $i -le 3 ]; do
        command -v udevadm >/dev/null && udevadm settle --timeout=5 2>/dev/null; sleep 1
        duel__run "$b.s2" shell "$@"
        if cmp -s "$b.s1.proto" "$b.s2.proto"; then
            echo "  ✗ 对拍不一致 [$label] $*"
            diff -u --label shell --label rust "$b.s1.proto" "$b.r.proto" | head -40 | sed 's/^/      /'
            DUEL_FAIL=$((DUEL_FAIL+1)); return 1
        fi
        # shell 前后不一致：环境在变。拿最新的一份重新比
        cp "$b.s2.proto" "$b.s1.proto"; duel__run "$b.r" rust "$@"
        if cmp -s "$b.s1.proto" "$b.r.proto"; then DUEL_PASS=$((DUEL_PASS+1)); return 0; fi
        i=$((i+1))
    done
    echo "  ⚠ 对拍不稳定 [$label] $*（shell 版自己前后输出都不一样，环境还在变）"
    DUEL_UNSTABLE=$((DUEL_UNSTABLE+1)); return 0
}

# 从 shell 版的探测结果里取这块盘的 FREE 区间与 ESP（用 shell 版的，是因为它是现役：
# 我们要比的是"前端拿着现役的探测结果去问方案"这条路上两边是否一致）
duel_scene() {
    duel_on || return 0
    duel__init || return 0
    local label=$1 d=$2 pr esp fr rs re rr
    duel_call "$label" gk3_probe
    pr=$(/bin/bash -c '. "$0" && gk3_probe' "$DUEL_LIB" 2>/dev/null)
    esp=$(printf '%s\n' "$pr" | awk -v d="$d" '$1=="PART" && index($2, "path="d"p")==1 && / os=esp /{sub("path=","",$2); print $2; exit}')
    for rr in yes no; do
        duel_call "$label" gk3_plan --disk "$d" --mode wipe --rescue "$rr"
        duel_call "$label" gk3_plan --disk "$d" --mode reinstall --rescue "$rr" --esp "${esp:-/dev/null}"
        duel_call "$label" gk3_plan --disk "$d" --mode reinstall --rescue "$rr" --esp "${esp:-/dev/null}" --keep-data yes
    done
    duel_call "$label" gk3_plan --disk "$d" --mode wipe --rescue no --userdata-mib 8191
    duel_call "$label" gk3_plan --disk "$d" --mode wipe --rescue no --userdata-mib 9000
    duel_call "$label" gk3_plan --disk "$d" --mode wipe --rescue no --userdata-mib 99999999
    duel_call "$label" gk3_plan --disk "$d" --mode reinstall --rescue no
    duel_call "$label" gk3_plan --disk "$d" --mode alongside --rescue no --esp "${esp:-/dev/null}"
    # 每一段空闲区：带 / 不带救援、不给 ESP、给一个 /data 大小
    while read -r fr; do
        [ -n "$fr" ] || continue
        rs=$(printf '%s\n' "$fr" | tr ' ' '\n' | sed -n 's/^start=//p'); re=$(printf '%s\n' "$fr" | tr ' ' '\n' | sed -n 's/^end=//p')
        for rr in yes no; do
            duel_call "$label" gk3_plan --disk "$d" --mode alongside --rescue "$rr" --region-start "$rs" --region-end "$re" --esp "${esp:-/dev/null}"
        done
        duel_call "$label" gk3_plan --disk "$d" --mode alongside --rescue no --region-start "$rs" --region-end "$re"
        duel_call "$label" gk3_plan --disk "$d" --mode alongside --rescue no --region-start "$rs" --region-end "$re" --esp "${esp:-/dev/null}" --userdata-mib 8192
    done <<EOF
$(printf '%s\n' "$pr" | grep "^FREE disk=$d ")
EOF
}

duel_pure() {
    duel_on || return 0
    duel__init || return 0
    local RS=100000000 RE
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes --disk-size-mib 476940
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 476940
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 20000
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 0
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib abc
    duel_call pure GK3_FAKE_DISK_MIB=65536 gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue yes
    for RE in $(( RS + 25 * 1024 * 2048 - 1 )) $(( RS + 10 * 1024 * 2048 - 1 )) $(( RS + 20644 * 2048 - 1 )) $(( RS + 20645 * 2048 + 2047 )); do
        duel_call pure gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no --region-start $RS --region-end $RE --esp /dev/nvme0n1p1
        duel_call pure gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue yes --region-start $((RS + 1)) --region-end $RE --esp /dev/nvme0n1p1
    done
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no --region-start $RS --region-end $(( RS + 25 * 1024 * 2048 - 1 ))
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode alongside --rescue no --esp /dev/nvme0n1p1
    duel_call pure gk3_plan --mode wipe
    duel_call pure gk3_plan
    duel_call pure gk3_plan --disk /dev/nvme0n1 --bogus x
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode foo --rescue no --region-start 2048 --region-end 99999999 --esp /dev/nvme0n1p1
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue maybe --disk-size-mib 30000 --userdata-mib abc
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 30000 --userdata-mib 17248
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 30000 --userdata-mib 17249
    duel_call pure gk3_plan --disk /dev/nvme0n1 --mode wipe --rescue no --disk-size-mib 30000 --keep-data yes --userdata-mib 8192
}

duel_summary() {
    duel_on || return 0
    echo "═══ 对拍：一致 $DUEL_PASS · 不一致 $DUEL_FAIL · 不稳定 $DUEL_UNSTABLE · 跳过（Rust 版未实现）$DUEL_SKIP ═══"
    [ -n "$DUEL_DIR" ] && echo "  （每次调用两边的原始输出在 ${DUEL_DIR}/）"
    [ "$DUEL_FAIL" -eq 0 ]
}
