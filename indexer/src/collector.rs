//! Bounded, read-only metadata synchronization. Source injection makes the state machine
//! testable without opening any host volume or installing a driver/service.
use crate::{catalog::{Catalog, VolumeMeta}, policy::Cancellation};
use anyhow::{Result, ensure, bail, Context};
use goz_core::{index::{LinkTarget, WtfName}, layout::LayoutFile,
    usn::{JournalInfo, JournalWalk, Resync, SavedCursor, validate_cursor}, types::Frn};
use serde::{Deserialize, Serialize};
use std::{collections::HashSet, time::{Duration, Instant}};

pub trait MetadataSource {
    fn journal(&mut self) -> Result<JournalInfo>;
    fn layout_page(&mut self, restart: bool) -> Result<Option<Vec<LayoutFile>>>;
    fn changes(&mut self, cursor: i64, journal_id: u64) -> Result<JournalWalk>;
    /// Read the actual hard-link names for one FRN from the provider.
    fn file_layout(&mut self, frn: Frn) -> Result<LayoutFile>;
    /// Variant used by the state machine when a provider needs the current
    /// private FRN graph to resolve a path-based hard-link API.
    fn file_layout_with_catalog(&mut self,frn:Frn,_catalog:&Catalog)->Result<LayoutFile> { self.file_layout(frn) }
    /// Complete provider-specific initial hard-link reconciliation. Providers
    /// may checkpoint private, unpublished progress between bounded batches.
    fn finish_initial_links<F>(&mut self,_catalog:&mut Catalog,_cancel:&Cancellation,_checkpoint:&mut F)->Result<()>
    where Self:Sized, F:FnMut(&Self,&Catalog)->Result<()> { Ok(()) }
    /// Some sources return multiple FILE-record fragments for one base FRN;
    /// those names must be merged add-only to avoid rewriting another link.
    fn layout_pages_are_fragments(&self)->bool { false }
    /// The first page of an MFT enumeration must supply the volume-specific
    /// sequence number for NTFS root record 5 before entries are inserted.
    fn requires_observed_root(&self)->bool { false }
    fn observed_root(&self,files:&[LayoutFile])->Result<Option<Frn>> {
        let mut roots=HashSet::new();
        for file in files {
            if file.frn.record_number()==5 { roots.insert(file.frn); }
            for name in &file.names {
                if name.parent_frn.record_number()==5 { roots.insert(name.parent_frn); }
            }
        }
        ensure!(roots.len()<=1,"enumeration page contains conflicting NTFS root references");
        Ok(roots.into_iter().next())
    }
    fn checkpoint_state(&self)->Result<Option<serde_json::Value>> { Ok(None) }
    fn restore_checkpoint_state(&mut self,_state:serde_json::Value)->Result<()> {
        bail!("metadata source has no resumable initial-build cursor")
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Limits {
    pub max_entries: usize,
    pub max_index_bytes: u64,
    pub max_layout_pages: usize,
    pub max_journal_batches: usize,
    pub max_link_refreshes_per_batch: usize,
    pub total_seconds: u64,
    pub page_pause_ms: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self { max_entries: 1_000_000, max_index_bytes: 512 * 1024 * 1024,
            max_layout_pages: 16_384, max_journal_batches: 256, max_link_refreshes_per_batch: 128,
            total_seconds: 120, page_pause_ms: 5 }
    }
}
fn check(c: &Catalog, limits: &Limits, started: Instant, cancel: &Cancellation) -> Result<()> {
    cancel.check()?;
    ensure!(started.elapsed() < Duration::from_secs(limits.total_seconds), "metadata synchronization total deadline exceeded");
    ensure!(c.index.len() <= limits.max_entries, "metadata entry budget exceeded");
    ensure!(c.index.memory().total().allocated.saturating_add(c.query_index_bytes()) <= limits.max_index_bytes,
        "metadata index memory budget exceeded");
    Ok(())
}
fn validate(c: &Catalog, live: &JournalInfo) -> Result<()> {
    ensure!(validate_cursor(&SavedCursor { journal_id: c.meta.journal_id, next_usn: c.meta.next_usn },live) == Resync::InSync,
        "journal discontinuity: snapshot is stale; automatic rebuild is disabled");
    Ok(())
}
fn replay(c: &mut Catalog, source: &mut impl MetadataSource, limits: &Limits,
          started: Instant, cancel: &Cancellation, indexed: bool) -> Result<()> {
    cancel.check()?;
    let live = source.journal()?; validate(c,&live)?;
    let target = live.next_usn;
    for batches in 0..=limits.max_journal_batches {
        check(c,limits,started,cancel)?;
        if c.meta.next_usn >= target {
            validate(c,&source.journal()?)?;
            c.meta.stale = false;
            return Ok(());
        }
        ensure!(batches < limits.max_journal_batches,"journal catch-up batch budget exhausted; snapshot remains unpublished");
        let batch = source.changes(c.meta.next_usn,c.meta.journal_id)?;
        ensure!(batch.skipped.unknown_version == 0, "unknown USN record versions: cannot prove completeness");
        ensure!(batch.next_usn > c.meta.next_usn, "journal reader made no forward progress");
        let frns: Vec<_> = batch.records.iter().map(|r|r.frn).collect::<HashSet<_>>().into_iter().collect();
        let directories_changed=batch.records.iter().any(|r| r.attributes&0x10!=0 ||
            c.index.head_of(r.frn).is_some_and(|idx|c.index.entry(idx).is_dir()));
        if indexed { c.detach_names(&frns,cancel)?; }
        let outcome = c.index.apply_batch(&batch.records);
        ensure!(outcome.needs_link_reconcile.len() <= limits.max_link_refreshes_per_batch,
            "hard-link refresh batch exceeds budget");
        for frn in outcome.needs_link_reconcile {
            cancel.check()?;
            let file = source.file_layout_with_catalog(frn,c)?;
            let mut actual = Vec::new();
            for name in file.names.into_iter().filter(|n| !n.dos_only) {
                let parent = c.index.head_of(name.parent_frn).ok_or_else(|| anyhow::anyhow!("hard-link parent missing from index"))?;
                actual.push(LinkTarget { parent, name: WtfName::new(name.name,name.name_lossy) });
            }
            c.index.reconcile_links(frn,&actual);
        }
        if indexed { c.attach_names(&frns,directories_changed,cancel)?; }
        // Cursor advances only after the complete batch and its required name reconciliation.
        c.meta.next_usn = batch.next_usn;
        if limits.page_pause_ms > 0 { std::thread::sleep(Duration::from_millis(limits.page_pause_ms)); }
    }
    bail!("journal catch-up batch budget exhausted; snapshot remains unpublished")
}

#[cfg(test)]
pub fn build(source: &mut impl MetadataSource, meta: VolumeMeta,
             limits: &Limits, cancel: &Cancellation) -> Result<Catalog> {
    build_checkpointed(source,meta,None,limits,cancel,|_,_|Ok(()))
}
/// Build a private generation, periodically saving a cursor/state pair through
/// `checkpoint`. Partial generations are never made queryable by this module.
pub fn build_checkpointed<S:MetadataSource>(source: &mut S, mut meta: VolumeMeta,
             partial:Option<Catalog>, limits: &Limits, cancel: &Cancellation,
             mut checkpoint:impl FnMut(&S,&Catalog)->Result<()>) -> Result<Catalog> {
    cancel.check()?;
    let started = Instant::now();
    let resumed=partial.is_some();
    let mut c=if let Some(c)=partial {
        let live=source.journal()?;validate(&c,&live)?;
        ensure!(c.meta.stale,"partial generation must remain unpublished/stale");
        c
    } else {
        let live = source.journal()?;
        meta.journal_id = live.journal_id; meta.next_usn = live.next_usn; meta.stale = true;
        Catalog::empty(meta)
    };
    let mut complete = false;
    for page in 0..limits.max_layout_pages {
        check(&c,limits,started,cancel)?;
        match source.layout_page(page == 0 && !resumed)? {
            None => { complete = true; break; }
            Some(files) => {
                ensure!(!files.is_empty(), "layout source returned an empty nonterminal page");
                if page==0 && !resumed && source.requires_observed_root() {
                    let root=source.observed_root(&files)?.context("first USN page did not expose the sequence-bearing reference to NTFS root record 5")?;
                    c.set_empty_root(root)?;
                }
                let fragments=source.layout_pages_are_fragments();
                for file in files { if fragments {c.index.enrich(&file)} else {c.index.insert_layout(&file)} }
            }
        }
        check(&c,limits,started,cancel)?;
        checkpoint(source,&c)?;
        if limits.page_pause_ms > 0 { std::thread::sleep(Duration::from_millis(limits.page_pause_ms)); }
    }
    ensure!(complete, "layout page budget exhausted");
    source.finish_initial_links(&mut c,cancel,&mut |source,catalog| {
        check(catalog,limits,started,cancel)?;
        checkpoint(source,catalog)
    })?;
    replay(&mut c,source,limits,started,cancel,false)?;
    c.index.optimize_storage(); c.rebuild_query_index(cancel)?;
    check(&c,limits,started,cancel)?;
    Ok(c)
}
/// Resume a *private, unpublished* loaded snapshot. Failure marks it stale; callers keep
/// their existing published generation. No journal errors cause automatic whole-volume re-enumeration.
pub fn catch_up(c: &mut Catalog, source: &mut impl MetadataSource, limits: &Limits,
                cancel: &Cancellation) -> Result<()> {
    c.meta.stale = true;
    let started = Instant::now();
    let result = (|| {
        replay(c,source,limits,started,cancel,true)?;
        check(c,limits,started,cancel)
    })();
    if result.is_err() { c.meta.stale = true; }
    result
}

#[cfg(test)] mod tests {
    use super::*;
    use anyhow::Context;
    use crate::catalog::{Query, layout};
    use goz_core::{index::NTFS_ROOT_FRN, usn::{ParsedUsnRecord, SkipCounts}};
    struct Fake { pages: Vec<Vec<LayoutFile>>, calls: usize, changed: bool, journal_id: u64, require_root:bool }
    impl MetadataSource for Fake {
        fn journal(&mut self) -> Result<JournalInfo> {
            Ok(JournalInfo { journal_id: self.journal_id, first_usn: 0, next_usn: if self.calls == 0 {10} else {20}, lowest_valid_usn: 0, max_usn: i64::MAX })
        }
        fn layout_page(&mut self, _: bool) -> Result<Option<Vec<LayoutFile>>> {
            self.calls += 1;
            Ok(if self.pages.is_empty() { None } else { Some(self.pages.remove(0)) })
        }
        fn changes(&mut self, _: i64, _: u64) -> Result<JournalWalk> {
            self.changed = true;
            Ok(JournalWalk { next_usn: 20, records: vec![ParsedUsnRecord { major_version:2, frn:Frn(200), parent_frn:Frn(100),
                usn: 15, timestamp_ft:0, reason:0x2000, attributes:0, name:b"new.exe".to_vec(), name_lossy:false }], skipped:SkipCounts::default() })
        }
        fn file_layout(&mut self, _: Frn) -> Result<LayoutFile> { bail!("no hard-link changes in this fixture") }
        fn requires_observed_root(&self)->bool { self.require_root }
    }
    fn fixture() -> (Fake,VolumeMeta,Limits) {
        (Fake { pages: vec![vec![layout(100,NTFS_ROOT_FRN.0,b"workshop",true),layout(200,100,b"old.exe",false)]],calls:0,changed:false,journal_id:7,require_root:false },
         VolumeMeta { identity:"test".into(),drive:'C',journal_id:0,next_usn:0,stale:true },
         Limits { page_pause_ms:0, ..Default::default() })
    }
    #[test] fn bootstrap_replays_changes_that_arrive_during_enumeration() {
        let (mut source,meta,limits) = fixture(); let cancel = Cancellation::default();
        let c = build(&mut source,meta,&limits,&cancel).unwrap();
        assert!(source.changed); assert!(!c.meta.stale); assert_eq!(c.meta.next_usn,20);
        let r = c.query(&Query { words:vec!["new".into()],workshop_only:true,..Default::default() },&cancel).unwrap();
        assert_eq!(r.hits.len(),1); assert!(r.hits[0].path.ends_with("new.exe"));
    }
    #[test] fn bootstrap_uses_the_observed_sequence_for_ntfs_root_record_five() {
        let (mut source,meta,limits)=fixture();
        let root=Frn((1u64<<48)|5);
        source.require_root=true;
        source.pages=vec![vec![layout(100,root.0,b"workshop",true),layout(200,100,b"flowtary.exe",false)]];
        let cancel=Cancellation::default();
        let catalog=build(&mut source,meta,&limits,&cancel).unwrap();
        assert_eq!(catalog.index.entry(catalog.index.root()).frn(),root);
        let path=String::from_utf16(&catalog.api_path_for_frn(Frn(200)).unwrap()).unwrap();
        assert_eq!(path,r"C:\workshop\new.exe");
    }
    #[test] fn cancellation_happens_before_any_volume_source_call() {
        let (mut source,meta,limits) = fixture(); let cancel = Cancellation::default(); cancel.cancel();
        assert!(build(&mut source,meta,&limits,&cancel).is_err()); assert_eq!(source.calls,0);
    }
    #[test] fn fragment_add_only_ingestion_preserves_links_from_separate_mft_records() {
        let cancel=Cancellation::default();
        let mut catalog=Catalog::empty(VolumeMeta {identity:"fixture".into(),drive:'C',journal_id:1,next_usn:1,stale:false});
        catalog.index.enrich(&layout(NTFS_ROOT_FRN.0+1,NTFS_ROOT_FRN.0,b"workshop",true));
        catalog.index.enrich(&layout(200,NTFS_ROOT_FRN.0+1,b"base.exe",false));
        catalog.index.enrich(&layout(200,NTFS_ROOT_FRN.0+1,b"extension-link.exe",false));
        catalog.rebuild_query_index(&cancel).unwrap();
        for name in ["base","extension-link"] {
            let result=catalog.query(&Query {words:vec![name.into()],workshop_only:true,..Default::default()},&cancel).unwrap();
            assert_eq!(result.hits.len(),1,"{name}");
        }
    }
    #[test] fn limits_and_journal_identity_change_never_start_an_automatic_rebuild() {
        let (mut source,meta,mut limits) = fixture(); let cancel = Cancellation::default(); limits.max_layout_pages=1;
        assert!(build(&mut source,meta,&limits,&cancel).is_err()); assert_eq!(source.calls,1);
        let (mut source,meta,limits) = fixture(); let mut c = build(&mut source,meta,&limits,&cancel).unwrap();
        let calls = source.calls; source.journal_id=9;
        assert!(catch_up(&mut c,&mut source,&limits,&cancel).is_err()); assert!(c.meta.stale); assert_eq!(source.calls,calls);
    }
    struct Delta { target:i64, records:Vec<ParsedUsnRecord>, link:Option<LayoutFile>, reads:usize }
    impl MetadataSource for Delta {
        fn journal(&mut self)->Result<JournalInfo> { Ok(JournalInfo { journal_id:7,first_usn:0,next_usn:self.target,lowest_valid_usn:0,max_usn:i64::MAX }) }
        fn layout_page(&mut self,_:bool)->Result<Option<Vec<LayoutFile>>> { bail!("incremental update must never enumerate a volume") }
        fn changes(&mut self,_:i64,_:u64)->Result<JournalWalk> {
            self.reads+=1;
            Ok(JournalWalk { next_usn:self.target,records:std::mem::take(&mut self.records),skipped:SkipCounts::default() })
        }
        fn file_layout(&mut self,_:Frn)->Result<LayoutFile> { self.link.take().context("unexpected link refresh") }
    }
    fn record(frn:u64,parent:u64,name:&[u8],reason:u32,dir:bool)->ParsedUsnRecord {
        ParsedUsnRecord { major_version:2,frn:Frn(frn),parent_frn:Frn(parent),usn:20,timestamp_ft:0,reason,
            attributes:if dir {0x10} else {0},name:name.into(),name_lossy:false }
    }
    #[test] fn incremental_updates_preserve_postings_across_renames_links_and_directory_moves() {
        let (mut source,meta,mut limits)=fixture(); let cancel=Cancellation::default();
        let mut c=build(&mut source,meta,&limits,&cancel).unwrap();
        // One allowed batch must succeed, including the final target check.
        limits.max_journal_batches=1;
        let apply=|c:&mut Catalog,records,link| {
            let mut delta=Delta { target:c.meta.next_usn+10,records,link,reads:0 };
            catch_up(c,&mut delta,&limits,&cancel).unwrap(); assert_eq!(delta.reads,1);
        };
        apply(&mut c,vec![record(200,100,b"renamed.exe",0x2000,false),record(201,100,b"second.exe",0x100,false)],None);
        let query=|c:&Catalog,word:&str,workshop_only| c.query(&Query { words:vec![word.into()],workshop_only,..Default::default() },&cancel).unwrap().hits;
        assert!(query(&c,"new",false).is_empty()); assert_eq!(query(&c,"renamed",true).len(),1);
        // A directory rename reclassifies all descendant executable paths.
        apply(&mut c,vec![record(100,NTFS_ROOT_FRN.0,b"elsewhere",0x2000,true)],None);
        assert!(query(&c,"renamed",true).is_empty()); assert!(query(&c,"renamed",false)[0].path.contains("elsewhere"));
        apply(&mut c,vec![record(100,NTFS_ROOT_FRN.0,b"workshop",0x2000,true)],None);
        assert_eq!(query(&c,"renamed",true).len(),1);
        // Hard-link refresh replaces every old posting and adds every real name.
        let mut links=layout(200,100,b"renamed.exe",false);
        links.names.extend(layout(200,100,b"alias.exe",false).names);
        apply(&mut c,vec![record(200,100,b"renamed.exe",0x10000,false)],Some(links));
        assert_eq!(query(&c,"alias",true).len(),1);
        apply(&mut c,vec![record(200,100,b"renamed.exe",0x200,false)],None);
        assert!(query(&c,"alias",false).is_empty()); assert!(query(&c,"renamed",false).is_empty());
        let long=format!("{}second.exe","x".repeat(100));
        apply(&mut c,vec![record(201,100,long.as_bytes(),0x2000,false)],None);
        let before=query(&c,"second",true); assert_eq!(before.len(),1);
        c.rebuild_query_index(&cancel).unwrap(); assert_eq!(before,query(&c,"second",true));
        let mut idle=Delta { target:c.meta.next_usn,records:vec![],link:None,reads:0 };
        catch_up(&mut c,&mut idle,&limits,&cancel).unwrap(); assert_eq!(idle.reads,0);
    }
}
