//! 外部命令执行器（设计稿 §4）。
//!
//! shell 版里外部命令的失败有四种死法，这里每一种都进类型：
//!   1. 起不来（没装、没权限）            → [`ExecError::Spawn`]
//!   2. 退出码不在"预期"之内               → [`Output::status`] + [`Output::check`] 变成 [`ExecError::Status`]
//!   3. 被信号杀了                         → [`Status::Signaled`]
//!   4. 挂住不动（shell 版会一直等下去）   → 超时，[`Status::TimedOut`]，子进程被 SIGKILL
//!
//! 每次执行都留下完整命令行、退出状态、耗时、stdout / stderr 的长度（[`Output`]），调用方决定怎么报；
//! 宿主（host.rs）另把它们写进追踪日志（`GK3_TRACE`）。
//!
//! ⚠️ 子进程【不】另开进程组：前端取消下载时是按进程组发 TERM 的（installer/live/lib/backend/shell_backend.dart:30-35、
//!    :127-129），另开进程组的子进程会成为孤儿、照样往盘上写。所以超时只杀子进程本身（我们调的 sgdisk / blkid /
//!    lsblk / findmnt 都不再 fork）。将来要跑管道（zstd | unsparse）时，每一段都是这里的一个 Cmd，各自被看管。

use std::fmt;
use std::io::{self, Read};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

/// 只读查询的默认超时。sgdisk / blkid 在正常的盘上是毫秒级；卡住的 NVMe 上 shell 版会永远等下去
pub const QUERY_TIMEOUT: Duration = Duration::from_secs(60);

/// 一条要执行的命令。stdin 永远是 /dev/null（不许有命令在等交互输入 —— ntfsresize 那次的教训，installer-lib.sh:1530-1536）
#[derive(Debug, Clone)]
pub struct Cmd {
    argv: Vec<String>,
    timeout: Duration,
    env: Vec<(String, String)>,
}

impl Cmd {
    pub fn new(prog: &str) -> Self {
        Cmd { argv: vec![prog.to_string()], timeout: QUERY_TIMEOUT, env: Vec::new() }
    }
    pub fn arg(mut self, a: impl Into<String>) -> Self {
        self.argv.push(a.into());
        self
    }
    pub fn args<I, S>(mut self, it: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        self.argv.extend(it.into_iter().map(Into::into));
        self
    }
    pub fn timeout(mut self, t: Duration) -> Self {
        self.timeout = t;
        self
    }
    pub fn env(mut self, k: &str, v: &str) -> Self {
        self.env.push((k.to_string(), v.to_string()));
        self
    }
    pub fn argv(&self) -> &[String] {
        &self.argv
    }
    /// 给日志看的命令行（含空格 / 引号的参数加单引号）
    pub fn display(&self) -> String {
        self.argv.iter().map(|a| quote(a)).collect::<Vec<_>>().join(" ")
    }

    /// 执行并收集输出。只有"起不来 / 等不了"是 `Err`；退出码由调用方用 [`Output::check`] 判。
    pub fn run(&self) -> Result<Output, ExecError> {
        let (prog, rest) = match self.argv.split_first() {
            Some(x) => x,
            None => return Err(ExecError::Spawn { argv: String::new(), kind: io::ErrorKind::InvalidInput, msg: "空命令".into() }),
        };
        let mut c = Command::new(prog);
        c.args(rest).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
        for (k, v) in &self.env {
            c.env(k, v);
        }
        let t0 = Instant::now();
        let mut child = c.spawn().map_err(|e| ExecError::Spawn { argv: self.display(), kind: e.kind(), msg: e.to_string() })?;
        let out_rx = drain(child.stdout.take());
        let err_rx = drain(child.stderr.take());
        let status =
            wait_with_timeout(&mut child, self.timeout).map_err(|e| ExecError::Wait { argv: self.display(), msg: e.to_string() })?;
        // 子进程已经退出（或被杀）：管道的写端只剩可能的孙进程攥着 —— 最多再等 2 秒，等不到就算截断
        let grace = Duration::from_secs(2);
        let (stdout, so_trunc) = collect(&out_rx, grace);
        let (stderr, se_trunc) = collect(&err_rx, grace);
        Ok(Output { argv: self.display(), status, stdout, stderr, truncated: so_trunc || se_trunc, elapsed: t0.elapsed() })
    }
}

fn quote(a: &str) -> String {
    if !a.is_empty() && a.bytes().all(|c| c.is_ascii_alphanumeric() || b"-_./:=,+@%".contains(&c)) {
        a.to_string()
    } else {
        format!("'{}'", a.replace('\'', r"'\''"))
    }
}

fn drain<R: Read + Send + 'static>(r: Option<R>) -> mpsc::Receiver<io::Result<Vec<u8>>> {
    let (tx, rx) = mpsc::channel();
    match r {
        Some(mut r) => {
            thread::spawn(move || {
                let mut buf = Vec::new();
                let res = r.read_to_end(&mut buf).map(|_| buf);
                // 接收端已经放弃（超过宽限期）时发送失败是预期的：没人要这份输出了
                crate::discard(tx.send(res));
            });
        }
        None => {
            crate::discard(tx.send(Ok(Vec::new())));
        }
    }
    rx
}

fn collect(rx: &mpsc::Receiver<io::Result<Vec<u8>>>, grace: Duration) -> (Vec<u8>, bool) {
    match rx.recv_timeout(grace) {
        Ok(Ok(v)) => (v, false),
        Ok(Err(_)) | Err(_) => (Vec::new(), true),
    }
}

fn wait_with_timeout(child: &mut Child, timeout: Duration) -> io::Result<Status> {
    let t0 = Instant::now();
    let mut nap = Duration::from_millis(1);
    loop {
        if let Some(st) = child.try_wait()? {
            return Ok(Status::from(st));
        }
        if t0.elapsed() >= timeout {
            // SIGKILL；已经退出了（竞争）也没关系，下面的 wait 照样收尸
            crate::discard(child.kill());
            child.wait()?;
            return Ok(Status::TimedOut(timeout));
        }
        thread::sleep(nap);
        nap = (nap * 2).min(Duration::from_millis(20));
    }
}

/// 子进程是怎么结束的
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Exited(i32),
    Signaled(i32),
    TimedOut(Duration),
}

impl From<ExitStatus> for Status {
    fn from(st: ExitStatus) -> Self {
        use std::os::unix::process::ExitStatusExt;
        match (st.code(), st.signal()) {
            (Some(c), _) => Status::Exited(c),
            (None, Some(s)) => Status::Signaled(s),
            // 既无退出码也无信号（被 ptrace 停住之类）：按"被信号结束"报，信号号 0
            (None, None) => Status::Signaled(0),
        }
    }
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Status::Exited(c) => write!(f, "退出码 {c}"),
            Status::Signaled(s) => write!(f, "被信号 {s} 结束"),
            Status::TimedOut(t) => write!(f, "{} 秒没结束，已杀掉", t.as_secs()),
        }
    }
}

/// 一次执行的完整记录
#[derive(Debug, Clone)]
pub struct Output {
    pub argv: String,
    pub status: Status,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
    /// 输出没收全（孙进程攥着管道过了宽限期）
    pub truncated: bool,
    pub elapsed: Duration,
}

impl Output {
    /// 退出码在 `accept` 里（且没超时、没被信号杀、输出收全了）⇒ Ok
    pub fn check(self, accept: &[i32]) -> Result<Output, ExecError> {
        match self.status {
            Status::Exited(c) if accept.contains(&c) && !self.truncated => Ok(self),
            _ => Err(ExecError::Status(Box::new(self))),
        }
    }
    pub fn exited(&self, code: i32) -> bool {
        self.status == Status::Exited(code)
    }
    /// stderr 的最后一行（给报错用）
    pub fn stderr_tail(&self) -> String {
        let s = String::from_utf8_lossy(&self.stderr);
        s.trim_end().lines().last().unwrap_or("").to_string()
    }
}

/// 执行失败
#[derive(Debug, Clone)]
pub enum ExecError {
    Spawn { argv: String, kind: io::ErrorKind, msg: String },
    Wait { argv: String, msg: String },
    Status(Box<Output>),
}

impl ExecError {
    pub fn argv(&self) -> &str {
        match self {
            ExecError::Spawn { argv, .. } | ExecError::Wait { argv, .. } => argv,
            ExecError::Status(o) => &o.argv,
        }
    }
    /// 命令不存在（shell 版里对应 `command -v` 不过、或者 127）
    pub fn not_found(&self) -> bool {
        matches!(self, ExecError::Spawn { kind: io::ErrorKind::NotFound, .. })
    }
}

impl fmt::Display for ExecError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ExecError::Spawn { argv, msg, .. } => write!(f, "起不来：{argv}（{msg}）"),
            ExecError::Wait { argv, msg } => write!(f, "等不到结束：{argv}（{msg}）"),
            ExecError::Status(o) => {
                write!(f, "{}：{}", o.argv, o.status)?;
                if o.truncated {
                    write!(f, "，输出没收全")?;
                }
                let t = o.stderr_tail();
                if !t.is_empty() {
                    write!(f, "（{t}）")?;
                }
                Ok(())
            }
        }
    }
}

impl std::error::Error for ExecError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn captures_stdout_stderr_and_code() {
        let o = Cmd::new("sh").arg("-c").arg("echo out; echo err >&2; exit 3").run().unwrap();
        assert_eq!(o.stdout, b"out\n");
        assert_eq!(o.stderr, b"err\n");
        assert_eq!(o.status, Status::Exited(3));
        assert!(o.clone().check(&[0]).is_err());
        assert!(o.check(&[0, 3]).is_ok());
    }

    #[test]
    fn spawn_failure_is_typed() {
        let e = Cmd::new("/nonexistent/gk3-no-such-tool").run().unwrap_err();
        assert!(e.not_found(), "{e}");
    }

    #[test]
    fn timeout_kills() {
        let t0 = Instant::now();
        let o = Cmd::new("sleep").arg("5").timeout(Duration::from_millis(200)).run().unwrap();
        assert!(matches!(o.status, Status::TimedOut(_)));
        assert!(t0.elapsed() < Duration::from_secs(4));
        let e = o.check(&[0]).unwrap_err();
        assert!(e.to_string().contains("没结束"), "{e}");
    }

    #[test]
    fn signal_is_typed() {
        let o = Cmd::new("sh").arg("-c").arg("kill -9 $$").run().unwrap();
        assert_eq!(o.status, Status::Signaled(9));
    }

    #[test]
    fn stdin_is_null() {
        // 等 stdin 的命令立刻读到 EOF，不会挂住
        let o = Cmd::new("cat").timeout(Duration::from_secs(5)).run().unwrap();
        assert_eq!(o.status, Status::Exited(0));
        assert!(o.stdout.is_empty());
    }

    #[test]
    fn display_quotes() {
        let c = Cmd::new("sgdisk").arg("-c").arg("1:EFI system partition").arg("it's");
        assert_eq!(c.display(), r"sgdisk -c '1:EFI system partition' 'it'\''s'");
    }
}
