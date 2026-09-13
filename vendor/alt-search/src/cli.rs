use std::path::{Path, PathBuf};
use std::time::Instant;
use clap::Parser;
use std::sync::{Arc, Mutex};
use altsearch::cache::{Cache, FileEntry};
use altsearch::search::{search, Query};
use altsearch::watcher::start_watcher;

#[derive(Parser)]
#[command(name = "altsearch")]
pub struct Cli {
    /// 索引根目录,可重复传入(--dir C:\ --dir D:\);缺省为当前目录
    #[arg(short, long)]
    pub dir: Vec<String>,

    #[arg(short, long)]
    pub name: Option<String>,

    #[arg(short, long)]
    pub ext: Option<String>,

    #[arg(long)]
    pub min_size: Option<u64>,

    #[arg(long)]
    pub max_size: Option<u64>,

    #[arg(long)]
    pub dirs_only: bool,

    #[arg(long)]
    pub files_only: bool,

    #[arg(long)]
    pub reindex: bool,

    /// 守护进程模式:常驻单例,监听 127.0.0.1 收查询(Flowtary 调用)
    #[arg(long)]
    pub serve: bool,

    /// 守护进程监听端口(单例判据:端口被占即已有实例)
    #[arg(long, default_value_t = 47771)]
    pub port: u16,

    /// 索引遍历线程数上限(默认 2~4,压低首次建索引的 CPU 占用)
    #[arg(long)]
    pub threads: Option<usize>,

    /// 无客户端空闲多少秒后落盘退出(0=永不退出)
    #[arg(long, default_value_t = 1800)]
    pub idle_exit_secs: u64,
}

pub fn build_query(cli: &Cli) -> Query {
    Query {
        name_contains: cli.name.clone(),
        extension: cli.ext.clone(),
        min_size: cli.min_size,
        max_size: cli.max_size,
        is_dir: match (cli.dirs_only, cli.files_only) {
            (true, _) => Some(true),
            (_, true) => Some(false),
            _ => None,
        },
        ..Query::new()
    }
}

pub fn print_results(results: &[&FileEntry]) {
    if results.is_empty() {
        println!("No results found.");
        return;
    }

    println!("Found {} results.", results.len());

    for entry in results {
        println!("{}", entry.name);
    }
}

pub fn run(cli: &Cli) {
    let cache_path = std::env::var("APPDATA")
        .map(|appdata| PathBuf::from(appdata).join("AltSearch").join("cache.bin"))
        .unwrap_or_else(|_| PathBuf::from("cache.bin"));

    if let Some(parent) = cache_path.parent() {
        std::fs::create_dir_all(parent).unwrap();
    }

    let dirs: Vec<String> = if cli.dir.is_empty() {
        vec![".".to_string()]
    } else {
        cli.dir.clone()
    };

    let mut cache = Cache::new();
    let mut loaded = false;
    if cache_path.exists() && !cli.reindex {
        let start = Instant::now();
        if let Ok(c) = Cache::load(&cache_path) {
            if c.version == altsearch::cache::CACHE_VERSION {
                cache = c;
                loaded = true;
                println!("Cache loaded in {}ms", start.elapsed().as_millis());
            }
        }
    }
    if !loaded {
        let start = Instant::now();
        for d in &dirs {
            let _ = cache.build(Path::new(d));
        }
        println!("Indexed {} entries in {}ms", cache.len(), start.elapsed().as_millis());
        cache.save(&cache_path).unwrap();
    }

    let cache = Arc::new(Mutex::new(cache));
    let _ = start_watcher(Arc::clone(&cache), dirs.clone(), cache_path);

    let cache = cache.lock().unwrap();
    let query = build_query(cli);

    let start = Instant::now();
    let results = search(&cache, &query);
    println!("Search took {}ms", start.elapsed().as_millis());

    print_results(&results);
}
