//! 与前端之间的行协议 —— 与 `scripts/live/installer-lib.sh` 的文件头（:11-31）逐字节一致。
//!
//! * stdout：`TYPE k=v k=v …` 一行一条（DISK / PART / FREE / CHECK / PLAN / PLANERR / PLANSUM / NOTE …）
//! * stderr：`PROGRESS <百分比> <代码> [k=v …]`、`ERR code=<代码> [k=v …] [touched=yes|no]`，紧跟一行 `!! <中文说明>`；
//!   其余 stderr 行是给人看的日志（前端只放进"详情"）
//! * 值里不许有空格：自由文本一律经 [`enc`] 做百分号编码（`%`→`%25`、空格→`%20`、制表符→`%09`，installer-lib.sh:119）
//!
//! 前端的解析器是 `installer/live/lib/backend/protocol.dart`：按空格切、每个值 %-解码。
//!
//! ★ 所有写出都返回 `io::Result` —— 不用 `println!`（遇到 EPIPE 会 panic，Cargo.toml 的 lint 禁了它），
//!   也不吞写失败：前端没收到的记录，等于没发生。

use std::io::{self, Write};
use std::sync::atomic::{AtomicU8, Ordering};

/// installer-lib.sh:119 `gk3__enc`：`%` → `%25`，空格 → `%20`，制表符 → `%09`。按【字节】做 ——
/// FAT 卷标可能是 GBK 之类的非 UTF-8 字节（libblkid 原样给出），shell 版也是原样透过。
pub fn enc(v: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(v.len());
    for &c in v {
        match c {
            b'%' => out.extend_from_slice(b"%25"),
            b' ' => out.extend_from_slice(b"%20"),
            b'\t' => out.extend_from_slice(b"%09"),
            _ => out.push(c),
        }
    }
    out
}

/// 一个值在不编码时能不能原样放进一行：shell 版对数字、路径、类型 GUID 这些"按理不含空白"的字段不编码。
/// 真含了空白时 shell 版会输出一行切不开的记录；这里改为编码（设计稿 §3.3 的偏差 D4），并让调用方知道。
fn token_safe(v: &[u8]) -> bool {
    !v.iter().any(|&c| c == b' ' || c == b'\t' || c == b'\n')
}

/// 一条行记录。字段顺序 = 加入顺序（与 shell 版的 echo 顺序逐个对应）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    kind: String,
    fields: Vec<(String, Vec<u8>)>,
    /// 有 raw 字段因为含空白而被改成了编码（偏差 D4）
    pub coerced: bool,
}

impl Record {
    pub fn new(kind: &str) -> Self {
        Record { kind: kind.to_string(), fields: Vec::new(), coerced: false }
    }

    /// shell 版不编码的字段（`num=$num`、`fs=${fstype:-}` 这类）。含空白时退回编码，并记下 `coerced`。
    pub fn raw(mut self, key: &str, val: impl AsRef<[u8]>) -> Self {
        let v = val.as_ref();
        if token_safe(v) {
            self.fields.push((key.to_string(), v.to_vec()));
        } else {
            self.coerced = true;
            self.fields.push((key.to_string(), enc(v)));
        }
        self
    }

    /// 整数字段
    pub fn num(self, key: &str, val: i64) -> Self {
        let s = val.to_string();
        self.raw(key, s)
    }

    /// shell 版经 gk3__enc 的字段（自由文本）
    pub fn enc(mut self, key: &str, val: impl AsRef<[u8]>) -> Self {
        self.fields.push((key.to_string(), enc(val.as_ref())));
        self
    }

    pub fn kind(&self) -> &str {
        &self.kind
    }

    /// 取一个字段的【线上】值（未解码）
    pub fn get(&self, key: &str) -> Option<&[u8]> {
        self.fields.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_slice())
    }

    /// 序列化成一行（不含换行）
    pub fn line(&self) -> Vec<u8> {
        let mut out = self.kind.as_bytes().to_vec();
        for (k, v) in &self.fields {
            out.push(b' ');
            out.extend_from_slice(k.as_bytes());
            out.push(b'=');
            out.extend_from_slice(v);
        }
        out
    }
}

/// 目标盘动过没有（installer-lib.sh:100-117 的 `touched=`）。只在写盘入口（apply / shrink）的调用栈里出现，
/// 所以是 `Option`：只读入口的 ERR 不带这个字段 —— 与 shell 版"看调用栈"的规则一致。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Touched {
    No,
    Yes,
}

impl Touched {
    fn as_str(self) -> &'static str {
        match self {
            Touched::No => "no",
            Touched::Yes => "yes",
        }
    }
}

/// 全局的"现在在不在写盘入口里、盘动过没有"：0 = 不在写盘入口里，1 = no，2 = yes。
/// 给 panic hook 用（它拿不到调用栈上的状态）；正常路径上由 [`TouchGuard`] 维护，不直接写。
static TOUCH_STATE: AtomicU8 = AtomicU8::new(0);

/// 写盘入口持有它：创建时 = 没动过；第一次写盘【之前】调 [`TouchGuard::mark`]（宁可早报"动过"，
/// installer-lib.sh:1031-1032 的规矩）。drop 时退出写盘入口。
#[derive(Debug)]
pub struct TouchGuard {
    _priv: (),
}

impl TouchGuard {
    pub fn enter() -> Self {
        TOUCH_STATE.store(1, Ordering::SeqCst);
        TouchGuard { _priv: () }
    }
    pub fn mark(&self) {
        TOUCH_STATE.store(2, Ordering::SeqCst);
    }
    pub fn state(&self) -> Touched {
        if TOUCH_STATE.load(Ordering::SeqCst) == 2 {
            Touched::Yes
        } else {
            Touched::No
        }
    }
}

impl Drop for TouchGuard {
    fn drop(&mut self) {
        TOUCH_STATE.store(0, Ordering::SeqCst);
    }
}

/// 当前的 touched 状态（不在写盘入口里 ⇒ None）
pub fn current_touched() -> Option<Touched> {
    match TOUCH_STATE.load(Ordering::SeqCst) {
        1 => Some(Touched::No),
        2 => Some(Touched::Yes),
        _ => None,
    }
}

/// 一次失败：给界面的 `ERR code=… k=v… [touched=…]` + 给人看的 `!! 说明`（installer-lib.sh:106-117 `gk3_fail`）
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Failure {
    pub code: String,
    /// 值在序列化时编码（gk3_fail 对每个值都过 gk3__enc）
    pub fields: Vec<(String, Vec<u8>)>,
    pub touched: Option<Touched>,
    pub human: String,
}

impl Failure {
    pub fn new(code: &str, human: impl Into<String>) -> Self {
        Failure { code: code.to_string(), fields: Vec::new(), touched: current_touched(), human: human.into() }
    }
    pub fn field(mut self, k: &str, v: impl AsRef<[u8]>) -> Self {
        self.fields.push((k.to_string(), v.as_ref().to_vec()));
        self
    }
    pub fn err_line(&self) -> Vec<u8> {
        let mut out = b"ERR code=".to_vec();
        out.extend_from_slice(self.code.as_bytes());
        for (k, v) in &self.fields {
            out.push(b' ');
            out.extend_from_slice(k.as_bytes());
            out.push(b'=');
            out.extend_from_slice(&enc(v));
        }
        if let Some(t) = self.touched {
            out.extend_from_slice(b" touched=");
            out.extend_from_slice(t.as_str().as_bytes());
        }
        out
    }
}

/// 进度：`PROGRESS <百分比> <代码> [k=v…]`，值一律编码（installer-lib.sh:95-99 `gk3_prog`）
pub fn progress_line(pct: u8, code: &str, fields: &[(&str, &[u8])]) -> Vec<u8> {
    let mut out = format!("PROGRESS {pct} {code}").into_bytes();
    for (k, v) in fields {
        out.push(b' ');
        out.extend_from_slice(k.as_bytes());
        out.push(b'=');
        out.extend_from_slice(&enc(v));
    }
    out
}

/// 两条输出流。每写一行就 flush：前端是边读边显示的（写 super 的进度要实时到）。
pub struct Out<'a> {
    stdout: &'a mut dyn Write,
    stderr: &'a mut dyn Write,
    /// 被改成编码的 raw 字段数（偏差 D4 发生过几次；对拍时据此知道"这里本来就会不同"）
    pub coerced: usize,
}

impl std::fmt::Debug for Out<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Out").field("coerced", &self.coerced).finish()
    }
}

impl<'a> Out<'a> {
    pub fn new(stdout: &'a mut dyn Write, stderr: &'a mut dyn Write) -> Self {
        Out { stdout, stderr, coerced: 0 }
    }

    fn put(w: &mut dyn Write, line: &[u8]) -> io::Result<()> {
        w.write_all(line)?;
        w.write_all(b"\n")?;
        w.flush()
    }

    /// stdout 上的一条记录
    pub fn record(&mut self, r: &Record) -> io::Result<()> {
        if r.coerced {
            self.coerced += 1;
        }
        Self::put(self.stdout, &r.line())
    }

    /// stdout 上原样的一行（PLANERR 那种自带格式、值里没有自由文本的记录也走 [`Out::record`]；这个只给特殊情况）
    pub fn stdout_line(&mut self, line: &[u8]) -> io::Result<()> {
        Self::put(self.stdout, line)
    }

    pub fn progress(&mut self, pct: u8, code: &str, fields: &[(&str, &[u8])]) -> io::Result<()> {
        Self::put(self.stderr, &progress_line(pct, code, fields))
    }

    /// `ERR …` + `!! …`（顺序与 gk3_fail 相同：先给界面的，再给人看的）
    pub fn fail(&mut self, f: &Failure) -> io::Result<()> {
        Self::put(self.stderr, &f.err_line())?;
        let mut h = b"!! ".to_vec();
        h.extend_from_slice(f.human.as_bytes());
        Self::put(self.stderr, &h)
    }

    /// 只有 `!! 说明`、没有 ERR 的失败（shell 版里还剩几处 gk3_die，installer-lib.sh:92）
    pub fn die(&mut self, human: &str) -> io::Result<()> {
        let mut h = b"!! ".to_vec();
        h.extend_from_slice(human.as_bytes());
        Self::put(self.stderr, &h)
    }

    /// 给人看的日志（stderr）
    pub fn log(&mut self, line: &str) -> io::Result<()> {
        Self::put(self.stderr, line.as_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enc_matches_shell() {
        // 期望值是在容器里对 installer-lib.sh 的 gk3__enc 跑出来的
        assert_eq!(enc(b"Basic data partition"), b"Basic%20data%20partition");
        assert_eq!(enc(b"50% off\tx"), b"50%25%20off%09x");
        assert_eq!(enc("中文 卷标".as_bytes()), "中文%20卷标".as_bytes());
        assert_eq!(enc(b""), b"");
        // 非 UTF-8 字节原样透过（GBK 的"系统"）
        assert_eq!(enc(&[0xcf, 0xb5, b' ', 0xcd, 0xb3]), &[0xcf, 0xb5, b'%', b'2', b'0', 0xcd, 0xb3]);
    }

    #[test]
    fn record_line() {
        let r = Record::new("PART").raw("path", "/dev/nvme0n1p1").num("num", 1).enc("name", "EFI system partition").raw("fs", "");
        assert_eq!(r.line(), b"PART path=/dev/nvme0n1p1 num=1 name=EFI%20system%20partition fs=");
        assert!(!r.coerced);
        assert_eq!(r.get("name"), Some(&b"EFI%20system%20partition"[..]));
    }

    #[test]
    fn raw_with_space_is_coerced_not_broken() {
        let r = Record::new("PART").raw("fs", "a b");
        assert_eq!(r.line(), b"PART fs=a%20b");
        assert!(r.coerced);
    }

    #[test]
    fn err_line_and_touched() {
        let f = Failure { code: "esp-full".into(), fields: vec![("need_mib".into(), b"150".to_vec())], touched: None, human: "x".into() };
        assert_eq!(f.err_line(), b"ERR code=esp-full need_mib=150");
        let f = Failure { touched: Some(Touched::No), ..f };
        assert_eq!(f.err_line(), b"ERR code=esp-full need_mib=150 touched=no");
        // 值编码（gk3_fail 对每个值都编码）
        let f = Failure::new("reinstall-busy", "y").field("parts", "/dev/a /dev/b");
        assert_eq!(f.err_line(), b"ERR code=reinstall-busy parts=/dev/a%20/dev/b");
    }

    #[test]
    fn touch_guard() {
        assert_eq!(current_touched(), None);
        {
            let g = TouchGuard::enter();
            assert_eq!(current_touched(), Some(Touched::No));
            assert_eq!(Failure::new("x", "").touched, Some(Touched::No));
            g.mark();
            assert_eq!(g.state(), Touched::Yes);
            assert_eq!(Failure::new("x", "").err_line(), b"ERR code=x touched=yes");
        }
        assert_eq!(current_touched(), None);
        assert_eq!(Failure::new("x", "").err_line(), b"ERR code=x");
    }

    #[test]
    fn progress_and_out() {
        assert_eq!(
            progress_line(30, "write-super", &[("done_mib", b"12"), ("name", b"a b")]),
            b"PROGRESS 30 write-super done_mib=12 name=a%20b"
        );
        let (mut o, mut e) = (Vec::new(), Vec::new());
        {
            let mut out = Out::new(&mut o, &mut e);
            out.record(&Record::new("CHECK").raw("id", "root").raw("ok", "yes")).unwrap();
            out.fail(&Failure { code: "c".into(), fields: vec![], touched: None, human: "坏了".into() }).unwrap();
            out.progress(1, "check", &[]).unwrap();
        }
        assert_eq!(o, b"CHECK id=root ok=yes\n");
        assert_eq!(String::from_utf8(e).unwrap(), "ERR code=c\n!! 坏了\nPROGRESS 1 check\n");
    }

    /// 写失败（前端关了管道）必须变成错误，不是 panic、也不是静默
    #[test]
    fn write_failure_is_an_error() {
        struct Broken;
        impl Write for Broken {
            fn write(&mut self, _: &[u8]) -> io::Result<usize> {
                Err(io::Error::from(io::ErrorKind::BrokenPipe))
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let (mut o, mut e) = (Broken, Vec::new());
        let mut out = Out::new(&mut o, &mut e);
        assert!(out.record(&Record::new("X")).is_err());
    }
}
