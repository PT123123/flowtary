use std::sync::{Arc, Mutex};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};
use notify::{Config, Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use notify::event::{RenameMode, ModifyKind};
use crate::cache::Cache;

pub fn start_watcher(
    cache: Arc<Mutex<Cache>>,
    paths: Vec<String>,
    cache_path: PathBuf,
) -> notify::Result<()> {
    let (tx, rx) = std::sync::mpsc::channel::<notify::Result<Event>>();
    let mut watcher = RecommendedWatcher::new(tx, Config::default())?;

    for path in &paths {
        watcher.watch(Path::new(path), RecursiveMode::Recursive)?;
    }

    std::thread::spawn(move || {
        let _watcher = watcher;
        let mut changes: i32 = 0;
        let mut last_save = Instant::now();

        for result in rx {
            match result {
                Ok(event) => handle_event(event, &cache, &mut changes),
                Err(e) => eprintln!("watch error: {:?}", e),
            }
            // 大索引落盘一次要数十秒且期间查询被锁:攒够条数且距上次落盘
            // 超过 5 分钟才写,避免频繁全量保存卡住查询
            if changes >= 500 && last_save.elapsed() >= Duration::from_secs(300) {
                let _ = cache.lock().unwrap().save(&cache_path);
                changes = 0;
                last_save = Instant::now();
            }
        }
    });

    Ok(())
}

fn handle_event(event: Event, cache: &Arc<Mutex<Cache>>, changes: &mut i32) {
    let mut cache = cache.lock().unwrap();

    match event.kind {
        EventKind::Create(_) => {
            for path in &event.paths { cache.add_entry(path); }
            *changes += 1;
        }
        EventKind::Remove(_) => {
            for path in &event.paths { cache.remove_entry(path); }
            *changes += 1;
        }
        EventKind::Modify(ModifyKind::Data(_)) | EventKind::Modify(ModifyKind::Metadata(_)) => {
            for path in &event.paths { cache.update_entry(path); }
            *changes += 1;
        }
        EventKind::Modify(ModifyKind::Name(RenameMode::Both)) => {
            if event.paths.len() == 2 {
                cache.remove_entry(&event.paths[0]);
                cache.add_entry(&event.paths[1]);
                *changes += 1;
            }
        }
        _ => {}
    }
}
