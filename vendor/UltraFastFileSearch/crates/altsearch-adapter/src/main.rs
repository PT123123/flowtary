// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Flowtary

//! `altsearch` — Flowtary d/f 搜索后端适配层。
//!
//! 保持原 `altsearch.exe --serve` 的 CLI 参数与 `127.0.0.1:47771` 行协议
//! 完全不变（flowtary 主程序零改动），内部把索引与查询全部委托给 UFFS
//! 守护进程（`uffsd`）：
//!
//! * 首次索引：`uffsd` 直接读 NTFS MFT 二进制记录构建，不做全盘目录遍历，
//!   顺序 I/O、CPU 占用远低于旧的 jwalk 全盘扫描；
//! * 增量更新：`uffsd` 轮询 USN Journal 应用增量（Everything 同款方案），
//!   守护进程存活期间索引始终新鲜，不存在“漏掉守护进程停机期间的变更”；
//! * 重启恢复：`uffsd` 从持久化索引热启（秒级），启动不再做全量扫描。
//!
//! MFT 原始卷读取需要管理员权限：非管理员账户下，首次拉起 daemon 时会
//! 通过一次 UAC 请求提权（`--no-elevate` 可关闭）；之后 `uffsd` 常驻
//! （空闲 24h 才退休），后续启动直接复用，不再有 UAC、不再有 CPU 尖峰。
//! 更彻底的无感方案是把 `uffs-broker` 装成 Windows 服务（一次性提权安装），
//! daemon 即可长期以普通用户运行。

#![cfg(windows)]

use std::ffi::OsString;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use clap::Parser;
use uffs_client::connect_sync::UffsClientSync;
use uffs_client::error::ClientError;
use uffs_client::protocol::response::SearchPayload;
use uffs_client::protocol::SearchParams;
use uffs_mft::platform::DriveLetter;

/// 断线重连时需要恢复的盘符列表与提权开关（run() 启动时写入）。
static REQUESTED_LETTERS: OnceLock<Vec<DriveLetter>> = OnceLock::new();
static NO_ELEVATE: AtomicBool = AtomicBool::new(false);

/// uffsd 对 IPC 连接有 5 分钟空闲超时（S4.4.8，`IDLE_CONNECTION_SECS=300`），
/// 超时直接断开。适配层持有一条长连接，必须周期性 keepalive 保活。
const DAEMON_KEEPALIVE_SECS: u64 = 120;
const DEFAULT_PORT: u16 = 47771;
/// 与旧 alt-search 一致：空闲 30 分钟无查询则退出（flowtary 会自动重新拉起）。
const DEFAULT_IDLE_EXIT_SECS: u64 = 1800;
/// uffsd 空闲多久退休。设得远大于适配层空闲退出时间，保证下次启动时
/// daemon 通常仍在运行 → 无 UAC、无冷启。
const DEFAULT_DAEMON_IDLE_SECS: u64 = 86_400;
/// 首次冷建（MFT 读取）的等待上限（空闲预算 10 分钟，daemon 端 5x 硬顶）。
const READY_IDLE_BUDGET_SECS: u64 = 600;
/// 多关键词时按首词取回的宽集上限（再在本地按剩余词过滤，保 AND 语义）。
const MULTI_TERM_FETCH_MULT: usize = 100;
const MULTI_TERM_FETCH_MIN: usize = 200;
const MULTI_TERM_FETCH_MAX: usize = 2_000;

#[derive(Parser, Debug)]
#[command(name = "altsearch", version, about = "Flowtary d/f backend (UFFS adapter)")]
struct Cli {
    /// Run as resident daemon (Flowtary launches with this flag)
    #[arg(long)]
    serve: bool,
    /// Root directory to index, e.g. "C:\\" (repeatable; drive roots preferred)
    #[arg(long = "dir")]
    dirs: Vec<String>,
    /// ALTSEARCH TCP port
    #[arg(long, default_value_t = DEFAULT_PORT)]
    port: u16,
    /// Force full rebuild of the UFFS index (ignore cache)
    #[arg(long)]
    reindex: bool,
    /// Accepted for compatibility; UFFS MFT read is sequential I/O (low CPU)
    #[arg(long)]
    threads: Option<usize>,
    /// Exit after this many seconds with no client activity
    #[arg(long, default_value_t = DEFAULT_IDLE_EXIT_SECS)]
    idle_exit_secs: u64,
    /// Never trigger a UAC elevation prompt (daemon must already be running)
    #[arg(long)]
    no_elevate: bool,
    /// uffsd idle timeout before it retires
    #[arg(long, default_value_t = DEFAULT_DAEMON_IDLE_SECS)]
    daemon_idle_secs: u64,
}

fn main() {
    let cli = Cli::parse();
    if !cli.serve {
        eprintln!("altsearch: only --serve mode is supported (Flowtary d/f backend).");
        std::process::exit(1);
    }
    let code = match run(cli) {
        Ok(code) => code,
        Err(err) => {
            eprintln!("altsearch: {err}");
            2
        }
    };
    std::process::exit(code);
}

fn run(cli: Cli) -> Result<i32, Box<dyn std::error::Error>> {
    // 1. --dir 根目录 → 盘符（去重、仅保留 NTFS 卷）。
    let mut requested: Vec<DriveLetter> = Vec::new();
    for dir in &cli.dirs {
        let c = dir.trim_end_matches(['\\', '/']).chars().next();
        if let Some(c) = c.filter(|c| c.is_ascii_alphabetic()) {
            if let Ok(letter) = DriveLetter::parse(c.to_ascii_uppercase()) {
                if !requested.contains(&letter) {
                    requested.push(letter);
                }
            }
        }
    }
    if requested.is_empty() {
        return Ok(2); // 没有可用盘符，flowtary 会自动回退 Everything
    }
    let requested: Vec<DriveLetter> =
        requested.into_iter().filter(|l| volume_is_ntfs(*l)).collect();
    if requested.is_empty() {
        eprintln!("altsearch: none of the requested roots are NTFS volumes");
        return Ok(2);
    }
    let _ = REQUESTED_LETTERS.set(requested.clone());
    NO_ELEVATE.store(cli.no_elevate, Ordering::SeqCst);

    // 调优 uffsd 常驻 CPU：默认 500ms 的 USN 轮询（每轮含 MFT extent 映射重建）
    // 约占 12% 单核。这里把轮询间隔放宽到 2s——按 Everything 的增量更新思路，
    // 变化累积 2s 一起应用即可，搜索新鲜度不受影响；用户显式设置时保持原值。
    if std::env::var_os("UFFS_USN_POLL_INTERVAL_MS").is_none() {
        // Safety: 此处仍在单线程启动阶段（任何线程尚未创建），
        // 不存在并发读 env 的数据竞争。
        unsafe { std::env::set_var("UFFS_USN_POLL_INTERVAL_MS", "2000") };
    }

    // 2. 先绑定 ALTSEARCH 端口：flowtary 立即能连上并收到 STATUS（与旧行为一致）。
    let listener = TcpListener::bind(("127.0.0.1", cli.port))?;

    // 3. 后台线程连接/拉起 uffsd 并等待就绪；主线程同时开始服务协议。
    let ready = Arc::new(AtomicBool::new(false));
    let ready_count = Arc::new(AtomicU64::new(0));
    let daemon = Arc::new(Mutex::new(None::<UffsClientSync>));

    {
        let ready = Arc::clone(&ready);
        let ready_count = Arc::clone(&ready_count);
        let daemon = Arc::clone(&daemon);
        let letters = requested.clone();
        let spawn = spawn_args(&letters, cli.daemon_idle_secs);
        let reindex = cli.reindex;
        let no_elevate = cli.no_elevate;
        std::thread::Builder::new()
            .name("uffs-connect".to_string())
            .spawn(move || match connect_and_wait(&letters, &spawn, reindex, no_elevate) {
                Ok(mut client) => {
                    let count = client
                        .drives()
                        .ok()
                        .map(|r| r.drives.iter().map(|d| d.records as u64).sum())
                        .unwrap_or(0);
                    if let Ok(mut guard) = daemon.lock() {
                        *guard = Some(client);
                    }
                    ready_count.store(count, Ordering::SeqCst);
                    ready.store(true, Ordering::SeqCst);
                }
                Err(err) => {
                    eprintln!("altsearch: failed to bring up UFFS daemon: {err}");
                    // 保持进程存活一小段时间让 flowtary 至少收到 STATUS 后退出；
                    // flowtary 会自动回退 Everything。
                    std::thread::sleep(Duration::from_secs(3));
                    std::process::exit(2);
                }
            })?
    };

    // 3b. keepalive 保活：daemon 对 IPC 连接 5 分钟空闲即断开（S4.4.8），
    //     这里每 120s ping 一次，保证长连接跨查询间隔存活。
    {
        let daemon = Arc::clone(&daemon);
        std::thread::Builder::new()
            .name("uffs-keepalive".to_string())
            .spawn(move || loop {
                std::thread::sleep(Duration::from_secs(DAEMON_KEEPALIVE_SECS));
                if let Ok(mut guard) = daemon.lock() {
                    if let Some(client) = guard.as_mut() {
                        let _ = client.keepalive();
                    }
                }
            })?
    };

    // 4. 空闲退出监视。
    let last_activity = Arc::new(AtomicU64::new(0));
    {
        let ready = Arc::clone(&ready);
        let last_activity = Arc::clone(&last_activity);
        let daemon = Arc::clone(&daemon);
        let idle_ms = cli.idle_exit_secs.saturating_mul(1000);
        std::thread::Builder::new()
            .name("idle-exit".to_string())
            .spawn(move || loop {
                std::thread::sleep(Duration::from_secs(10));
                if !ready.load(Ordering::SeqCst) {
                    continue;
                }
                let last = last_activity.load(Ordering::SeqCst);
                let now = Instant::now().elapsed().as_millis() as u64;
                if last != 0 && now.saturating_sub(last) >= idle_ms {
                    eprintln!("altsearch: idle timeout reached, retiring daemon");
                    if let Ok(mut guard) = daemon.lock() {
                        if let Some(client) = guard.as_mut() {
                            let _ = client.shutdown();
                        }
                    }
                    std::process::exit(0);
                }
            })?
    };

    // 5. 服务 ALTSEARCH 协议。
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(s) => s,
            Err(_) => continue,
        };
        last_activity.store(Instant::now().elapsed().as_millis() as u64, Ordering::SeqCst);
        let ready = Arc::clone(&ready);
        let ready_count = Arc::clone(&ready_count);
        let daemon = Arc::clone(&daemon);
        let last_activity = Arc::clone(&last_activity);
        std::thread::Builder::new()
            .name("alt-client".to_string())
            .spawn(move || {
                let _ = serve_client(stream, &ready, &ready_count, &daemon, &last_activity);
            })?;
    }

    Ok(0)
}

fn spawn_args(letters: &[DriveLetter], daemon_idle_secs: u64) -> Vec<OsString> {
    let mut args: Vec<OsString> = Vec::new();
    for letter in letters {
        args.push("--drive".into());
        args.push(letter.to_string().into());
    }
    args.push("--idle-timeout".into());
    args.push(daemon_idle_secs.to_string().into());
    args
}

fn connect_and_wait(
    letters: &[DriveLetter],
    spawn_args: &[OsString],
    reindex: bool,
    no_elevate: bool,
) -> Result<UffsClientSync, ClientError> {
    let mut client = match UffsClientSync::connect_with_args(spawn_args) {
        Ok(c) => c,
        Err(ClientError::DaemonNeedsElevation { .. }) if !no_elevate => {
            eprintln!(
                "altsearch: UFFS daemon needs elevation to read the NTFS MFT — \
                 requesting one-time UAC approval (first launch only)"
            );
            UffsClientSync::connect_with_elevation(spawn_args)?
        }
        Err(err) => return Err(err),
    };

    // 确保 flowtary 请求的盘都已加载；已加载的（含复用常驻 daemon 时）跳过。
    let loaded = client.drives()?.drives;
    let loaded_letters: Vec<DriveLetter> = loaded.iter().map(|d| d.letter).collect();
    let missing: Vec<DriveLetter> = letters
        .iter()
        .copied()
        .filter(|l| !loaded_letters.contains(l))
        .collect();
    if reindex {
        let _ = client.load_drive_letters(letters, true)?;
    } else if !missing.is_empty() {
        let _ = client.load_drive_letters(&missing, false)?;
    }

    client.await_ready(Duration::from_secs(READY_IDLE_BUDGET_SECS))?;
    Ok(client)
}

fn serve_client(
    stream: TcpStream,
    ready: &AtomicBool,
    ready_count: &AtomicU64,
    daemon: &Mutex<Option<UffsClientSync>>,
    last_activity: &AtomicU64,
) -> std::io::Result<()> {
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut writer = stream;

    // 握手：先发协议标识，随后在 daemon 就绪前周期性发 STATUS，就绪后发 READY。
    writeln!(writer, "ALTSEARCH\t1")?;
    while !ready.load(Ordering::SeqCst) {
        writeln!(writer, "STATUS\tindexing")?;
        writer.flush()?;
        std::thread::sleep(Duration::from_millis(500));
    }
    writeln!(writer, "READY\t{}", ready_count.load(Ordering::SeqCst))?;
    writer.flush()?;

    let mut line = String::new();
    loop {
        line.clear();
        let n = reader.read_line(&mut line)?;
        if n == 0 {
            return Ok(());
        }
        let line = line.trim_end_matches(['\r', '\n']);
        last_activity.store(Instant::now().elapsed().as_millis() as u64, Ordering::SeqCst);
        if line == "EXIT" {
            return Ok(());
        }
        if let Some(rest) = line.strip_prefix('Q') {
            let parts: Vec<&str> = rest.strip_prefix('\t').unwrap_or(rest).split('\t').collect();
            if parts.len() < 3 {
                writeln!(writer, "E\tbad_request")?;
                continue;
            }
            let max: usize = parts[1].parse().unwrap_or(10).clamp(1, 200);
            let terms = tokenize(parts[2]);
            if terms.is_empty() {
                writeln!(writer, "N\t0")?;
                writer.flush()?;
                continue;
            }
            match query(daemon, &terms, parts[0], max) {
                Ok(rows) => {
                    writeln!(writer, "N\t{}", rows.len())?;
                    for (is_dir, path) in rows {
                        writeln!(writer, "{}\t{}", if is_dir { 'd' } else { 'f' }, path)?;
                    }
                }
                Err(err) => {
                    eprintln!("altsearch: query failed: {err}");
                    writeln!(writer, "E\tsearch_failed")?;
                }
            }
            writer.flush()?;
        } else {
            writeln!(writer, "E\tbad_request")?;
            writer.flush()?;
        }
    }
}

fn query(
    daemon: &Mutex<Option<UffsClientSync>>,
    terms: &[String],
    mode: &str,
    max: usize,
) -> Result<Vec<(bool, String)>, ClientError> {
    let filter = match mode {
        "f" => Some("files".to_string()),
        "d" => Some("dirs".to_string()),
        _ => None,
    };

    // 单关键词：直接查；多关键词：首词取宽集，本地按剩余词对文件名做 AND 过滤。
    let (pattern, fetch_limit, extra): (String, usize, &[String]) = if terms.len() == 1 {
        (terms[0].clone(), max, &[])
    } else {
        (
            terms[0].clone(),
            (max.saturating_mul(MULTI_TERM_FETCH_MULT))
                .clamp(MULTI_TERM_FETCH_MIN, MULTI_TERM_FETCH_MAX),
            &terms[1..],
        )
    };

    let params = SearchParams {
        pattern: contains_glob(&pattern),
        limit: Some(fetch_limit as u32),
        filter,
        // 默认 match_path=false：按文件名匹配（与旧 alt-search 一致，而非全路径）。
        ..Default::default()
    };

    // 先试当前连接；失败（daemon 5 分钟空闲断连等）则重连一次再重试。
    for attempt in 0..2 {
        {
            let mut guard = daemon.lock().map_err(|_| {
                ClientError::Protocol("daemon mutex poisoned".to_string())
            })?;
            if let Some(client) = guard.as_mut() {
                match client.search(&params) {
                    Ok(resp) => return collect_rows(resp, extra, max),
                    Err(err) if attempt == 0 => {
                        eprintln!("altsearch: daemon connection lost ({err}), reconnecting…");
                        *guard = None;
                    }
                    Err(err) => return Err(err),
                }
            }
        }
        reconnect_daemon(daemon)?;
    }
    Err(ClientError::Protocol("daemon not connected".to_string()))
}

/// UFFS 的 glob 语义：无通配符的模式按**精确匹配**（`GlobKind::Exact`）。
/// 旧 alt-search / Everything 是“包含”语义，故普通词包成 `*term*`；
/// 用户已带 glob 元字符（`*`/`?`/`[`）时保持原样。
fn contains_glob(term: &str) -> String {
    if term.contains(['*', '?', '[']) {
        term.to_string()
    } else {
        format!("*{term}*")
    }
}

/// 重新连接 uffsd（daemon 通常仍在运行，直接连上；若已退休则重新拉起，
/// 需要提权时按 `--no-elevate` 开关决定是否弹 UAC）。
fn reconnect_daemon(daemon: &Mutex<Option<UffsClientSync>>) -> Result<(), ClientError> {
    let mut guard = daemon.lock().map_err(|_| {
        ClientError::Protocol("daemon mutex poisoned".to_string())
    })?;
    if guard.is_some() {
        return Ok(());
    }
    let letters = REQUESTED_LETTERS.get().cloned().unwrap_or_default();
    if letters.is_empty() {
        return Err(ClientError::Protocol("no drives to reconnect".to_string()));
    }
    let spawn = spawn_args(&letters, DEFAULT_DAEMON_IDLE_SECS);
    let mut client = match UffsClientSync::connect_with_args(&spawn) {
        Ok(c) => c,
        Err(ClientError::DaemonNeedsElevation { .. }) if !NO_ELEVATE.load(Ordering::SeqCst) => {
            UffsClientSync::connect_with_elevation(&spawn)?
        }
        Err(err) => return Err(err),
    };
    let loaded = client.drives()?.drives;
    let loaded_letters: Vec<DriveLetter> = loaded.iter().map(|d| d.letter).collect();
    let missing: Vec<DriveLetter> = letters
        .iter()
        .copied()
        .filter(|l| !loaded_letters.contains(l))
        .collect();
    if !missing.is_empty() {
        let _ = client.load_drive_letters(&missing, false)?;
    }
    client.await_ready(Duration::from_secs(READY_IDLE_BUDGET_SECS))?;
    *guard = Some(client);
    Ok(())
}

fn collect_rows(
    resp: uffs_client::protocol::response::SearchResponse,
    extra: &[String],
    max: usize,
) -> Result<Vec<(bool, String)>, ClientError> {
    let mut out: Vec<(bool, String)> = Vec::with_capacity(max);
    match resp.payload {
        SearchPayload::InlineRows(rows) => {
            for row in rows {
                if extra.iter().all(|t| row.name.to_lowercase().contains(t)) {
                    out.push((row.is_directory, row.path));
                    if out.len() >= max {
                        break;
                    }
                }
            }
        }
        SearchPayload::Empty => {}
        other => {
            eprintln!("altsearch: unexpected search payload channel: {other:?}");
        }
    }
    Ok(out)
}

/// 按旧 alt-search 的语义分词：空白分隔，双引号包裹的短语视为一个词。
fn tokenize(input: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut cur = String::new();
    let mut in_quote = false;
    for ch in input.chars() {
        match ch {
            '"' => {
                in_quote = !in_quote;
                if !in_quote && !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                } else if in_quote {
                    cur.clear();
                }
            }
            c if c.is_whitespace() && !in_quote => {
                if !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
            }
            c => cur.push(c),
        }
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// 判断盘符对应的卷是否为 NTFS（无法查询时保守返回 true，交给 uffsd 处理）。
fn volume_is_ntfs(letter: DriveLetter) -> bool {
    let root: Vec<u16> = format!("{letter}:\\")
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    let mut fs_name = [0u16; 16];
    let mut fs_flags = 0u32;

    // SAFETY: root 是有效的以 NUL 结尾的宽字符路径；两个输出缓冲区
    // 均为可写内存，长度由调用方传入。
    let ok = unsafe {
        GetVolumeInformationW(
            root.as_ptr(),
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut fs_flags,
            fs_name.as_mut_ptr(),
            fs_name.len() as u32,
        )
    };
    if ok == 0 {
        return true;
    }
    let len = fs_name.iter().position(|&c| c == 0).unwrap_or(fs_name.len());
    let name = String::from_utf16_lossy(&fs_name[..len]);
    name.eq_ignore_ascii_case("ntfs")
}

#[link(name = "Kernel32")]
unsafe extern "system" {
    /// https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw
    fn GetVolumeInformationW(
        lp_root_path_name: *const u16,
        lp_volume_name_buffer: *mut u16,
        n_volume_name_size: u32,
        lp_volume_serial_number: *mut u32,
        lp_maximum_component_length: *mut u32,
        lp_file_system_flags: *mut u32,
        lp_file_system_name_buffer: *mut u16,
        n_file_system_name_size: u32,
    ) -> i32;
}
