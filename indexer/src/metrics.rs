//! Machine-wide counters. No object-table walking, directory enumeration or working-set trimming.
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs::File, io::{BufWriter, Write}, path::Path,
    sync::mpsc, thread, time::{Duration, Instant, SystemTime, UNIX_EPOCH}};

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Sample {
    pub unix_ms: u64,
    pub elapsed_s: f64,
    pub phase: String,
    pub monitored_pid: u32,
    pub commit_bytes: u64,
    pub commit_limit_bytes: u64,
    pub commit_ratio: f64,
    pub physical_total_bytes: u64,
    pub physical_available_bytes: u64,
    /// Physical-memory pressure proxy: `1 - available / total`, clamped to
    /// [0, 1]. This is not a Windows pressure score or commit ratio.
    pub memory_pressure_ratio: f64,
    pub pages_input_per_s: Option<f64>,
    pub system_handles: Option<u64>,
    pub disk_read_bytes_per_s: Option<f64>,
    pub disk_write_bytes_per_s: Option<f64>,
    pub disk_busy_percent: Option<f64>,
    pub disk_queue_length: Option<f64>,
    pub pool_paged_bytes: u64,
    pub pool_nonpaged_bytes: u64,
    pub cpu_percent: Option<f64>,
    /// Process CPU as a fraction of aggregate machine capacity across all logical processors.
    pub process_cpu_percent: Option<f64>,
    pub process_private_bytes: u64,
    pub process_working_set_bytes: u64,
    pub process_handles: u32,
    /// Counter failures are recorded, never substituted with zero.
    pub counter_errors: BTreeMap<String, u32>,
    pub guard_violations: Vec<String>,
}
fn physical_memory_pressure_ratio(total_bytes:u64,available_bytes:u64)->f64 {
    (1.0-available_bytes as f64/total_bytes.max(1) as f64).clamp(0.0,1.0)
}
const RAPID_SYSTEM_HANDLE_INTERVALS:u32=2;
fn advance_rapid_handle_streak(streak:&mut u32,violated:bool)->bool {
    if violated { *streak=streak.saturating_add(1); } else { *streak=0; }
    *streak>=RAPID_SYSTEM_HANDLE_INTERVALS
}

#[cfg(windows)]
mod native {
    use super::*;
    use std::{mem::{size_of, zeroed}, ptr::{null, null_mut}};
    use windows_sys::Win32::{Foundation::{FILETIME,GetLastError}, System::{
        Performance::{PdhAddEnglishCounterW, PdhCloseQuery, PdhCollectQueryData,
            PdhGetFormattedCounterValue, PdhOpenQueryW, PDH_FMT_COUNTERVALUE, PDH_FMT_DOUBLE},
        ProcessStatus::{GetPerformanceInfo, GetProcessMemoryInfo, PERFORMANCE_INFORMATION, PROCESS_MEMORY_COUNTERS_EX},
        Threading::{GetCurrentProcess, GetCurrentProcessId, GetProcessHandleCount, GetSystemTimes, GetProcessTimes,
            OpenProcess,PROCESS_QUERY_LIMITED_INFORMATION,PROCESS_VM_READ},
    }};

    pub struct Sampler {
        query: *mut core::ffi::c_void,
        counters: Vec<(&'static str, *mut core::ffi::c_void)>,
        add_errors: BTreeMap<String, u32>,
        last_times: Option<(u64, u64)>,
        last_process_ticks: Option<u64>,
        start: Instant,
        process: *mut core::ffi::c_void,
        owned_process:bool,
        monitored_pid:u32,
    }
    // SAFETY: the PDH query has one owner and is accessed only by its sampling thread.
    unsafe impl Send for Sampler {}
    impl Sampler {
        pub fn new() -> Result<Self> {
            Self::for_process(None)
        }
        pub fn for_process(pid:Option<u32>) -> Result<Self> {
            let mut query = null_mut();
            // SAFETY: query is a valid writable output pointer; null data source means local machine.
            let status = unsafe { PdhOpenQueryW(null(), 0, &mut query) };
            if status != 0 { bail!("PdhOpenQueryW failed: {status:#x}"); }
            let mut this = Self { query, counters: Vec::new(), add_errors: BTreeMap::new(), last_times: None, last_process_ticks:None, start: Instant::now(),
                process:unsafe {GetCurrentProcess()},owned_process:false,monitored_pid:unsafe {GetCurrentProcessId()} };
            if let Some(pid)=pid {
                // SAFETY: read-only metric rights for an explicitly requested process ID.
                let process=unsafe {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,0,pid)};
                if process.is_null(){bail!("open monitored process {pid} failed: {}",std::io::Error::last_os_error());}
                this.process=process;this.owned_process=true;this.monitored_pid=pid;
            }
            for (key, path) in [
                ("system_handles", "\\Process(System)\\Handle Count"),
                ("pages_input_per_s", "\\Memory\\Pages Input/sec"),
                ("disk_read_bytes_per_s", "\\PhysicalDisk(_Total)\\Disk Read Bytes/sec"),
                ("disk_write_bytes_per_s", "\\PhysicalDisk(_Total)\\Disk Write Bytes/sec"),
                ("disk_idle_percent", "\\PhysicalDisk(_Total)\\% Idle Time"),
                ("disk_queue_length", "\\PhysicalDisk(_Total)\\Current Disk Queue Length"),
            ] {
                let wide: Vec<u16> = path.encode_utf16().chain(Some(0)).collect();
                let mut counter = null_mut();
                // SAFETY: wide is null terminated and both query and counter output are valid.
                let status = unsafe { PdhAddEnglishCounterW(query, wide.as_ptr(), 0, &mut counter) };
                if status == 0 { this.counters.push((key, counter)); }
                else { this.add_errors.insert(key.into(), status); }
            }
            // Seed rate counters. The first sample still reports unavailable rates honestly.
            unsafe { PdhCollectQueryData(query); }
            Ok(this)
        }
        pub fn sample(&mut self, phase: &str) -> Result<Sample> {
            // SAFETY: the structures are zeroed then initialized with the SDK-required sizes.
            let mut perf: PERFORMANCE_INFORMATION = unsafe { zeroed() };
            perf.cb = size_of::<PERFORMANCE_INFORMATION>() as u32;
            if unsafe { GetPerformanceInfo(&mut perf, perf.cb) } == 0 { bail!("GetPerformanceInfo failed"); }
            let mut proc: PROCESS_MEMORY_COUNTERS_EX = unsafe { zeroed() };
            proc.cb = size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32;
            if unsafe { GetProcessMemoryInfo(self.process, &mut proc as *mut _ as _, proc.cb) } == 0 {
                bail!("GetProcessMemoryInfo failed");
            }
            let mut handles = 0;
            if unsafe { GetProcessHandleCount(self.process, &mut handles) } == 0 { bail!("GetProcessHandleCount failed"); }
            let mut idle: FILETIME = unsafe { zeroed() };
            let mut kernel: FILETIME = unsafe { zeroed() };
            let mut user: FILETIME = unsafe { zeroed() };
            let mut cpu = None;
            let mut process_cpu = None;
            let mut errors = self.add_errors.clone();
            if unsafe { GetSystemTimes(&mut idle, &mut kernel, &mut user) } != 0 {
                let ticks = |f: FILETIME| ((f.dwHighDateTime as u64) << 32) | f.dwLowDateTime as u64;
                let total = ticks(kernel).saturating_add(ticks(user));
                let idle = ticks(idle);
                let mut creation: FILETIME = unsafe { zeroed() }; let mut exit: FILETIME = unsafe { zeroed() };
                let mut pk: FILETIME = unsafe { zeroed() }; let mut pu: FILETIME = unsafe { zeroed() };
                let process_ticks = if unsafe { GetProcessTimes(self.process,&mut creation,&mut exit,&mut pk,&mut pu) } != 0 {
                    Some(ticks(pk).saturating_add(ticks(pu)))
                } else { errors.insert("process_cpu_percent".into(),unsafe {GetLastError()}); None };
                if let Some((old_total, old_idle)) = self.last_times {
                    let dt = total.saturating_sub(old_total);
                    if dt > 0 { cpu = Some((100.0 * (dt.saturating_sub(idle.saturating_sub(old_idle))) as f64 / dt as f64).clamp(0.0, 100.0)); }
                    if let Some((old,new)) = self.last_process_ticks.zip(process_ticks) {
                        if dt > 0 { process_cpu = Some((100.0 * new.saturating_sub(old) as f64 / dt as f64).clamp(0.0,100.0)); }
                    }
                }
                self.last_times = Some((total, idle));
                self.last_process_ticks = process_ticks;
            } else { errors.insert("cpu_percent".into(),unsafe {GetLastError()}); }
            let mut values = BTreeMap::new();
            let collected = unsafe { PdhCollectQueryData(self.query) };
            if collected != 0 { errors.insert("collect".into(), collected); }
            for (key, counter) in &self.counters {
                if collected != 0 { continue; } // Never expose stale cached rates after a failed collection.
                let mut value: PDH_FMT_COUNTERVALUE = unsafe { zeroed() };
                let status = unsafe { PdhGetFormattedCounterValue(*counter, PDH_FMT_DOUBLE, null_mut(), &mut value) };
                if status != 0 { errors.insert((*key).into(), status); continue; }
                if value.CStatus > 1 { errors.insert((*key).into(), value.CStatus); continue; }
                let number = unsafe { value.Anonymous.doubleValue };
                if number.is_finite() { values.insert(*key, number); }
                else { errors.insert((*key).into(),13); } // ERROR_INVALID_DATA
            }
            let bytes = |pages: usize| (pages as u64).saturating_mul(perf.PageSize as u64);
            let get = |name| values.get(name).copied();
            Ok(Sample {
                unix_ms: SystemTime::now().duration_since(UNIX_EPOCH)?.as_millis() as u64,
                elapsed_s: self.start.elapsed().as_secs_f64(), phase: phase.into(),
                monitored_pid:self.monitored_pid,
                commit_bytes: bytes(perf.CommitTotal), commit_limit_bytes: bytes(perf.CommitLimit),
                commit_ratio: perf.CommitTotal as f64 / perf.CommitLimit.max(1) as f64,
                physical_total_bytes: bytes(perf.PhysicalTotal), physical_available_bytes: bytes(perf.PhysicalAvailable),
                memory_pressure_ratio: physical_memory_pressure_ratio(bytes(perf.PhysicalTotal),bytes(perf.PhysicalAvailable)),
                pool_paged_bytes: bytes(perf.KernelPaged), pool_nonpaged_bytes: bytes(perf.KernelNonpaged),
                system_handles: get("system_handles").map(|n| n as u64),
                pages_input_per_s: get("pages_input_per_s"), disk_read_bytes_per_s: get("disk_read_bytes_per_s"),
                disk_write_bytes_per_s: get("disk_write_bytes_per_s"),
                disk_busy_percent: get("disk_idle_percent").map(|n| (100.0 - n).clamp(0.0, 100.0)),
                disk_queue_length: get("disk_queue_length"), cpu_percent: cpu,process_cpu_percent:process_cpu,
                process_private_bytes: proc.PrivateUsage as u64, process_working_set_bytes: proc.WorkingSetSize as u64,
                process_handles: handles, counter_errors: errors, guard_violations: vec![],
            })
        }
    }
    impl Drop for Sampler {
        fn drop(&mut self) { unsafe {
            PdhCloseQuery(self.query);
            if self.owned_process {windows_sys::Win32::Foundation::CloseHandle(self.process);}
        } }
    }
}
#[cfg(windows)]
pub use native::Sampler;

/// Recorder lives in the workload process so phases and cancellation are unambiguous.
enum Command { Phase(String, mpsc::SyncSender<()>), Stop }
pub struct Recorder {
    requests: mpsc::Sender<Command>,
    thread: Option<thread::JoinHandle<Result<Vec<Sample>>>>,
}
impl Recorder {
    pub fn start(path: &Path, interval: Duration) -> Result<Self> {
        Self::start_guarded(path, interval, None)
    }
    pub fn start_guarded(path: &Path, interval: Duration,
        guard: Option<(crate::policy::Budget, crate::policy::Cancellation)>) -> Result<Self> {
        Self::start_with_target(path,interval,guard,None)
    }
    pub fn start_for_process(path:&Path,interval:Duration,pid:u32)->Result<Self>{
        Self::start_with_target(path,interval,None,Some(pid))
    }
    fn start_with_target(path:&Path,interval:Duration,
        guard:Option<(crate::policy::Budget,crate::policy::Cancellation)>,pid:Option<u32>)->Result<Self>{
        if interval < Duration::from_millis(250) { bail!("sampling interval must be at least 250 ms"); }
        let sampler = Sampler::for_process(pid)?;
        let file = File::create(path).with_context(|| format!("create {}", path.display()))?;
        let (requests, rx) = mpsc::channel();
        let thread = thread::spawn(move || {
            let mut sampler = sampler;
            let mut writer = BufWriter::new(file);
            let mut samples = Vec::new();
            let mut baseline = None;
            let mut consecutive = 0u32;
            let mut rapid_handle_growth_consecutive=0u32;
            let mut last_guard_at:Option<f64> = None;
            let mut last_guard_sample:Option<Sample> = None;
            let mut phase = String::from("baseline");
            let mut capture = |phase: &str| -> Result<()> {
                let stop_on_error = |error: anyhow::Error| {
                    if let Some((_,cancel)) = &guard { cancel.cancel(); }
                    error
                };
                if samples.len() >= 20_000 {
                    if let Some((_,cancel)) = &guard { cancel.cancel(); }
                    bail!("telemetry sample budget exhausted");
                }
                let mut sample = sampler.sample(phase).map_err(stop_on_error)?;
                if let Some((budget, cancel)) = &guard {
                    let base = baseline.get_or_insert_with(|| sample.clone());
                    sample.guard_violations = crate::policy::growth_violations(budget, base, &sample);
                    // Boundary bursts do not constitute seconds of sustained growth.
                    let mut rapid_trip=false;
                    if last_guard_at.is_none_or(|t|sample.elapsed_s-t>=interval.as_secs_f64()*0.9) {
                        let rapid_handle_growth=if let Some(previous)=last_guard_sample.as_ref() {
                            if let Some(violation)=crate::policy::system_handle_rate_violation(budget,previous,&sample) {
                                sample.guard_violations.push(violation);
                                true
                            } else { false }
                        } else { false };
                        rapid_trip=advance_rapid_handle_streak(&mut rapid_handle_growth_consecutive,rapid_handle_growth);
                        if sample.guard_violations.is_empty() { consecutive = 0; } else { consecutive += 1; }
                        last_guard_at=Some(sample.elapsed_s);
                        last_guard_sample=Some(sample.clone());
                    }
                    if consecutive >= 3 || rapid_trip || !sample.counter_errors.is_empty() || sample.physical_available_bytes < budget.minimum_available_bytes || sample.commit_ratio > budget.maximum_commit_ratio || sample.memory_pressure_ratio > budget.maximum_memory_pressure_ratio {
                        cancel.cancel();
                    }
                }
                serde_json::to_writer(&mut writer, &sample).map_err(anyhow::Error::from).map_err(stop_on_error)?;
                writer.write_all(b"\n").map_err(anyhow::Error::from).map_err(stop_on_error)?;
                writer.flush().map_err(anyhow::Error::from).map_err(stop_on_error)?;
                samples.push(sample);
                Ok(())
            };
            capture(&phase)?;
            loop {
                match rx.recv_timeout(interval) {
                    Ok(Command::Phase(next, ack)) => {
                        // Both sides of every boundary are sampled, even for a sub-second operation.
                        capture(&phase)?;
                        phase = next;
                        capture(&phase)?;
                        let _ = ack.send(());
                    }
                    Ok(Command::Stop) | Err(mpsc::RecvTimeoutError::Disconnected) => { capture(&phase)?; break; }
                    Err(mpsc::RecvTimeoutError::Timeout) => capture(&phase)?,
                }
            }
            Ok(samples)
        });
        Ok(Self { requests, thread: Some(thread) })
    }
    pub fn phase(&self, name: &str) -> Result<()> {
        let (ack, rx) = mpsc::sync_channel(1);
        self.requests.send(Command::Phase(name.into(),ack)).context("metrics recorder stopped")?;
        rx.recv_timeout(Duration::from_secs(5)).context("metrics boundary capture did not complete")?;
        Ok(())
    }
    pub fn finish(mut self) -> Result<Vec<Sample>> {
        let _ = self.requests.send(Command::Stop);
        self.thread.take().unwrap().join().map_err(|_| anyhow::anyhow!("metrics thread panicked"))?
    }
}
impl Drop for Recorder {
    fn drop(&mut self) {
        let _ = self.requests.send(Command::Stop);
        if let Some(t) = self.thread.take() { let _ = t.join(); }
    }
}

#[derive(Debug, Serialize)]
pub struct PhaseSummary {
    pub phase: String,
    pub samples: usize,
    pub first_unix_ms: u64,
    pub last_unix_ms: u64,
    pub commit_delta_bytes: i64,
    pub minimum_available_bytes: u64,
    pub maximum_pressure_ratio: f64,
    pub system_handle_delta: Option<i64>,
    pub pool_paged_delta_bytes: i64,
    pub pool_nonpaged_delta_bytes: i64,
    pub mean_cpu_percent: Option<f64>,
    pub mean_process_cpu_percent: Option<f64>,
    pub max_disk_bytes_per_s: Option<f64>,
    pub max_process_private_bytes: u64,
    pub max_pages_input_per_s: Option<f64>,
    pub counter_error_samples: usize,
    pub duration_seconds: f64,
    pub cpu_coverage_seconds: f64,
    pub process_cpu_coverage_seconds: f64,
    pub disk_coverage_seconds: f64,
    pub mean_disk_bytes_per_s: Option<f64>,
    pub estimated_disk_bytes: Option<f64>,
    pub mean_disk_busy_percent: Option<f64>,
    pub mean_pages_input_per_s: Option<f64>,
    pub maximum_commit_bytes: u64,
    pub maximum_commit_ratio: f64,
    pub maximum_system_handles: Option<u64>,
    pub maximum_paged_pool_bytes: u64,
    pub maximum_nonpaged_pool_bytes: u64,
}
// A rate at sample i describes the preceding interval. The first sample in a
// phase includes time from the old phase and must not be charged to the new one.
// Missing counters reduce coverage rather than silently counting as zero.
fn weighted(ss: &[&Sample], value: impl Fn(&Sample) -> Option<f64>) -> (Option<f64>, f64) {
    let (mut integral, mut coverage) = (0.0, 0.0);
    for pair in ss.windows(2) {
        let dt = pair[1].elapsed_s - pair[0].elapsed_s;
        if let Some(v) = value(pair[1]).filter(|v| v.is_finite()) {
            if dt > 0.0 { integral += v * dt; coverage += dt; }
        }
    }
    ((coverage > 0.0).then(|| integral / coverage), coverage)
}
pub fn summarize(samples: &[Sample]) -> Vec<PhaseSummary> {
    // Preserve chronological episodes; merging two occurrences of a phase would
    // incorrectly integrate the intervening work as if it belonged to that phase.
    let mut phases: Vec<Vec<&Sample>> = Vec::new();
    for s in samples {
        if phases.last().is_none_or(|p| p[0].phase != s.phase) { phases.push(Vec::new()); }
        phases.last_mut().unwrap().push(s);
    }
    phases.into_iter().map(|ss| {
        let first = ss[0]; let last = ss[ss.len() - 1];
        let (cpu,cpu_coverage) = weighted(&ss, |s| s.cpu_percent);
        let (process_cpu,process_coverage) = weighted(&ss, |s| s.process_cpu_percent);
        let (disk,disk_coverage) = weighted(&ss, |s| s.disk_read_bytes_per_s.zip(s.disk_write_bytes_per_s).map(|(r,w)| r+w));
        PhaseSummary { phase:first.phase.clone(), samples: ss.len(), first_unix_ms: first.unix_ms, last_unix_ms: last.unix_ms,
            commit_delta_bytes: last.commit_bytes as i64 - first.commit_bytes as i64,
            minimum_available_bytes: ss.iter().map(|s| s.physical_available_bytes).min().unwrap(),
            maximum_pressure_ratio: ss.iter().map(|s| s.memory_pressure_ratio).fold(0.0, f64::max),
            system_handle_delta: first.system_handles.zip(last.system_handles).map(|(a,b)| b as i64 - a as i64),
            pool_paged_delta_bytes: last.pool_paged_bytes as i64 - first.pool_paged_bytes as i64,
            pool_nonpaged_delta_bytes: last.pool_nonpaged_bytes as i64 - first.pool_nonpaged_bytes as i64,
            mean_cpu_percent: cpu,
            mean_process_cpu_percent: process_cpu,
            max_disk_bytes_per_s: ss.iter().filter_map(|s| s.disk_read_bytes_per_s.zip(s.disk_write_bytes_per_s).map(|(r,w)| r+w)).reduce(f64::max),
            max_process_private_bytes: ss.iter().map(|s| s.process_private_bytes).max().unwrap(),
            max_pages_input_per_s: ss.iter().filter_map(|s| s.pages_input_per_s).reduce(f64::max),
            counter_error_samples: ss.iter().filter(|s| !s.counter_errors.is_empty()).count(),
            duration_seconds:(last.elapsed_s-first.elapsed_s).max(0.0),
            cpu_coverage_seconds:cpu_coverage, process_cpu_coverage_seconds:process_coverage,
            disk_coverage_seconds:disk_coverage, mean_disk_bytes_per_s:disk,
            estimated_disk_bytes:disk.map(|v| v*disk_coverage),
            mean_disk_busy_percent:weighted(&ss, |s| s.disk_busy_percent).0,
            mean_pages_input_per_s:weighted(&ss, |s| s.pages_input_per_s).0,
            maximum_commit_bytes:ss.iter().map(|s| s.commit_bytes).max().unwrap(),
            maximum_commit_ratio:ss.iter().map(|s| s.commit_ratio).fold(0.0,f64::max),
            maximum_system_handles:ss.iter().filter_map(|s| s.system_handles).max(),
            maximum_paged_pool_bytes:ss.iter().map(|s| s.pool_paged_bytes).max().unwrap(),
            maximum_nonpaged_pool_bytes:ss.iter().map(|s| s.pool_nonpaged_bytes).max().unwrap(),
        }
    }).collect()
}

#[cfg(test)] mod tests {
    use super::*;
    #[test] fn rapid_handle_growth_requires_two_consecutive_regular_intervals() {
        let mut streak=0;
        assert!(!advance_rapid_handle_streak(&mut streak,true));
        assert!(!advance_rapid_handle_streak(&mut streak,false));
        assert_eq!(streak,0);
        assert!(!advance_rapid_handle_streak(&mut streak,true));
        assert!(advance_rapid_handle_streak(&mut streak,true));
        assert!(advance_rapid_handle_streak(&mut streak,true));
        assert!(!advance_rapid_handle_streak(&mut streak,false));
        assert_eq!(streak,0);
    }
    #[test] fn physical_pressure_proxy_is_bounded_and_uses_available_memory() {
        assert_eq!(physical_memory_pressure_ratio(32,8),0.75);
        assert_eq!(physical_memory_pressure_ratio(32,40),0.0);
        assert_eq!(physical_memory_pressure_ratio(0,0),1.0);
    }
    #[test] fn summary_weights_intervals_excludes_incoming_boundary_and_reports_missing_coverage() {
        let make = |t,phase: &str,cpu,disk| Sample { elapsed_s:t,phase:phase.into(),cpu_percent:cpu,
            disk_read_bytes_per_s:disk,disk_write_bytes_per_s:disk.map(|_|0.0),..Default::default() };
        let ss = [make(0.0,"idle",Some(99.0),Some(999.0)),
            make(1.0,"work",Some(90.0),Some(900.0)),
            make(2.0,"work",Some(10.0),Some(100.0)),
            make(5.0,"work",Some(50.0),Some(200.0)),
            make(6.0,"work",None,None),make(7.0,"idle",Some(80.0),Some(800.0))];
        let out=summarize(&ss); assert_eq!(out.len(),3); assert_eq!(out[1].phase,"work");
        assert_eq!(out[1].mean_cpu_percent,Some(40.0));
        assert_eq!(out[1].cpu_coverage_seconds,4.0); assert_eq!(out[1].duration_seconds,5.0);
        assert_eq!(out[1].estimated_disk_bytes,Some(700.0));
        assert_eq!(out[2].mean_cpu_percent,None);
    }
}
