//! 守护进程模式(`--serve`):Flowtary 的 d/f 搜索后端。
//!
//! 常驻进程:启动时加载(或建立)索引 → 输出 READY → 在 stdin 上收查询行、
//! stdout 回结果;stdin EOF(主程序退出)时保存缓存并退出。
//!
//! 行协议(UTF-8,TAB 分隔):
//!   收:`Q\t<mode>\t<max>\t<terms>\n`,mode:`f`=仅文件 / `d`=仅文件夹 / `a`=全部,
//!      terms 为原始关键词(此处按空白分词、引号短语整体);
//!   回:`N\t<count>\n` 后跟 count 行 `<d|f>\t<完整路径>\n`;
//!   其余输入回 `E\tbad_request\n`;请求 `EXIT` 可主动结束。
//! 就绪前可输出 `STATUS\t...` 进度行,调用方按行忽略未知前缀即可。

use std::io::{BufRead, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use crate::cache::Cache;
use crate::watcher::start_watcher;

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

fn load_or_build(roots: &[String], reindex: bool) -> Cache {
    let cpath = cache_path();
    if let Some(parent) = cpath.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let mut cache = Cache::new();
    if cpath.exists() && !reindex {
        match Cache::load(&cpath) {
            // 根目录一致才复用;不一致(或旧格式缓存解码失败)整体重建
            Ok(c) if c.roots == roots && !c.roots.is_empty() => {
                println!("STATUS\tloaded {}", c.len());
                return c;
            }
            _ => {}
        }
    }

    println!("STATUS\tindexing");
    let _ = std::io::stdout().flush();
    let start = Instant::now();
    for r in roots {
        let _ = cache.build(Path::new(r));
    }
    cache.roots = roots.to_vec();
    // 防御：根目录存在却索引出 0 条，多半是启动参数的路径被命令行转义弄坏了。
    // 不落盘（避免空缓存被后续启动复用）、不发 READY，直接失败退出，
    // 让调用方走回退后端（Flowtary 侧收到 EOF 会自动回退 Everything IPC）。
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

pub fn serve(dirs: &[String], reindex: bool) {
    // 根路径归一化:统一反斜杠,调用方给 C:/ 或 C:\ 都收敛到同一 roots,
    // 保证缓存复用判定一致
    let roots: Vec<String> = dirs.iter().map(|d| d.replace('/', "\\")).collect();
    let cache = load_or_build(&roots, reindex);
    let cache = Arc::new(Mutex::new(cache));

    if !roots.is_empty() {
        let _ = start_watcher(Arc::clone(&cache), roots.clone(), cache_path());
    }

    println!("READY\t{}", cache.lock().unwrap().len());
    let _ = std::io::stdout().flush();

    let stdin = std::io::stdin();
    let mut out = std::io::stdout().lock();
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        if handle_line(&cache, &line, &mut out) {
            break;
        }
    }
    // stdin EOF(主程序退出):落盘后退出
    let _ = cache.lock().unwrap().save(&cache_path());
}
