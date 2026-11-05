use crate::metrics::Sample;
use serde::{Deserialize, Serialize};
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};

const MIB: u64 = 1024 * 1024;
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Budget {
    pub minimum_available_bytes: u64,
    pub maximum_commit_ratio: f64,
    pub maximum_memory_pressure_ratio: f64,
    pub maximum_commit_growth_bytes: u64,
    pub maximum_system_handle_growth: u64,
    /// Sustained System-handle growth rate; the recorder cancels after two
    /// consecutive regular sample intervals above this threshold.
    pub maximum_system_handle_growth_per_second: f64,
    pub maximum_nonpaged_growth_bytes: u64,
    pub maximum_paged_growth_bytes: u64,
    pub maximum_machine_cpu_percent: f64,
    pub maximum_disk_busy_percent: f64,
    pub maximum_pages_input_per_s: f64,
}
impl Default for Budget {
    fn default() -> Self {
        Self { minimum_available_bytes: 1536 * MIB, maximum_commit_ratio: 0.85, maximum_memory_pressure_ratio: 0.90,
            maximum_commit_growth_bytes: 1024 * MIB, maximum_system_handle_growth: 512,
            maximum_system_handle_growth_per_second: 120.0,
            maximum_nonpaged_growth_bytes: 128 * MIB, maximum_paged_growth_bytes: 256 * MIB,
            maximum_machine_cpu_percent: 85.0, maximum_disk_busy_percent: 90.0,
            maximum_pages_input_per_s: 10_000.0 }
    }
}
/// Provisional conservative limits, not a claim of a universal safe operating point.
pub fn growth_violations(b: &Budget, baseline: &Sample, now: &Sample) -> Vec<String> {
    let mut out = Vec::new();
    if !now.counter_errors.is_empty() { out.push("required performance counters have errors".into()); }
    if now.physical_available_bytes < b.minimum_available_bytes { out.push("available physical memory below floor".into()); }
    if now.commit_ratio > b.maximum_commit_ratio { out.push("machine commit ratio above limit".into()); }
    if now.memory_pressure_ratio > b.maximum_memory_pressure_ratio { out.push("machine memory pressure above limit".into()); }
    if now.commit_bytes.saturating_sub(baseline.commit_bytes) > b.maximum_commit_growth_bytes { out.push("machine commit growth exceeded budget".into()); }
    if now.pool_nonpaged_bytes.saturating_sub(baseline.pool_nonpaged_bytes) > b.maximum_nonpaged_growth_bytes { out.push("nonpaged pool growth exceeded budget".into()); }
    if now.pool_paged_bytes.saturating_sub(baseline.pool_paged_bytes) > b.maximum_paged_growth_bytes { out.push("paged pool growth exceeded budget".into()); }
    if now.cpu_percent.is_none_or(|v| v > b.maximum_machine_cpu_percent) { out.push("machine CPU counter unavailable or above limit".into()); }
    if now.disk_busy_percent.is_none_or(|v| v > b.maximum_disk_busy_percent) { out.push("disk activity counter unavailable or above limit".into()); }
    if now.pages_input_per_s.is_some_and(|v| v > b.maximum_pages_input_per_s) {
        out.push("paging input sample exceeded configured threshold".into());
    }
    match (baseline.system_handles, now.system_handles) {
        (Some(a), Some(n)) if n.saturating_sub(a) > b.maximum_system_handle_growth => out.push("System handle growth exceeded budget".into()),
        (None, _) | (_, None) => out.push("System handle counter unavailable".into()),
        _ => {}
    }
    out
}

/// Detect rapid System-handle growth between regular resource-guard samples.
/// Counter decreases and sub-sample time deltas do not count as growth.
pub fn system_handle_rate_violation(b:&Budget,previous:&Sample,now:&Sample)->Option<String> {
    let (Some(before),Some(after))=(previous.system_handles,now.system_handles) else {
        return Some("System handle rate counter unavailable".into());
    };
    let elapsed=now.elapsed_s-previous.elapsed_s;
    if !elapsed.is_finite() || elapsed<0.25 { return None; }
    let growth=after.saturating_sub(before) as f64;
    if growth/elapsed>b.maximum_system_handle_growth_per_second {
        Some("System handle growth rate exceeded budget".into())
    } else { None }
}

/// Snapshot startup is allowed only with enough machine-wide headroom for a
/// bounded resident query index. This is stricter on free memory than a small
/// native-volume probe; it does not change production collector limits.
#[cfg(test)]
pub fn snapshot_load_violations(baseline:&Sample,now:&Sample)->Vec<String> {
    let budget=Budget {minimum_available_bytes:3*1024*MIB,..Budget::default()};
    growth_violations(&budget,baseline,now)
}

/// An already abnormal host is never a native-volume validation environment.
pub fn native_host_violations(sample: &Sample) -> Vec<String> {
    let mut out = Vec::new();
    if sample.system_handles.is_none_or(|n| n > 100_000) { out.push("System handle baseline is abnormal or unavailable".into()); }
    if sample.pool_nonpaged_bytes > 2048 * MIB { out.push("nonpaged pool baseline exceeds 2 GiB".into()); }
    if sample.pool_paged_bytes > 4096 * MIB { out.push("paged pool baseline exceeds 4 GiB".into()); }
    if sample.physical_available_bytes < 2048 * MIB { out.push("less than 2 GiB physical headroom".into()); }
    if sample.commit_ratio > 0.8 { out.push("machine commit ratio exceeds 80%".into()); }
    if sample.memory_pressure_ratio > 0.90 { out.push("machine memory pressure exceeds 90%".into()); }
    if !sample.counter_errors.is_empty() { out.push("required performance counters have errors".into()); }
    out
}

#[derive(Clone, Default)]
pub struct Cancellation { cancelled:Arc<AtomicBool>, parent:Option<Arc<Cancellation>> }
impl Cancellation {
    pub fn cancel(&self) { self.cancelled.store(true, Ordering::Release); }
    /// Create an operation-local cancellation that also observes its owner.
    /// Resource guards can cancel a single reload without stopping the host.
    pub fn child_of(parent:&Cancellation)->Self {
        Self {cancelled:Arc::new(AtomicBool::new(false)),parent:Some(Arc::new(parent.clone()))}
    }
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire) || self.parent.as_ref().is_some_and(|p|p.is_cancelled())
    }
    pub fn check(&self) -> anyhow::Result<()> {
        anyhow::ensure!(!self.is_cancelled(), "operation cancelled by resource guard or caller");
        Ok(())
    }
}

#[cfg(test)] mod tests {
    use super::*;
    #[test] fn child_cancellation_is_local_but_observes_host_shutdown() {
        let host=Cancellation::default();
        let reload=Cancellation::child_of(&host);
        reload.cancel();
        assert!(reload.is_cancelled());
        assert!(!host.is_cancelled(),"a failed reload must not stop query hosting");
        let second=Cancellation::child_of(&host);
        host.cancel();
        assert!(second.is_cancelled(),"host shutdown must cancel in-flight reload work");
    }
    #[test] fn guard_checks_system_resources_and_missing_counters() {
        let b = Budget::default();
        let base = Sample { physical_available_bytes:4*1024*MIB, system_handles:Some(1000),
            commit_bytes:8*1024*MIB, commit_ratio:0.4, pool_nonpaged_bytes:100*MIB, pool_paged_bytes:200*MIB,
            cpu_percent:Some(5.0), disk_busy_percent:Some(2.0),
            ..Default::default() };
        assert!(growth_violations(&b,&base,&base).is_empty());
        let mut now=base.clone(); now.system_handles=Some(1513);
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("System handle")));
        now=base.clone(); now.pool_nonpaged_bytes+=129*MIB;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("nonpaged")));
        now=base.clone(); now.physical_available_bytes=1024*MIB;
        assert!(!growth_violations(&b,&base,&now).is_empty());
        now=base.clone(); now.memory_pressure_ratio=b.maximum_memory_pressure_ratio+0.01;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("memory pressure")));
        now.memory_pressure_ratio=b.maximum_memory_pressure_ratio;
        assert!(!growth_violations(&b,&base,&now).iter().any(|s|s.contains("memory pressure")));
        now=base.clone(); now.system_handles=None;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("unavailable")));
        now=base.clone(); now.cpu_percent=None;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("CPU counter unavailable")));
        now=base.clone(); now.disk_busy_percent=None;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("disk activity counter unavailable")));
        now=base.clone(); now.commit_bytes+=b.maximum_commit_growth_bytes+1;
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("commit growth")));
        now=base.clone(); now.pages_input_per_s=Some(b.maximum_pages_input_per_s+1.0);
        assert!(growth_violations(&b,&base,&now).iter().any(|s|s.contains("paging input")));
        now.pages_input_per_s=Some(b.maximum_pages_input_per_s);
        assert!(!growth_violations(&b,&base,&now).iter().any(|s|s.contains("paging input")));
    }
    #[test] fn system_handle_rate_guard_catches_scan_growth_but_ignores_short_or_falling_samples() {
        let budget=Budget::default();
        let before=Sample {elapsed_s:4.0,system_handles:Some(100_000),..Default::default()};
        let fast=Sample {elapsed_s:5.0,system_handles:Some(100_151),..Default::default()};
        assert!(system_handle_rate_violation(&budget,&before,&fast).is_some());
        let modest=Sample {elapsed_s:5.0,system_handles:Some(100_100),..Default::default()};
        assert!(system_handle_rate_violation(&budget,&before,&modest).is_none());
        let falling=Sample {elapsed_s:5.0,system_handles:Some(99_900),..Default::default()};
        assert!(system_handle_rate_violation(&budget,&before,&falling).is_none());
        let too_close=Sample {elapsed_s:4.1,system_handles:Some(100_151),..Default::default()};
        assert!(system_handle_rate_violation(&budget,&before,&too_close).is_none());
    }
    #[test] fn native_host_gate_rejects_memory_pressure_above_ninety_percent() {
        let mut sample=Sample {physical_available_bytes:4*1024*MIB,system_handles:Some(10_000),
            commit_bytes:8*1024*MIB,commit_ratio:0.5,pool_nonpaged_bytes:128*MIB,pool_paged_bytes:256*MIB,
            memory_pressure_ratio:0.91,..Default::default()};
        assert!(native_host_violations(&sample).iter().any(|s|s.contains("memory pressure")));
        sample.memory_pressure_ratio=0.90;
        assert!(!native_host_violations(&sample).iter().any(|s|s.contains("memory pressure")));
    }
    #[test] fn snapshot_load_requires_extra_machine_headroom() {
        let base=Sample {physical_available_bytes:4*1024*MIB,system_handles:Some(1000),
            commit_bytes:8*1024*MIB,commit_ratio:0.4,pool_nonpaged_bytes:100*MIB,pool_paged_bytes:200*MIB,
            cpu_percent:Some(5.0),disk_busy_percent:Some(2.0),pages_input_per_s:Some(1.0),..Default::default()};
        let mut now=base.clone();now.physical_available_bytes=3*1024*MIB-1;
        assert!(snapshot_load_violations(&base,&now).iter().any(|s|s.contains("available physical memory")));
        now=base.clone();now.memory_pressure_ratio=0.91;
        assert!(snapshot_load_violations(&base,&now).iter().any(|s|s.contains("memory pressure")));
        now=base.clone();now.commit_bytes+=1024*MIB+1;
        assert!(snapshot_load_violations(&base,&now).iter().any(|s|s.contains("commit growth")));
    }
}
