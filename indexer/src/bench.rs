use crate::{catalog::{Catalog, Query, VolumeMeta, layout}, collector::{self, MetadataSource}, metrics::{self, Recorder}, policy::{Budget, Cancellation}, snapshot};
use anyhow::{Result, ensure, bail};
use goz_core::{index::{LinkTarget,NTFS_ROOT_FRN,WtfName},layout::LayoutFile,types::Frn,usn::{JournalInfo,JournalWalk,ParsedUsnRecord,SkipCounts}};
use serde::Serialize;
use std::{fs, path::Path, time::{Duration, Instant}};

const BENCH_QUERY_INDEX_BUDGET: u64 = 768 * 1024 * 1024;

#[derive(Debug, Serialize)]
struct Latency {
    queries: usize, p50_us: u64, p95_us: u64, max_us: u64,
}
fn timings(c: &Catalog, workshop: bool, cancel: &Cancellation) -> Result<Latency> {
    let started = Instant::now(); let mut times = Vec::new();
    while (times.len() < 100 || started.elapsed() < Duration::from_secs(2)) && times.len() < 500_000 {
        cancel.check()?;
        let q = if workshop { Query { words: vec!["flowtary".into()], workshop_only: true, limit: 10, ..Default::default() } }
            else { Query { words: vec![format!("prj-{:07}", (times.len() * 41) % 10_000)], limit: 10, ..Default::default() } };
        let r = c.query(&q,cancel)?;
        ensure!(!r.hits.is_empty() && !r.truncated, "expected fixture query did not complete");
        times.push(r.elapsed_us);
    }
    times.sort_unstable(); let n = times.len();
    Ok(Latency { queries: n, p50_us: times[n/2], p95_us: times[(n-1)*95/100], max_us: times[n-1] })
}
struct SyntheticChanges { target:i64, records:Vec<ParsedUsnRecord> }
impl MetadataSource for SyntheticChanges {
    fn journal(&mut self)->Result<JournalInfo> { Ok(JournalInfo { journal_id:1,first_usn:0,next_usn:self.target,lowest_valid_usn:0,max_usn:i64::MAX }) }
    fn layout_page(&mut self,_:bool)->Result<Option<Vec<LayoutFile>>> { bail!("offline delta must not enumerate anything") }
    fn changes(&mut self,_:i64,_:u64)->Result<JournalWalk> {
        Ok(JournalWalk { next_usn:self.target,records:std::mem::take(&mut self.records),skipped:SkipCounts::default() })
    }
    fn file_layout(&mut self,_:Frn)->Result<LayoutFile> { bail!("offline rename benchmark has no hard-link changes") }
}
fn incremental(c:&mut Catalog,cancel:&Cancellation)->Result<serde_json::Value> {
    // Synthetic maximum-scale data already exceeds the production collector's
    // current 1,000,000-entry cap once roots and benchmark deltas are included.
    // Keep this test-only budget local; the production limit remains unchanged.
    let synthetic_bytes=c.index.memory().total().allocated.saturating_add(c.query_index_bytes());
    let limits=collector::Limits { page_pause_ms:0,max_entries:c.index.len()+10_000,
        max_index_bytes:synthetic_bytes.saturating_add(64*1024*1024),..Default::default() };
    let mut times=Vec::new();
    for round in 0..50 {
        let target=c.meta.next_usn+100;
        // Keep delta FRNs above the largest supported initial synthetic profile
        // so this workload never mutates a pre-existing hard-link chain.
        let records=(0..32).map(|i|ParsedUsnRecord { major_version:2,frn:Frn(10_000_000+round*32+i),parent_frn:Frn(101),
            usn:target-1,timestamp_ft:0,reason:0x2000,attributes:0,name:format!("renamed-{round:04}-{i:04}.rs").into_bytes(),name_lossy:false }).collect();
        let mut source=SyntheticChanges { target,records };
        let t=Instant::now(); collector::catch_up(c,&mut source,&limits,cancel)?;
        times.push(t.elapsed().as_micros() as u64);
        let result=c.query(&Query {words:vec![format!("renamed-{round:04}")],limit:50,..Default::default()},cancel)?;
        ensure!(result.hits.len()==32 && !result.truncated,"incremental query lost changed names");
    }
    times.sort_unstable();
    Ok(serde_json::json!({"batches":50,"renames_per_batch":32,"renames":1600,
        "batch_p50_us":times[25],"batch_p95_us":times[46],"batch_max_us":times[49],
        "full_query_index_rebuilds":0,"final_usn":c.meta.next_usn}))
}

pub fn run(output_dir: &Path, entries: usize, shape: &str) -> Result<()> {
    ensure!((10_000..=6_000_000).contains(&entries), "offline synthetic benchmark permits 10,000..6,000,000 entries; production collector limits are unchanged");
    ensure!(shape=="shallow" || shape=="nested-hardlinks","unknown offline workload shape");
    fs::create_dir_all(output_dir)?;
    let samples_path = output_dir.join("samples.jsonl");
    let snapshot_path = output_dir.join("fixture.fti");
    let cancel = Cancellation::default();
    // Capacity runs may use more than the production safety envelope while
    // constructing synthetic posting lists. Keep the wider commit allowance
    // local to this offline benchmark; all pressure, available-memory, pool,
    // System-handle, CPU, disk, paging, and absolute commit-ratio guards stay.
    let budget = Budget { maximum_commit_growth_bytes: 1536 * 1024 * 1024, ..Budget::default() };
    let recorder = Recorder::start_guarded(&samples_path,Duration::from_secs(1),Some((budget.clone(),cancel.clone())))?;
    std::thread::sleep(Duration::from_secs(3));
    let work = (|| -> Result<serde_json::Value> {
        recorder.phase("offline_metadata_build")?;
        let t = Instant::now();
        let mut c = Catalog::empty(VolumeMeta { identity: "SYNTHETIC-NO-VOLUME-ACCESS".into(), drive: 'C', journal_id: 1, next_usn: 10, stale: false });
        let root = NTFS_ROOT_FRN.0;
        for f in [layout(100,root,b"workshop",true),layout(101,root,b"data",true),layout(102,root,b"workshop-old",true)] { c.index.insert_layout(&f); }
        for v in 0..20 {
            let dir = 200 + v;
            c.index.insert_layout(&layout(dir,100,format!("Flowtary-1.1.{v}.0").as_bytes(),true));
            c.index.insert_layout(&layout(300+v,dir,b"flowtary.exe",false));
        }
        let mut nested_parents=Vec::new();
        if shape=="nested-hardlinks" {
            for region in 0..64u64 {
                let region_frn=10_000+region;
                c.index.insert_layout(&layout(region_frn,101,format!("region-{region:02}").as_bytes(),true));
                for shard in 0..64u64 {
                    let shard_frn=100_000+region*64+shard;
                    c.index.insert_layout(&layout(shard_frn,region_frn,format!("shard-{shard:02}").as_bytes(),true));
                    nested_parents.push((region,shard,shard_frn));
                }
            }
        }
        let mut synthetic_hardlinks=0usize;
        for i in 0..entries {
            if i % 2048 == 0 { cancel.check()?; }
            let name = format!("prj-{i:07}.{}",if i % 100 == 0 {"exe"} else {"rs"});
            let frn_id=1_000_000+i as u64;
            let parent_frn=if shape=="nested-hardlinks" {
                let region=(i%64) as u64;
                let shard=((i/64)%64) as u64;
                nested_parents[(region*64+shard) as usize].2
            }else{101};
            c.index.insert_layout(&layout(frn_id,parent_frn,name.as_bytes(),false));
            if shape=="nested-hardlinks" && i%1000==0 {
                let region=(i%64) as u64;
                let shard=(((i/64)+1)%64) as u64;
                let alias_parent=nested_parents[(region*64+shard) as usize].2;
                let first_parent=c.index.head_of(Frn(parent_frn)).expect("inserted leaf directory");
                let second_parent=c.index.head_of(Frn(alias_parent)).expect("inserted alias directory");
                let alias=format!("hardlink-{i:07}.rs").into_bytes();
                c.index.reconcile_links(Frn(frn_id),&[
                    LinkTarget{parent:first_parent,name:WtfName::new(name.into_bytes(),false)},
                    LinkTarget{parent:second_parent,name:WtfName::new(alias,false)},
                ]);
                synthetic_hardlinks+=1;
            }
        }
        c.index.optimize_storage();
        let build_ms = t.elapsed().as_millis();
        recorder.phase("offline_query_index_build")?;
        let t = Instant::now(); c.rebuild_query_index_limited(&cancel,BENCH_QUERY_INDEX_BUDGET)?; let postings_ms = t.elapsed().as_millis();
        let core_allocated = c.index.memory().total().allocated;
        let query_allocated = c.query_index_bytes();
        recorder.phase("general_warm_queries")?;
        let general = timings(&c,false,&cancel)?;
        recorder.phase("workshop_warm_queries")?;
        let workshop = timings(&c,true,&cancel)?;
        let best = c.query(&Query { words: vec!["flowtary".into()], workshop_only: true, limit: 1, ..Default::default() },&cancel)?;
        ensure!(best.hits[0].path.contains("1.1.19.0"), "workshop must rank newest release first");
        recorder.phase("bounded_broad_query")?;
        let broad = c.query(&Query { words: vec!["prj".into()], limit: 10, ..Default::default() },&cancel)?;
        recorder.phase("offline_incremental_renames")?;
        let deltas=incremental(&mut c,&cancel)?;
        recorder.phase("snapshot_write")?;
        let t = Instant::now(); snapshot::save(&c,&snapshot_path,&cancel)?; let save_ms = t.elapsed().as_millis();
        let count = c.index.len();
        recorder.phase("release_before_restore")?;
        drop(c); std::thread::sleep(Duration::from_secs(1));
        recorder.phase("snapshot_restore")?;
        let restore_budget=core_allocated.saturating_add(query_allocated).saturating_add(64*1024*1024);
        let t = Instant::now(); let restored = snapshot::load_with_budgets(&snapshot_path,&cancel,restore_budget,BENCH_QUERY_INDEX_BUDGET)?; let load_ms = t.elapsed().as_millis();
        ensure!(restored.index.len() == count, "snapshot lost entries");
        let verified = restored.query(&Query { words: vec!["flowtary".into()], workshop_only: true, limit: 1, ..Default::default() },&cancel)?;
        ensure!(verified.hits == best.hits, "restored workshop query changed");
        recorder.phase("release_and_cooldown")?;
        drop(restored); std::thread::sleep(Duration::from_secs(3));
        cancel.check()?;
        Ok(serde_json::json!({
            "records": entries, "index_entries": count, "workload_shape":shape,
            "synthetic_directory_count":nested_parents.len()+3+40,
            "synthetic_hardlink_files":synthetic_hardlinks,
            "metadata_build_ms": build_ms,
            "query_index_build_ms": postings_ms, "core_allocated_bytes": core_allocated,
            "query_index_allocated_bytes_estimate": query_allocated,
            "general": general, "workshop": workshop, "broad_query": broad,
            "incremental":deltas,
            "snapshot_write_ms": save_ms, "snapshot_restore_ms": load_ms,
            "snapshot_bytes": fs::metadata(&snapshot_path)?.len(), "workshop_best": best.hits,
        }))
    })();
    let samples = recorder.finish()?;
    let work=if work.is_ok() && cancel.is_cancelled(){Err(anyhow::anyhow!("resource guard cancelled during final sampling"))}else{work};
    let first = samples.first().expect("recorder always captures a baseline");
    let last = samples.last().unwrap();
    let startup_cpu_warmup=samples.first().is_some_and(|s|s.cpu_percent.is_none() &&
        s.counter_errors.is_empty() && s.guard_violations.iter().all(|v|v=="machine CPU counter unavailable or above limit"));
    let resource_guard_violation_samples=samples.iter().enumerate()
        .filter(|(i,s)|!(*i==0 && startup_cpu_warmup) && !s.guard_violations.is_empty()).count();
    let (status, result, error) = match work {
        Ok(v) => ("completed",v,None), Err(e) => ("aborted",serde_json::Value::Null,Some(format!("{e:#}"))),
    };
    let report = serde_json::json!({
        "schema": 2, "status": status, "error": error, "mode": "offline-synthetic",
        "source_fingerprint":env!("FLOWTARY_SOURCE_FINGERPRINT"),
        "upstream_revision":"8c6a7b1562bb1eea46dd67d462805b49b9a1f152",
        "native_volume_opens": 0, "directory_enumerations": 0,
        "scope_limit": "Measures data structures, queries, snapshot I/O and actual host counters; does NOT validate NTFS/minifilter safety.",
        "budget": budget, "cancelled_by_guard_or_caller": cancel.is_cancelled(),
        "resource_guard_clean":resource_guard_violation_samples==0,
        "resource_guard_violation_samples":resource_guard_violation_samples,
        "startup_cpu_warmup_sample_excluded":startup_cpu_warmup,
        "native_host_violations": crate::policy::native_host_violations(last),
        "result": result, "phases": metrics::summarize(&samples),
        "first_sample": first, "last_sample": last,
        "whole_run": {
            "commit_peak_growth_bytes": samples.iter().map(|s| s.commit_bytes.saturating_sub(first.commit_bytes)).max(),
            "available_minimum_bytes": samples.iter().map(|s| s.physical_available_bytes).min(),
            "system_handles_peak_growth": samples.iter().filter_map(|s| s.system_handles.zip(first.system_handles).map(|(a,b)| a.saturating_sub(b))).max(),
            "pool_paged_peak_growth_bytes": samples.iter().map(|s| s.pool_paged_bytes.saturating_sub(first.pool_paged_bytes)).max(),
            "pool_nonpaged_peak_growth_bytes": samples.iter().map(|s| s.pool_nonpaged_bytes.saturating_sub(first.pool_nonpaged_bytes)).max(),
            "process_private_peak_bytes": samples.iter().map(|s| s.process_private_bytes).max(),
        },
    });
    fs::write(output_dir.join("report.json"),serde_json::to_vec_pretty(&report)?)?;
    println!("{}",serde_json::to_string_pretty(&report)?);
    ensure!(status == "completed", "benchmark aborted; see {}",output_dir.join("report.json").display());
    Ok(())
}
