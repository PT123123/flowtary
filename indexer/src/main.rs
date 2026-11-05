mod metrics;
mod policy;
mod catalog;
mod snapshot;
mod bench;
mod collector;
mod native;
mod session;
mod mft_names;
mod owner;
mod service;
mod reloader;
use anyhow::{Context, Result};
use collector::MetadataSource;
use std::{collections::HashMap, path::PathBuf, time::{Duration, Instant}};

fn flag(args: &[String], name: &str, default: &str) -> String {
    args.iter().position(|a| a == name).and_then(|i| args.get(i+1)).cloned().unwrap_or_else(|| default.into())
}
fn encode_workload_result(result:&Result<serde_json::Value>)->serde_json::Value {
    match result {
        Ok(value)=>serde_json::json!({"Ok":value}),
        Err(error)=>serde_json::json!({"Err":format!("{error:#}")}),
    }
}
fn json_fragment(fragment:&mft_names::Fragment)->serde_json::Value {
    serde_json::json!({"record_frn":fragment.record.0,"owner_frn":fragment.owner.0,"is_base":fragment.is_base,
        "base_hardlink_count":fragment.base_link_count,"file_name_attributes":fragment.file_name_attributes,
        "dos_only_attributes":fragment.dos_only_attributes,"has_attribute_list":fragment.has_attribute_list,
        "nonresident_attribute_list":fragment.nonresident_attribute_list,
        "extension_record_frns":fragment.extension_records.iter().map(|frn|frn.0).collect::<Vec<_>>(),
        "names":fragment.names.iter().map(|n|serde_json::json!({"parent_frn":n.parent_frn.0,
            "name":String::from_utf8_lossy(&n.name)})).collect::<Vec<_>>()})
}
fn main() {
    if let Err(error) = run() { eprintln!("{error:#}"); std::process::exit(1); }
}
fn run() -> Result<()> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("metrics") => {
            let output = PathBuf::from(flag(&args, "--output", "resource-samples.jsonl"));
            let seconds: u64 = flag(&args, "--seconds", "10").parse().context("--seconds")?;
            let interval: u64 = flag(&args, "--interval-ms", "1000").parse().context("--interval-ms")?;
            let pid:u32=flag(&args,"--pid","0").parse().context("--pid")?;
            anyhow::ensure!((2..=3600).contains(&seconds), "--seconds must be 2..3600");
            let recorder = if pid==0 {metrics::Recorder::start(&output, Duration::from_millis(interval))?}
                else {metrics::Recorder::start_for_process(&output,Duration::from_millis(interval),pid)?};
            recorder.phase(if pid==0 {"machine_observation"} else {"external_process_observation"})?;
            std::thread::sleep(Duration::from_secs(seconds));
            let samples = recorder.finish()?;
            let last = samples.last().context("no metric samples collected")?;
            println!("{}", serde_json::to_string_pretty(&serde_json::json!({
                "schema": 1, "mode": "metrics-only-no-volume-access", "samples_file": output,
                "native_host_violations": policy::native_host_violations(last),
                "phases": metrics::summarize(&samples), "last": last,
            }))?);
        }
        Some("collect") => {
            let manifest = PathBuf::from(flag(&args,"--manifest","isolated-test.json"));
            let volume_identity = serde_json::from_slice::<native::Manifest>(&std::fs::read(&manifest)
                .with_context(|| format!("read manifest {}", manifest.display()))?)?.volume_guid;
            let output = PathBuf::from(flag(&args,"--snapshot","isolated-test.fti"));
            let log = PathBuf::from(flag(&args,"--metrics","native-samples.jsonl"));
            let resume=args.iter().any(|a|a=="--resume");
            let resume_initial=args.iter().any(|a|a=="--resume-initial");
            let test_stop_after_initial_checkpoint=args.iter().any(|a|a=="--test-stop-after-initial-checkpoint");
            let checkpoint_seconds:u64=flag(&args,"--checkpoint-seconds","30").parse().context("--checkpoint-seconds")?;
            let host_test=args.iter().any(|a|a=="--host-test-vhd");
            let watch_seconds:u64=flag(&args,"--watch-seconds","0").parse().context("--watch-seconds")?;
            let poll_ms:u64=flag(&args,"--poll-ms","1000").parse().context("--poll-ms")?;
            anyhow::ensure!(watch_seconds<=3600,"--watch-seconds must be 0..3600");
            anyhow::ensure!(!host_test || watch_seconds<=30,"local VHD watch windows are capped at 30 seconds per invocation");
            anyhow::ensure!((250..=10_000).contains(&poll_ms),"--poll-ms must be 250..10000");
            anyhow::ensure!(!resume || !resume_initial,"--resume and --resume-initial are mutually exclusive");
            anyhow::ensure!(!test_stop_after_initial_checkpoint || (host_test && !resume && !resume_initial),
                "--test-stop-after-initial-checkpoint is only valid for a fresh --host-test-vhd initial build");
            anyhow::ensure!((1..=300).contains(&checkpoint_seconds),"--checkpoint-seconds must be 1..300");
            let partial_output=output.with_extension("partial.fti");
            anyhow::ensure!(resume || resume_initial || !output.exists(),"snapshot exists; use --resume or a new output path");
            anyhow::ensure!(resume || resume_initial || !partial_output.exists(),"partial initial-build checkpoint exists; use --resume-initial or a new snapshot path");
            anyhow::ensure!(!resume_initial || partial_output.exists(),"no initial-build checkpoint exists to resume");
            let cancel = policy::Cancellation::default();
            let recorder = metrics::Recorder::start_guarded(&log,Duration::from_secs(1),Some((policy::Budget::default(),cancel.clone())))?;
            std::thread::sleep(Duration::from_secs(3));
            recorder.phase("isolated_test_preflight")?;
            let mut native_io_stats:Option<std::sync::Arc<std::sync::Mutex<native::NativeIoStats>>>=None;
            let result = (|| -> Result<serde_json::Value> {
                let mut sampler = metrics::Sampler::new()?;
                std::thread::sleep(Duration::from_millis(300));
                let sample = sampler.sample("preflight")?;
                let mode=if host_test {native::TestMode::AuthorizedHostVhd}else{native::TestMode::VirtualMachine};
                let _volume_owner=owner::VolumeOwner::acquire(&volume_identity)?;
                let (mut source,meta) = native::NativeSource::for_test(&manifest,&sample,cancel.clone(),mode)?;
                native_io_stats=Some(source.io_stats_handle());
                // Host-test mode is still confined to an explicitly authorized, disposable VHD (<=256 MiB).
                // Keep the index-memory cap and layout-page cap fixed while allowing representative scale fixtures.
                let limits=if host_test {collector::Limits {max_entries:200_000,max_index_bytes:64*1024*1024,max_layout_pages:128,total_seconds:180,..Default::default()}}else{collector::Limits::default()};
                let mut initial_dirty=!resume;
                let mut resumed_initial=false;
                let mut c=if resume {
                    recorder.phase("native_snapshot_restore")?;
                    let mut c=snapshot::load(&output,&cancel)?;
                    anyhow::ensure!(c.meta.identity.eq_ignore_ascii_case(&meta.identity) && c.meta.drive.eq_ignore_ascii_case(&meta.drive),
                        "saved snapshot belongs to another volume; automatic rebuild is disabled");
                    recorder.phase("native_resume_catch_up")?;
                    let old_cursor=c.meta.next_usn; let was_stale=c.meta.stale;
                    collector::catch_up(&mut c,&mut source,&limits,&cancel)?;
                    initial_dirty=c.meta.next_usn!=old_cursor || was_stale;
                    c
                } else {
                    recorder.phase("native_layout_and_catch_up")?;
                    let partial=if resume_initial {
                        recorder.phase("native_partial_snapshot_restore")?;
                        let (c,state)=snapshot::load_partial(&partial_output,&cancel)?;
                        anyhow::ensure!(c.meta.identity.eq_ignore_ascii_case(&meta.identity) && c.meta.drive.eq_ignore_ascii_case(&meta.drive),
                            "partial checkpoint belongs to another volume; automatic rebuild is disabled");
                        source.restore_checkpoint_state(state)?;resumed_initial=true;Some(c)
                    }else{None};
                    let mut last_partial_checkpoint=Instant::now();
                    collector::build_checkpointed(&mut source,meta,partial,&limits,&cancel,|provider,c|{
                        let Some(state)=provider.checkpoint_state()? else{return Ok(())};
                        let directory_link_walk=state.get("directory_link_walk").and_then(serde_json::Value::as_bool)==Some(true);
                        let current_directory=state.get("current_directory").and_then(serde_json::Value::as_u64);
                        if test_stop_after_initial_checkpoint && directory_link_walk && current_directory.is_none() {return Ok(())}
                        if !test_stop_after_initial_checkpoint && last_partial_checkpoint.elapsed()<Duration::from_secs(checkpoint_seconds){return Ok(())}
                        recorder.phase("native_initial_checkpoint")?;
                        snapshot::save_partial(c,&partial_output,&state,&cancel)?;
                        last_partial_checkpoint=Instant::now();
                        if test_stop_after_initial_checkpoint && (!directory_link_walk || current_directory.is_some()) {
                            anyhow::bail!("intentional host-VHD stop after durable initial checkpoint")
                        }
                        Ok(())
                    })?
                };
                if initial_dirty { recorder.phase("native_snapshot_publish")?; snapshot::save(&c,&output,&cancel)?; }
                let _=std::fs::remove_file(&partial_output);
                let start=Instant::now(); let mut last_checkpoint=Instant::now(); let mut polls=0u64; let mut changed_polls=0u64;
                let mut dirty=false;
                while start.elapsed()<Duration::from_secs(watch_seconds) {
                    recorder.phase("native_idle_wait")?;
                    let pause=Duration::from_millis(poll_ms).min(Duration::from_secs(watch_seconds).saturating_sub(start.elapsed()));
                    std::thread::sleep(pause); cancel.check()?;
                    recorder.phase("native_usn_incremental")?;
                    let old_cursor=c.meta.next_usn;
                    collector::catch_up(&mut c,&mut source,&limits,&cancel)?;
                    polls+=1; changed_polls+=u64::from(c.meta.next_usn!=old_cursor);
                    dirty |= c.meta.next_usn!=old_cursor;
                    // Persist at bounded intervals, not for every journal poll.
                    if last_checkpoint.elapsed()>=Duration::from_secs(30) && dirty {
                        recorder.phase("native_checkpoint")?; snapshot::save(&c,&output,&cancel)?;
                        last_checkpoint=Instant::now(); dirty=false;
                    }
                }
                if dirty { recorder.phase("native_final_checkpoint")?; snapshot::save(&c,&output,&cancel)?; }
                Ok(serde_json::json!({"entries":c.index.len(),"resumed":resume,"resumed_initial":resumed_initial,"polls":polls,"changed_polls":changed_polls,
                    "next_usn":c.meta.next_usn,"stale":c.meta.stale}))
            })();
            recorder.phase("native_cooldown")?;
            std::thread::sleep(Duration::from_secs(3));
            let samples = recorder.finish()?;
            let cancelled_by_guard_or_caller=cancel.is_cancelled();
            println!("{}",serde_json::to_string_pretty(&serde_json::json!({
                "schema":2,"mode":if host_test {"explicitly-authorized-host-vhd-test"}else{"isolated-test-only"},"snapshot":output,
                "source_fingerprint":env!("FLOWTARY_SOURCE_FINGERPRINT"),
                "native_volume_open_attempts":native::volume_open_attempts(),
                "native_io_stats":native_io_stats.as_ref().map(|stats| *stats.lock().unwrap()),
                "cancelled_by_guard_or_caller":cancelled_by_guard_or_caller,
                "result":encode_workload_result(&result),
                "phases":metrics::summarize(&samples),
            }))?);
            result?;
            anyhow::ensure!(!cancelled_by_guard_or_caller,"resource guard cancelled during workload/cooldown");
        }
        Some("probe-volume") => {
            let host_volume_probe=args.iter().any(|a|a=="--host-volume-probe");
            let host_vhd_probe=args.iter().any(|a|a=="--host-test-vhd");
            anyhow::ensure!(host_volume_probe ^ host_vhd_probe,
                "probe-volume requires exactly one of --host-volume-probe or --host-test-vhd");
            let pages:usize=flag(&args,"--pages","1").parse().context("--pages")?;
            let mechanism=flag(&args,"--mechanism","file-layout");
            anyhow::ensure!(mechanism=="file-layout" || mechanism=="enum-usn","unknown metadata probe mechanism");
            let probe_file_id=args.iter().any(|a|a=="--probe-file-id-layout");
            let probe_mft_record=args.iter().any(|a|a=="--probe-mft-record");
            let probe_mft_stream_zero=args.iter().any(|a|a=="--probe-mft-stream-zero");
            let probe_mft_by_file_id=args.iter().any(|a|a=="--probe-mft-by-file-id");
            let capture_record_payload=args.iter().any(|a|a=="--capture-file-record-payload-on-error");
            let probe_enum_duplicate_names=args.iter().any(|a|a=="--probe-enum-duplicate-names");
            let probe_directory_tree=args.iter().any(|a|a=="--probe-directory-tree");
            let tree_max_entries:usize=flag(&args,"--tree-max-entries","100000").parse().context("--tree-max-entries")?;
            let tree_max_directories:usize=flag(&args,"--tree-max-directories","10000").parse().context("--tree-max-directories")?;
            let tree_max_depth:u32=flag(&args,"--tree-max-depth","32").parse().context("--tree-max-depth")?;
            let tree_max_seconds:u64=flag(&args,"--tree-max-seconds","30").parse().context("--tree-max-seconds")?;
            let probe_hardlink_paths:Vec<String>=args.windows(2).filter(|pair|pair[0]=="--probe-hardlink-path")
                .map(|pair|pair[1].clone()).collect();
            let probe_directory_batch_paths:Vec<String>=args.windows(2).filter(|pair|pair[0]=="--probe-directory-batch")
                .map(|pair|pair[1].clone()).collect();
            let mft_batch:usize=flag(&args,"--probe-mft-batch","0").parse().context("--probe-mft-batch")?;
            let probe_frns:Vec<u64>=flag(&args,"--probe-frns","").split(',').filter(|s|!s.trim().is_empty())
                .map(|s|s.trim().parse().context("--probe-frns must be comma-separated 64-bit FRNs"))
                .collect::<Result<Vec<_>>>()?;
            let start_frn:u64=flag(&args,"--start-frn","0").parse().context("--start-frn")?;
            anyhow::ensure!((1..=4).contains(&pages),"physical-volume probes permit only 1..4 metadata pages");
            anyhow::ensure!(mft_batch<=128,"--probe-mft-batch is capped at 128 records");
            anyhow::ensure!(probe_frns.len()<=4,"--probe-frns is capped at four explicit file references");
            anyhow::ensure!(!capture_record_payload || (host_vhd_probe && probe_frns.len()==1),
                "raw FILE record payload diagnostics require exactly one explicit FRN on --host-test-vhd");
            anyhow::ensure!(!probe_enum_duplicate_names || (host_vhd_probe && mechanism=="enum-usn" && pages==1),
                "duplicate USN name diagnostics require exactly one page on --host-test-vhd");
            anyhow::ensure!(mft_batch==0 || mechanism=="enum-usn","bounded MFT batch requires --mechanism enum-usn");
            anyhow::ensure!(probe_hardlink_paths.len()<=256,"hard-link probe is capped at 256 explicit paths");
            anyhow::ensure!(probe_directory_batch_paths.len()<=4,"directory batch probe is capped at four explicit directories");
            anyhow::ensure!(probe_directory_batch_paths.is_empty() || host_vhd_probe,
                "directory batch probes are restricted to the authorized test VHD");
            anyhow::ensure!(!probe_directory_tree || host_vhd_probe,
                "directory-tree probes are restricted to the authorized test VHD");
            anyhow::ensure!((1..=100_000).contains(&tree_max_entries) && (1..=10_000).contains(&tree_max_directories)
                && (1..=32).contains(&tree_max_depth) && (1..=30).contains(&tree_max_seconds),
                "directory-tree probe limits exceed the enforced safety caps");
            for path in &probe_hardlink_paths {
                anyhow::ensure!(path.len()<=4096 && !path.starts_with(['\\','/']) && !path.contains([':', '*', '?']),
                    "hard-link probe requires relative literal file paths");
                anyhow::ensure!(path.split(['\\','/']).all(|part|!part.is_empty() && part!="." && part!=".."),
                    "hard-link probe path contains an invalid component");
            }
            for path in &probe_directory_batch_paths {
                anyhow::ensure!(path.len()<=4096 && !path.starts_with(['\\','/']) && !path.contains([':', '*', '?']),
                    "directory batch probe requires a relative literal path");
                anyhow::ensure!(path.split(['\\','/']).all(|part|!part.is_empty() && part!="." && part!=".."),
                    "directory batch probe path contains an invalid component");
            }
            let manifest=PathBuf::from(flag(&args,"--manifest","host-probe.json"));
            let volume_identity=serde_json::from_slice::<native::Manifest>(&std::fs::read(&manifest)
                .with_context(||format!("read manifest {}",manifest.display()))?)?.volume_guid;
            let log=PathBuf::from(flag(&args,"--metrics","host-probe-samples.jsonl"));
            let cancel=policy::Cancellation::default();
            let sample_interval=if probe_hardlink_paths.len()>1 || probe_directory_batch_paths.len()>1 || probe_directory_tree {Duration::from_millis(250)}else{Duration::from_secs(1)};
            let recorder=metrics::Recorder::start_guarded(&log,sample_interval,Some((policy::Budget::default(),cancel.clone())))?;
            std::thread::sleep(Duration::from_secs(3));recorder.phase("bounded_host_probe_preflight")?;
            let mut native_io_stats:Option<std::sync::Arc<std::sync::Mutex<native::NativeIoStats>>>=None;
            let result=(||->Result<serde_json::Value>{
                let mut sampler=metrics::Sampler::new()?;std::thread::sleep(Duration::from_millis(300));
                let sample=sampler.sample("preflight")?;
                let mode=if host_vhd_probe {native::TestMode::AuthorizedHostVhd}else{native::TestMode::AuthorizedHostVolumeProbe};
                let _volume_owner=owner::VolumeOwner::acquire(&volume_identity)?;
                let (mut source,meta)=native::NativeSource::for_test(&manifest,&sample,cancel.clone(),mode)?;
                if capture_record_payload {source.enable_file_record_payload_diagnostic()?;}
                native_io_stats=Some(source.io_stats_handle());
                let journal=source.journal()?;
                let mut observed=0usize;let mut pages_returned=0usize;let mut terminal=false;let mut next_frn=start_frn;let mut sample_file_id=None;
                let mut batch_candidates=Vec::new();
                let mut enum_file_id_samples=Vec::new();
                let mut duplicate_name_samples=Vec::new();
                let mut duplicate_file_count=0usize;
                for page in 0..pages {
                    cancel.check()?;recorder.phase("bounded_physical_metadata_page")?;
                    if mechanism=="file-layout" {
                        match source.layout_page(page==0)?{
                            Some(records)=>{observed+=records.len();pages_returned+=1;}
                            None=>{terminal=true;break;}
                        }
                    } else {
                        match source.enum_names_page(next_frn)?{
                            Some((next,records))=>{
                                if !probe_directory_batch_paths.is_empty() {
                                    for record in &records {
                                        let name=String::from_utf8_lossy(&record.name);
                                        if ["workshop","Flowtary-1.1.2","Flowtary-1.1.10","hardlink-bench","flowtary.exe","alias-flowtary.exe"].contains(&name.as_ref()) {
                                            enum_file_id_samples.push(serde_json::json!({"frn":record.frn.0,"parent_frn":record.parent_frn.0,
                                                "name":name,"attributes":record.attributes}));
                                        }
                                    }
                                }
                                if probe_enum_duplicate_names {
                                    let mut names:HashMap<goz_core::types::Frn,Vec<String>>=HashMap::new();
                                    for record in &records {
                                        let list=names.entry(record.frn).or_default();
                                        if list.len()<16 {list.push(String::from_utf8_lossy(&record.name).into_owned());}
                                    }
                                    let mut repeated:Vec<_>=names.into_iter().filter(|(_,names)|names.len()>1).collect();
                                    repeated.sort_unstable_by_key(|(frn,_)|frn.0);
                                    duplicate_file_count+=repeated.len();
                                    for (frn,names) in repeated.into_iter().take(16usize.saturating_sub(duplicate_name_samples.len())) {
                                        duplicate_name_samples.push(serde_json::json!({"frn":frn.0,"names":names}));
                                    }
                                }
                                if sample_file_id.is_none(){sample_file_id=records.iter().find(|r|r.frn.record_number()>=64 && r.attributes&0x10==0).map(|r|r.frn);}
                                if pages_returned==0 && mft_batch>0 {
                                    batch_candidates.extend(records.iter().map(|r|r.frn).filter(|f|f.record_number()>=64));
                                }
                                next_frn=next;observed+=records.len();pages_returned+=1;
                            }
                            None=>{terminal=true;break;}
                        }
                    }
                    recorder.phase("bounded_probe_pause")?;std::thread::sleep(Duration::from_millis(250));
                }
                let file_id_result=if probe_file_id {
                    if let Some(frn)=sample_file_id {
                        recorder.phase("bounded_file_id_layout_probe")?;
                        Some(source.file_layout(frn).map(|file|file.names.len()).map_err(|e|format!("{e:#}")))
                    }else{Some(Err("no file ID sample was found".into()))}
                }else{None};
                let mft_fragment_result=if probe_mft_record {
                    if let Some(frn)=sample_file_id {
                        recorder.phase("bounded_mft_record_probe")?;
                        Some(source.mft_name_fragment(frn).map(|fragment|fragment.map(|f|serde_json::json!({
                            "record_bytes_maximum":65536,"is_base":f.is_base,"is_directory":f.directory,
                            "record_frn":f.record.0,"owner_frn":f.owner.0,"base_hardlink_count":f.base_link_count,
                            "non_dos_file_names_in_this_record":f.names.len(),"extension_records_merged":false
                            ,"kernel_restored_fixups":f.kernel_restored_fixups,"file_name_attributes":f.file_name_attributes,
                            "dos_only_attributes":f.dos_only_attributes,"has_attribute_list":f.has_attribute_list,
                            "nonresident_attribute_list":f.nonresident_attribute_list,"extension_record_frns":f.extension_records.iter().map(|frn|frn.0).collect::<Vec<_>>()
                        }))).map_err(|e|format!("{e:#}")))
                    }else{Some(Err("no file ID sample was found".into()))}
                }else{None};
                let mft_batch_result=if mft_batch>0 {
                    recorder.phase("bounded_mft_fragment_batch")?;
                    let start=Instant::now();let mut succeeded=0usize;let mut empty=0usize;let mut names=0usize;
                    let mut base_records=0usize;let mut extension_records=0usize;let mut hardlink_headers=0usize;
                    let mut link_attribute_matches=0usize;let mut link_attribute_mismatches=0usize;
                    let mut total_file_name_attributes=0usize;let mut total_dos_attributes=0usize;let mut attribute_lists=0usize;
                    let mut nonresident_attribute_lists=0usize;let mut referenced_extension_records=0usize;
                    let mut mismatch_examples=Vec::new();
                    let mut failure=None;
                    let batch_ids:Vec<_>=if batch_candidates.len()<=mft_batch {batch_candidates.clone()} else {
                        (0..mft_batch).map(|i|batch_candidates[i*batch_candidates.len()/mft_batch]).collect()
                    };
                    for frn in &batch_ids {
                        cancel.check()?;
                        match source.mft_name_fragment(*frn) {
                            Ok(Some(fragment))=>{succeeded+=1;names+=fragment.names.len();
                                total_file_name_attributes+=fragment.file_name_attributes;total_dos_attributes+=fragment.dos_only_attributes;
                                attribute_lists+=usize::from(fragment.has_attribute_list);
                                nonresident_attribute_lists+=usize::from(fragment.nonresident_attribute_list);
                                referenced_extension_records+=fragment.extension_records.len();
                                if fragment.is_base {base_records+=1;
                                    if fragment.base_link_count==Some(fragment.file_name_attributes as u16){link_attribute_matches+=1;}
                                    else {link_attribute_mismatches+=1;if mismatch_examples.len()<8 {mismatch_examples.push(serde_json::json!({
                                        "frn":fragment.record.0,"base_link_count":fragment.base_link_count,
                                        "file_name_attributes":fragment.file_name_attributes,"dos_only_attributes":fragment.dos_only_attributes,
                                        "has_attribute_list":fragment.has_attribute_list,"nonresident_attribute_list":fragment.nonresident_attribute_list,
                                        "extension_record_frns":fragment.extension_records.iter().map(|frn|frn.0).collect::<Vec<_>>()}));}}
                                } else {extension_records+=1;}
                                hardlink_headers+=usize::from(fragment.base_link_count.unwrap_or(0)>1);
                            }
                            Ok(None)=>empty+=1,
                            Err(error)=>{failure=Some(format!("{error:#}"));break;}
                        }
                    }
                    let elapsed_ms=start.elapsed().as_secs_f64()*1000.0;
                    std::thread::sleep(Duration::from_secs(2));cancel.check()?;
                    Some(serde_json::json!({"requested_records":mft_batch,"attempted_records":succeeded+empty+usize::from(failure.is_some()),
                        "decoded_records":succeeded,"empty_records":empty,"non_dos_names":names,"base_records":base_records,
                        "extension_records":extension_records,"records_with_multiple_link_header":hardlink_headers,
                        "base_headers_matching_local_FILE_NAME_attribute_count":link_attribute_matches,
                        "base_headers_mismatching_local_FILE_NAME_attribute_count":link_attribute_mismatches,
                        "total_FILE_NAME_attributes":total_file_name_attributes,"total_DOS_only_attributes":total_dos_attributes,
                        "records_with_attribute_list":attribute_lists,"records_with_nonresident_attribute_list":nonresident_attribute_lists,
                        "referenced_FILE_NAME_extension_records":referenced_extension_records,"link_mismatch_examples":mismatch_examples,
                        "elapsed_ms":elapsed_ms,"records_per_second":(succeeded+empty) as f64/(elapsed_ms/1000.0).max(0.000001),
                        "failure":failure,"complete_hardlink_sets_proven":false}))
                }else{None};
                let mft_stream_zero_result=if probe_mft_stream_zero {
                    recorder.phase("bounded_mft_stream_record_zero_probe")?;
                    Some(source.probe_mft_stream_record_zero().map(|f|serde_json::json!({
                        "record_frn":f.record.0,"record_bytes_maximum":65536,"is_base":f.is_base,
                        "is_directory":f.directory,"base_hardlink_count":f.base_link_count,
                        "non_dos_file_names":f.names.len(),"file_name_attributes":f.file_name_attributes,
                        "dos_only_attributes":f.dos_only_attributes,"has_attribute_list":f.has_attribute_list,
                        "fixup_representation":"raw_disk_strict"
                    })).map_err(|e|format!("{e:#}")))
                }else{None};
                let mft_by_file_id_result=if probe_mft_by_file_id {
                    recorder.phase("bounded_mft_file_id_open")?;
                    Some(source.probe_mft_stream_by_file_id().map_err(|e|format!("{e:#}")))
                }else{None};
                let hardlink_name_result=if !probe_hardlink_paths.is_empty() {
                    recorder.phase("bounded_hardlink_name_enumeration")?;
                    let mut results=Vec::with_capacity(probe_hardlink_paths.len());
                    for path in &probe_hardlink_paths {
                        cancel.check()?;
                        let full_path=format!("{}:\\{}",meta.drive,path.replace('/',"\\"));
                        results.push(source.probe_hardlink_names(&full_path)?);
                    }
                    Some(serde_json::Value::Array(results))
                }else{None};
                let directory_batch_result=if !probe_directory_batch_paths.is_empty() {
                    recorder.phase("bounded_directory_batch_enumeration")?;
                    let mut results=Vec::with_capacity(probe_directory_batch_paths.len());
                    let mut remaining_entries=100_000usize;
                    for path in &probe_directory_batch_paths {
                        cancel.check()?;
                        let result=source.probe_directory_batch(path,remaining_entries)?;
                        let used=result.get("entries").and_then(serde_json::Value::as_u64)
                            .context("directory batch probe omitted its entry count")? as usize;
                        remaining_entries=remaining_entries.saturating_sub(used);
                        results.push(result);
                    }
                    Some(results)
                }else{None};
                let directory_tree_result=if probe_directory_tree {
                    recorder.phase("bounded_directory_tree_walk")?;
                    Some(source.probe_directory_tree(tree_max_entries,tree_max_directories,tree_max_depth,tree_max_seconds)?)
                }else{None};
                let targeted_mft_result=if !probe_frns.is_empty() {
                    recorder.phase("bounded_targeted_mft_attribute_list_probe")?;
                    let mut results=Vec::new();let mut fetched=std::collections::HashSet::new();
                    for raw_frn in &probe_frns {
                        cancel.check()?;
                        let frn=goz_core::types::Frn(*raw_frn);
                        let Some(fragment)=source.mft_name_fragment(frn)? else {
                            results.push(serde_json::json!({"requested_frn":raw_frn,"allocated":false}));continue;
                        };
                        let mut record=json_fragment(&fragment);
                        let mut extensions=Vec::new();
                        for extension_frn in fragment.extension_records.iter().take(8).copied() {
                            if !fetched.insert(extension_frn.0) {continue;}
                            cancel.check()?;
                            match source.mft_name_fragment(extension_frn)? {
                                Some(extension)=>extensions.push(json_fragment(&extension)),
                                None=>extensions.push(serde_json::json!({"record_frn":extension_frn.0,"allocated":false})),
                            }
                        }
                        record["extensions"]=serde_json::Value::Array(extensions);
                        results.push(record);
                    }
                    Some(results)
                }else{None};
                Ok(serde_json::json!({"drive":meta.drive,"mechanism":mechanism,"journal_id":journal.journal_id,"requested_pages":pages,
                    "pages_returned":pages_returned,"file_records_observed":observed,"reached_volume_end":terminal,
                    "per_page_output_buffer_bytes":256*1024,"complete_index_built":false,
                    "directory_enumerations":probe_directory_batch_paths.len(),
                    "file_id_layout_probe":file_id_result,"mft_record_probe":mft_fragment_result,"mft_batch_probe":mft_batch_result,"next_frn_cursor":next_frn,
                    "enum_duplicate_file_count":duplicate_file_count,"enum_duplicate_name_samples":duplicate_name_samples,
                    "enum_file_id_samples":enum_file_id_samples,
                    "mft_stream_record_zero_probe":mft_stream_zero_result,"mft_by_file_id_probe":mft_by_file_id_result,
                    "hardlink_name_probe":hardlink_name_result,
                    "directory_batch_probe":directory_batch_result,
                    "directory_tree_probe":directory_tree_result,
                    "targeted_mft_attribute_list_probe":targeted_mft_result,
                    "all_hardlink_names_proven":false}))
            })();
            recorder.phase("bounded_probe_cooldown")?;std::thread::sleep(Duration::from_secs(10));
            let samples=recorder.finish()?;
            let cancelled_by_guard_or_caller=cancel.is_cancelled();
            println!("{}",serde_json::to_string_pretty(&serde_json::json!({"schema":1,"mode":if host_vhd_probe {"authorized-bounded-host-vhd-probe"}else{"authorized-bounded-physical-volume-probe"},
                "source_fingerprint":env!("FLOWTARY_SOURCE_FINGERPRINT"),"native_volume_open_attempts":native::volume_open_attempts(),
                "native_io_stats":native_io_stats.as_ref().map(|stats|*stats.lock().unwrap()),
                "cancelled_by_guard_or_caller":cancelled_by_guard_or_caller,"result":encode_workload_result(&result),"phases":metrics::summarize(&samples)}))?);
            result?;
            anyhow::ensure!(!cancelled_by_guard_or_caller,"resource guard cancelled during probe/cooldown");
        }
        Some("bench-offline") => {
            let output = PathBuf::from(flag(&args,"--output-dir","offline-benchmark"));
            let count: usize = flag(&args,"--entries","100000").parse().context("--entries")?;
            let shape=flag(&args,"--shape","shallow");
            bench::run(&output,count,&shape)?;
        }
        Some("query") => {
            let file = PathBuf::from(flag(&args,"--snapshot","index.fti"));
            let cancel = policy::Cancellation::default();
            let catalog = snapshot::load(&file,&cancel)?;
            let q: catalog::Query = serde_json::from_str(&flag(&args,"--json","{}"))?;
            println!("{}",serde_json::to_string(&catalog.query(&q,&cancel)?)?);
        }
        Some("serve-snapshot") => {
            let file=PathBuf::from(flag(&args,"--snapshot","index.fti"));
            let cancel=policy::Cancellation::default();
            let telemetry_dir=file.parent().context("snapshot path has no parent")?.join("Logs");
            let (catalog,generation)=reloader::load_snapshot(&file,&telemetry_dir,&cancel,"snapshot-initial-load")?;
            let catalog=std::sync::Arc::new(std::sync::RwLock::new(catalog));
            let _reloader=reloader::SnapshotReloader::start(file,telemetry_dir,generation,catalog.clone(),&cancel)?;
            // Only machine-readable responses go to stdout; readiness goes to stderr.
            eprintln!("snapshot-ready: {} entries; background generation reload enabled",catalog.read().unwrap_or_else(|p|p.into_inner()).index.len());
            session::serve_shared(&catalog,std::io::stdin().lock(),std::io::stdout().lock(),&cancel,||Ok(()))?;
        }
        Some("service-install") => {
            let snapshot=PathBuf::from(flag(&args,"--snapshot",""));
            let owner_sid=flag(&args,"--owner-sid","");
            anyhow::ensure!(!snapshot.as_os_str().is_empty(),"service-install requires --snapshot");
            anyhow::ensure!(!owner_sid.is_empty(),"service-install requires --owner-sid");
            service::install(&snapshot,&owner_sid)?;
        }
        Some("service-run") => service::run_from_args(&args[1..])?,
        Some("service-uninstall") => service::uninstall()?,
        _ => println!("flowtary-indexer metrics --seconds 10 --output samples.jsonl\nflowtary-indexer bench-offline --entries 100000 --output-dir benchmark\nflowtary-indexer query --snapshot index.fti --json QUERY_JSON\nflowtary-indexer collect --manifest isolated-test.json --snapshot test.fti --metrics samples.jsonl [--resume | --resume-initial] [--checkpoint-seconds 30]\n  --resume: catch up an existing complete snapshot from its USN cursor\n  --resume-initial: continue an interrupted USN initial build from test.fti.partial.fti\n  --checkpoint-seconds: persist an initial-build checkpoint every 1..300 seconds (default 30)\nflowtary-indexer probe-volume (--host-volume-probe | --host-test-vhd) --manifest manifest.json --mechanism enum-usn --pages 1 [--probe-mft-by-file-id] [--probe-hardlink-path RELATIVE_FILE] [--probe-directory-batch RELATIVE_DIR]\n  --probe-directory-batch: test-VHD-only batched name/FileId enumeration, at most four directories and 100,000 aggregate entries\n  --capture-file-record-payload-on-error: one explicit FRN, authorized VHD only, <=4096 payload bytes\nNative collection is restricted to healthy rollback-capable test VMs and FLOWTARY_TEST-labelled volumes."),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn guard_cancellation_does_not_replace_a_completed_workload_result() {
        let result=Ok(serde_json::json!({"entries":10_000,"complete":true}));
        let cancelled_by_guard_or_caller=true;
        let encoded=encode_workload_result(&result);
        assert!(cancelled_by_guard_or_caller);
        assert_eq!(encoded,serde_json::json!({"Ok":{"entries":10_000,"complete":true}}));
    }
    #[test]
    fn workload_errors_remain_visible_in_cancelled_run_reports() {
        let result:Result<serde_json::Value>=Err(anyhow::anyhow!("resource guard stopped the probe"));
        assert_eq!(encode_workload_result(&result),serde_json::json!({"Err":"resource guard stopped the probe"}));
    }
}

