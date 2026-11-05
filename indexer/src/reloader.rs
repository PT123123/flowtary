//! Background, resource-guarded reloads for atomically published snapshots.
//! Queries continue against the last complete generation while the next one
//! is loaded; publication is a short RwLock swap after a stable file stamp.
use crate::{catalog::Catalog, metrics, policy::{Budget, Cancellation}, snapshot::{self, FileStamp}};
use anyhow::{Context, Result, ensure};
use std::{fs, path::{Path, PathBuf}, sync::{Arc, RwLock, atomic::{AtomicU64, Ordering}}, thread::{self, JoinHandle}, time::{Duration, Instant, SystemTime, UNIX_EPOCH}};

static RUN_ID:AtomicU64=AtomicU64::new(0);
const MIB:u64=1024*1024;

fn run_paths(root:&Path)->Result<(PathBuf,PathBuf)> {
    fs::create_dir_all(root).with_context(||format!("create snapshot telemetry directory {}",root.display()))?;
    let now=SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
    let id=RUN_ID.fetch_add(1,Ordering::Relaxed);
    let stem=format!("snapshot-load-{}-{now}-{id}",std::process::id());
    Ok((root.join(format!("{stem}.jsonl")),root.join(format!("{stem}.json"))))
}

fn write_report(path:&Path, value:&serde_json::Value) {
    if let Ok(bytes)=serde_json::to_vec_pretty(value) { let _=fs::write(path,bytes); }
}
fn report_path(root:&Path,prefix:&str)->PathBuf {
    let now=SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
    root.join(format!("{prefix}-{}-{now}.json",std::process::id()))
}

/// Load one bounded snapshot while sampling all six machine-wide resource
/// dimensions. A child token lets a failed refresh stop itself without stopping
/// the query host; service shutdown still propagates through the parent token.
pub fn load_snapshot(path:&Path,telemetry_dir:&Path,parent_cancel:&Cancellation,kind:&str)->Result<(Catalog,FileStamp)> {
    let expected=snapshot::file_stamp(path)?;
    let (samples_path,report_path)=run_paths(telemetry_dir)?;
    let cancel=Cancellation::child_of(parent_cancel);
    let budget=Budget {minimum_available_bytes:3*1024*MIB,..Budget::default()};
    let recorder=metrics::Recorder::start_guarded(&samples_path,Duration::from_secs(1),Some((budget,cancel.clone())))
        .with_context(||format!("start whole-machine guard for {kind}"))?;
    recorder.phase("snapshot_load")?;
    let loaded=snapshot::load(path,&cancel);
    let boundary=recorder.phase("snapshot_load_complete");
    let samples=recorder.finish().context("finish whole-machine snapshot-load telemetry")?;
    let cancelled=cancel.is_cancelled();
    let stamp=snapshot::file_stamp(path);
    let stable=stamp.as_ref().is_ok_and(|current|*current==expected);
    let outcome=if cancelled {"resource-guard-cancelled"} else if loaded.is_err() {"load-failed"}
        else if !stable {"snapshot-changed-during-load"} else if boundary.is_err() {"telemetry-boundary-failed"} else {"loaded"};
    write_report(&report_path,&serde_json::json!({
        "schema":1,"kind":kind,"outcome":outcome,"snapshot":path,"expected_generation":expected,
        "actual_generation":stamp.as_ref().ok(),"cancelled":cancelled,
        "error":loaded.as_ref().err().map(|e|format!("{e:#}")),
        "telemetry_error":boundary.as_ref().err().map(|e|format!("{e:#}")),
        "samples":samples.len(),"phases":metrics::summarize(&samples),"samples_file":samples_path,
    }));
    ensure!(!cancelled,"machine resource guard cancelled {kind}");
    boundary.context("snapshot-load telemetry boundary failed")?;
    let current=stamp?;
    ensure!(current==expected,"snapshot changed during {kind}; retaining the previous complete generation");
    Ok((loaded.context("load complete snapshot generation")?,current))
}

pub struct SnapshotReloader { stop:Cancellation, thread:Option<JoinHandle<()>> }
impl SnapshotReloader {
    pub fn start(path:PathBuf,telemetry_dir:PathBuf,initial:FileStamp,catalog:Arc<RwLock<Catalog>>,
                 parent_cancel:&Cancellation)->Result<Self> {
        let stop=Cancellation::child_of(parent_cancel);
        let worker_stop=stop.clone();
        let thread=thread::Builder::new().name("flowtary-snapshot-reloader".into()).spawn(move||{
            let mut active=initial;
            let mut retry_after:Option<Instant>=None;
            let mut retry_delay=Duration::from_secs(5);
            while !worker_stop.is_cancelled() {
                thread::sleep(Duration::from_secs(1));
                if worker_stop.is_cancelled(){break}
                let observed=match snapshot::file_stamp(&path){Ok(stamp)=>stamp,Err(_)=>continue};
                if observed==active {continue}
                if retry_after.is_some_and(|deadline|Instant::now()<deadline){continue}
                let Ok((next,generation))=load_snapshot(&path,&telemetry_dir,&worker_stop,"snapshot-generation-reload") else {
                    retry_after=Some(Instant::now()+retry_delay);
                    retry_delay=(retry_delay*2).min(Duration::from_secs(300));
                    continue
                };
                if worker_stop.is_cancelled(){break}
                let entries=next.index.len();
                if let Ok(mut current)=catalog.write() {
                    *current=next;
                    active=generation;
                    retry_after=None;
                    retry_delay=Duration::from_secs(5);
                    write_report(&report_path(&telemetry_dir,"snapshot-published"),&serde_json::json!({
                        "schema":1,"pid":std::process::id(),"phase":"published","generation":generation,
                        "entries":entries,"published_unix_ms":SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis()
                    }));
                }
            }
        }).context("start snapshot generation watcher")?;
        Ok(Self{stop,thread:Some(thread)})
    }
}
impl Drop for SnapshotReloader {
    fn drop(&mut self) {
        self.stop.cancel();
        if let Some(thread)=self.thread.take(){let _=thread.join();}
    }
}
