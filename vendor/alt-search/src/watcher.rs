use std::sync::{Arc, Mutex};
use std::path::{Path, PathBuf};
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
        let mut changes = 0;

        for result in rx {
            match result {
                Ok(event) => handle_event(event, &cache, &cache_path, &mut changes),
                Err(e) => eprintln!("watch error: {:?}", e),
            }
        }
    });

    Ok(())
}

fn handle_event(event: Event, cache: &Arc<Mutex<Cache>>, cache_path: &PathBuf, changes: &mut i32) {
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

    if *changes >= 50 {
        let _ = cache.save(cache_path);
        *changes = 0;
    }
}