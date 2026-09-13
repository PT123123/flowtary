//! 守护进程模式(`--serve`):Flowtary 的 d/f 搜索后端。
//!
//! 常驻单例:绑定 127.0.0.1:<port>,绑定失败即视为已有实例在跑,新拉起的进程
//! 直接退出。生命周期独立于 Flowtary(由主程序 detached 拉起,不随主程序退出):
//! 首次全盘索引可能耗时很久,独立存活保证跨多次 Flowtary 重启也只需完成一次;
//! 之后每次启动从版本化缓存秒级恢复,不再重新遍历磁盘。无客户端且空闲超过
//! `--idle-exit-secs` 时落盘退出,不给系统留常驻内存。
//!
//! 行协议(UTF-8,每条连接独立):
//!   连接后先发握手行 `ALTSEARCH\t1`,随后 `STATUS\tindexing`(可重复)直到
//!   `READY\t<条目数>`;之后查询:`Q\t<mode>\t<max>\t<terms>` → `N\t<count>` +
//!   count 行 `<d|f>\t<完整路径>`;`EXIT` 断开连接;未知输入回 `E\tbad_request`。

use std::io::{BufRead, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::cache::{Cache, CACHE_VERSION};
use crate::watcher::start_watcher;

const PROTO: &str = "ALTSEARCH\t1";

struct Shared {
    cache: Arc<Mutex<Cache>>,
    ready: AtomicBool,
    entries: AtomicUsize,
    indexing: AtomicBool,
    clients: AtomicUsize,
    last_client_ms: AtomicU64,
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn cache_path() -> PathBuf {
    std::env::var("APPDATA")
        .map(|appdata| PathBuf::from(appdata).join("AltSearch").join("cache.bin"))
        .unwrap_or_else(|_| PathBuf::from("cache.bin"))
}

/// 分词:空白分隔,引号短语整体(与 Flowtary 侧 BuildModifierQuery 的分词对齐)
fn tokenize(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut it = s.chars().peekable();
    while let Some(&c) = it.peek() {
        if c.is_whitespace() {
            it.next();
            continue;
        }
        let mut term = String::new();
        if c == '"' {
            it.next();
            for c2 in it.by_ref() {
                if c2 == '"' {
                    break;
                }
                term.push(c2);
            }
        } else {
            for c2 in it.by_ref() {
                if c2.is_whitespace() {
                    break;
                }
                term.push(c2);
            }
        }
        if !term.is_empty() {
            out.push(term);
        }
    }
    out
}

fn load_or_build(roots: &[String], reindex: bool, threads: usize) -> Cache {
    let cpath = cache_path();
    if let Some(parent) = cpath.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let mut cache = Cache::new();
    if cpath.exists() && !reindex {
        match Cache::load(&cpath) {
            // 版本一致 + 根目录一致才复用;缓存升版/根变化/解码失败都整体重建
            Ok(c) if c.version == CACHE_VERSION && c.roots == roots && !c.roots.is_empty() => {
                println!("STATUS\tloaded {}", c.len());
                return c;
            }
            Ok(_) => eprintln!("altsearch: cache version/roots mismatch, reindexing"),
            Err(_) => eprintln!("altsearch: cache unreadable, reindexing"),
        }
    }

    println!("STATUS\tindexing");
    let _ = std::io::stdout().flush();
    let start = Instant::now();
    let par = jwalk::Parallelism::RayonNewPool(threads);
    for r in roots {
        let _ = cache.build_par(Path::new(r), par.clone());
    }
    cache.roots = roots.to_vec();
    cache.version = CACHE_VERSION;
    // 防御:根目录存在却索引出 0 条,多半是启动参数的路径被命令行转义弄坏了。
    // 不落盘(避免空缓存被后续启动复用)、不发 READY,直接失败退出,
    // 让调用方走回退后端(Flowtary 侧收到 EOF 会自动回退 Everything IPC)。
    if cache.len() == 0 {
        eprintln!("altsearch: indexed 0 entries from {:?}", roots);
        std::process::exit(2);
    }
    let _ = cache.save(&cpath);
    println!("STATUS\tindexed {}\t{}ms", cache.len(), start.elapsed().as_millis());
    cache
}

fn handle_line(cache: &Arc<Mutex<Cache>>, line: &str, out: &mut impl Write) -> bool {
    let line = line.trim_end();
    if line.is_empty() {
        return false;
    }
    let f: Vec<&str> = line.split('\t').collect();
    match f.first().copied() {
        Some("Q") if f.len() >= 4 => {
            let is_dir = match f[1] {
                "d" => Some(true),
                "f" => Some(false),
                _ => None,
            };
            let max: usize = f[2].parse().unwrap_or(10).clamp(1, 200);
            let terms = tokenize(f[3]);
            let hits = cache.lock().unwrap().search_top(&terms, max, is_dir);
            let _ = writeln!(out, "N\t{}", hits.len());
            for (is_dir, p) in hits {
                let _ = writeln!(out, "{}\t{}", if is_dir { 'd' } else { 'f' }, p);
            }
            let _ = out.flush();
        }
        Some("EXIT") => return true,
        _ => {
            let _ = writeln!(out, "E\tbad_request");
            let _ = out.flush();
        }
    }
    false
}

fn serve_client(stream: TcpStream, shared: &Shared) -> std::io::Result<()> {
    let _ = stream.set_nodelay(true);
    let mut w = stream.try_clone()?;
    writeln!(w, "{}", PROTO)?;
    let _ = w.flush();
    // 等 READY:首次索引期间周期性发 STATUS,客户端据此提示进度
    loop {
        if shared.ready.load(Ordering::SeqCst) {
            writeln!(w, "READY\t{}", shared.entries.load(Ordering::SeqCst))?;
            let _ = w.flush();
            break;
        }
        writeln!(w, "STATUS\tindexing")?;
        let _ = w.flush();
        std::thread::sleep(Duration::from_millis(1000));
    }
    let mut reader = std::io::BufReader::new(stream.try_clone()?);
    let mut line = String::new();
    loop {
        line.clear();
        let n = reader.read_line(&mut line)?;
        if n == 0 {
            return Ok(()); // 客户端断开
        }
        if handle_line(&shared.cache, &line, &mut w) {
            return Ok(()); // EXIT
        }
    }
}

fn handle_client(stream: TcpStream, shared: Arc<Shared>) {
    shared.clients.fetch_add(1, Ordering::SeqCst);
    let _ = serve_client(stream, &shared);
    shared.clients.fetch_sub(1, Ordering::SeqCst);
    shared.last_client_ms.store(now_ms(), Ordering::SeqCst);
}

pub fn serve(dirs: &[String], reindex: bool, port: u16, threads: Option<usize>, idle_secs: u64) {
    // 根路径归一化:统一反斜杠,调用方给 C:/ 或 C:\ 都收敛到同一 roots,
    // 保证缓存复用判定一致
    let roots: Vec<String> = dirs.iter().map(|d| d.replace('/', "\\")).collect();

    // 单例:绑定失败 = 已有实例在跑,新进程自消,现役实例继续服务
    let listener = match TcpListener::bind(("127.0.0.1", port)) {
        Ok(l) => l,
        Err(_) => {
            eprintln!("altsearch: port {} busy, another instance is running", port);
            return;
        }
    };

    // 索引遍历并行度默认压到 2~4 线程:全盘首次索引耗时以小时计时,
    // 吃满所有核(约 80% CPU)不可接受;限流后约 15~25% CPU 可长期后台运行
    let threads = threads.unwrap_or_else(|| {
        let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
        (cores / 4).clamp(2, 4)
    });

    let shared = Arc::new(Shared {
        cache: Arc::new(Mutex::new(Cache::new())),
        ready: AtomicBool::new(false),
        entries: AtomicUsize::new(0),
        indexing: AtomicBool::new(true),
        clients: AtomicUsize::new(0),
        last_client_ms: AtomicU64::new(now_ms()),
    });

    // 索引加载/建立放后台线程:监听先就位,客户端连上即见 STATUS/READY
    {
        let shared = Arc::clone(&shared);
        let roots2 = roots.clone();
        std::thread::spawn(move || {
            let cache = load_or_build(&roots2, reindex, threads);
            let n = cache.len();
            *shared.cache.lock().unwrap() = cache;
            shared.entries.store(n, Ordering::SeqCst);
            shared.indexing.store(false, Ordering::SeqCst);
            shared.ready.store(true, Ordering::SeqCst);
            if !roots2.is_empty() {
                let _ = start_watcher(Arc::clone(&shared.cache), roots2, cache_path());
            }
        });
    }

    // 空闲退出:无客户端且超过 idle_secs → 落盘退出(索引期间/有客户端不退)
    {
        let shared = Arc::clone(&shared);
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_secs(15));
            if shared.indexing.load(Ordering::SeqCst) {
                continue;
            }
            if shared.clients.load(Ordering::SeqCst) > 0 {
                continue;
            }
            let idle_ms = now_ms().saturating_sub(shared.last_client_ms.load(Ordering::SeqCst));
            if idle_secs > 0 && idle_ms >= idle_secs.saturating_mul(1000) {
                let _ = shared.cache.lock().unwrap().save(&cache_path());
                std::process::exit(0);
            }
        });
    }

    for stream in listener.incoming() {
        let Ok(stream) = stream else { continue };
        let shared = Arc::clone(&shared);
        std::thread::spawn(move || handle_client(stream, shared));
    }
}
