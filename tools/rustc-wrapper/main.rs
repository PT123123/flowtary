// SPDX-License-Identifier: MPL-2.0
// rustc-wrapper.exe — Windows 原生 rustc 转发器。
//
// 用途：UFFS（vendor/UltraFastFileSearch）的 .cargo/config.toml 配置了
// `build.rustc-wrapper = "scripts/dev/rustc-wrapper"`（POSIX sh 转发器），
// Windows 下的官方替换是 rustc-wrapper.cmd，但 cmd.exe 批处理有 8191 字符
// 命令行上限 —— windows-sys 这类 crate 的 rustc 参数（几十个 --cfg + 完整
// check-cfg feature 列表）远超 8K，导致构建失败：
//     error: could not compile `windows-sys` (lib)
//     Caused by: process didn't exit successfully: ...\rustc-wrapper.cmd ...
//     The command line is too long.
//
// 本程序用 std::process::Command 原样转发 argv（CreateProcessW 上限 32K，
// 两跳都足够），并透传退出码，行为与官方 shim 一致（sccache 未安装时直接
// 透传 rustc；本项目不依赖 sccache）。
//
// 构建：rustc -O tools/rustc-wrapper/main.rs -o tools/rustc-wrapper/rustc-wrapper.exe
// 使用：RUSTC_WRAPPER=<abs>/rustc-wrapper.exe cargo build ...

use std::process::Command;

fn main() {
    let mut args = std::env::args_os();
    let _self = args.next().expect("argv[0] missing");
    let program = match args.next() {
        Some(p) => p,
        None => {
            eprintln!("rustc-wrapper: usage: rustc-wrapper <rustc> <args...>");
            std::process::exit(64);
        }
    };
    let rest: Vec<std::ffi::OsString> = args.collect();

    let status = match Command::new(&program).args(&rest).status() {
        Ok(s) => s,
        Err(err) => {
            eprintln!("rustc-wrapper: failed to spawn {:?}: {err}", program);
            std::process::exit(69);
        }
    };
    std::process::exit(status.code().unwrap_or(1));
}
