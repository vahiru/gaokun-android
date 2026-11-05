//! gaokun3 安装器后端的 Rust 实现 —— 并行轨道，现役仍是 `scripts/live/installer-lib.sh`。
//! 设计、迁移顺序、与 shell 版的偏差表：`docs/dev/installer-rust-design.md`。
//!
//! 入口与 shell 版的函数同名（`gk3-installer gk3_probe …` ≡ `bash -c '. installer-lib.sh && gk3_probe …'`），
//! stdout、协议行（PROGRESS / ERR / JOB）与退出码要与 shell 版逐字节相同 —— 对拍：`scripts/live/duel-lib.sh`。

pub mod exec;
pub mod gpt;
pub mod host;
pub mod plan;
pub mod preflight;
pub mod probe;
pub mod protocol;
pub mod sh;

use std::io;

use host::{Diag, Host};
use protocol::Out;

/// 明说"这个失败不要紧"的地方（每个调用处都写了理由）。不用 `let _ =`：lint 禁了它 ——
/// 因为它也会悄悄吞掉要紧的失败，而这里每一次忽略都要能被 grep 出来、被审到。
pub(crate) fn discard<T, E>(_: Result<T, E>) {}

/// 已经用 Rust 实现了的入口（对拍脚本据此决定比哪些函数）
pub const IMPLEMENTED: [&str; 3] = ["gk3_preflight", "gk3_probe", "gk3_plan"];

/// 调一个入口。`Ok(None)` = 还没实现这个函数；`Ok(Some(rc))` = shell 版语义下的退出码；
/// `Err` = 输出写不出去（前端关了管道之类）—— 这种时候没法再通知任何人，main 只能以非零退出。
pub fn dispatch(host: &dyn Host, func: &str, args: &[String], out: &mut Out, diag: &mut Diag) -> io::Result<Option<i32>> {
    let rc = match func {
        "gk3_probe" => probe::probe(host, out, diag)?,
        "gk3_plan" => plan::plan(host, args, out, diag)?,
        "gk3_preflight" => preflight::preflight(host, out, diag)?,
        _ => return Ok(None),
    };
    Ok(Some(rc))
}
