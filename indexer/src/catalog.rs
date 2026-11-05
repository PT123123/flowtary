use crate::policy::Cancellation;
use anyhow::{Result, ensure};
use goz_core::{fold::fold_into, index::{FrnMap, NTFS_ROOT_FRN, PathStatus, VolumeIndex},
    layout::{LayoutFile, LayoutName}, types::{EntryIdx, Frn, NIL}};
use serde::{Deserialize, Serialize};
use std::{cmp::Reverse, collections::{BinaryHeap, HashMap, HashSet}, time::Instant};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct VolumeMeta {
    pub identity: String,
    pub drive: char,
    pub journal_id: u64,
    pub next_usn: i64,
    pub stale: bool,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Query {
    pub words: Vec<String>,
    #[serde(default)] pub path_words: Vec<String>,
    #[serde(default)] pub excludes: Vec<String>,
    #[serde(default)] pub extension: Option<String>,
    #[serde(default)] pub folders_only: bool,
    #[serde(default)] pub workshop_only: bool,
    #[serde(default = "default_limit")] pub limit: usize,
    #[serde(default = "default_budget")] pub budget_ms: u64,
}
fn default_limit() -> usize { 50 }
fn default_budget() -> u64 { 25 }
impl Default for Query {
    fn default() -> Self { Self { words: vec![], path_words: vec![], excludes: vec![], extension: None,
        folders_only: false, workshop_only: false, limit: 50, budget_ms: 25 } }
}
#[derive(Debug, Clone, Serialize, Deserialize, Eq, PartialEq)]
pub struct Hit {
    pub path: String,
    #[serde(default)] pub path_utf16: Vec<u16>,
    pub is_folder: bool,
    pub workshop: bool,
    pub name_score: u16,
    pub version: Vec<u64>,
    pub backup: bool,
}
impl Ord for Hit {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.workshop.cmp(&other.workshop)
            .then_with(|| self.name_score.cmp(&other.name_score))
            .then_with(|| other.backup.cmp(&self.backup))
            .then_with(|| self.version.cmp(&other.version))
            .then_with(|| other.path_utf16.cmp(&self.path_utf16))
            .then_with(|| self.is_folder.cmp(&other.is_folder))
    }
}
impl PartialOrd for Hit { fn partial_cmp(&self, o: &Self) -> Option<std::cmp::Ordering> { Some(self.cmp(o)) } }
#[derive(Debug, Serialize, Deserialize)]
pub struct QueryResult {
    pub hits: Vec<Hit>,
    pub elapsed_us: u64,
    pub candidates_examined: u64,
    pub truncated: bool,
    pub stale: bool,
}
pub struct Catalog {
    pub meta: VolumeMeta,
    pub index: VolumeIndex,
    grams: HashMap<[u8; 3], Vec<EntryIdx>>,
    long_names: Vec<EntryIdx>,
    exe: Vec<EntryIdx>,
    workshop: Vec<EntryIdx>,
}
pub fn folded(s: &[u8]) -> Vec<u8> { let mut out = Vec::new(); fold_into(s, &mut out); out }
fn contains(hay: &[u8], needle: &[u8]) -> bool { needle.is_empty() || memchr::memmem::find(hay, needle).is_some() }
const MAX_PATH_QUERY_CANDIDATES:usize=250_000;
fn add_path_candidate(index:&VolumeIndex,idx:EntryIdx,expand_directory:bool,started:Instant,budget_ms:u64,
    cancel:&Cancellation,work:&mut usize,seen:&mut HashSet<EntryIdx>,entries:&mut Vec<EntryIdx>)->bool {
    if *work%64==0 && (cancel.is_cancelled() || started.elapsed().as_millis()>=budget_ms as u128) {return false}
    *work+=1;
    if seen.contains(&idx) {return true}
    if entries.len()>=MAX_PATH_QUERY_CANDIDATES {return false}
    seen.insert(idx);entries.push(idx);
    if expand_directory && index.entry(idx).is_dir() {
        for descendant in index.subtree_entries(idx) {
            if !add_path_candidate(index,descendant,false,started,budget_ms,cancel,work,seen,entries) {return false}
        }
    }
    true
}
fn path_is_system_volume_information(path:&[u8])->bool {
    let root=b"system volume information";
    path.len()>=root.len() && path[..root.len()].eq_ignore_ascii_case(root)
        && (path.len()==root.len() || path[root.len()]==b'\\')
}
pub fn layout(frn: u64, parent: u64, name: &[u8], dir: bool) -> LayoutFile {
    LayoutFile { frn: Frn(frn), attributes: if dir { 0x10 } else { 0 }, size: None, mtime_ft: None,
        names: vec![LayoutName { parent_frn: Frn(parent), name: name.to_vec(), name_lossy: std::str::from_utf8(name).is_err(), dos_only: false }] }
}
impl Catalog {
    pub fn empty(meta: VolumeMeta) -> Self {
        Self { meta, index: VolumeIndex::new(NTFS_ROOT_FRN, FrnMap::sparse()),
            grams: HashMap::new(), long_names: vec![], exe: vec![], workshop: vec![] }
    }
    /// Set the volume's actual NTFS root reference before any enumerated entry
    /// is ingested. NTFS record 5's sequence number is volume-specific.
    pub fn set_empty_root(&mut self, root:Frn) -> Result<()> {
        ensure!(root.record_number()==5,"observed NTFS root reference does not name MFT record 5");
        if self.index.entry(self.index.root()).frn()==root { return Ok(()) }
        ensure!(self.index.len()==2,"cannot replace the NTFS root after entries have been ingested");
        self.index=VolumeIndex::new(root,FrnMap::sparse());
        Ok(())
    }
    /// Resolve one indexed FRN to a path suitable for the Windows hard-link
    /// APIs. The path comes only from the in-memory parent graph; no walker or
    /// filesystem lookup is used to construct it.
    pub fn api_path_for_frn(&self, frn: Frn) -> Result<Vec<u16>> {
        let mut idx=self.index.head_of(frn).ok_or_else(||anyhow::anyhow!("FRN is absent from the private catalog"))?;
        let mut fallback=None;
        let mut selected=None;
        while idx!=NIL {
            let mut path=Vec::new();
            let status=self.index.path_of(idx,&mut path);
            if status==PathStatus::Ok && !path_is_system_volume_information(&path) {
                selected=Some((idx,path));break
            }
            if fallback.is_none(){fallback=Some((idx,status,path))}
            idx=self.index.entry(idx).next_link();
        }
        let (idx,status,relative)=if let Some((idx,path))=selected {(idx,PathStatus::Ok,path)}
            else {fallback.ok_or_else(||anyhow::anyhow!("FRN has no indexed hard-link names"))?};
        ensure!(!self.index.is_synthetic(idx),"cannot resolve a synthetic FRN to a filesystem path");
        if status!=PathStatus::Ok {
            let entry=self.index.entry(idx);
            let parent_idx=entry.parent();
            let parent=self.index.entry(parent_idx);
            anyhow::bail!("FRN {:#018X} name {:?} parent {:#018X} {:?} root {:#018X} path is cyclic, orphaned, or incomplete ({status:?})",
                frn.0,String::from_utf8_lossy(entry.name()),parent.frn().0,String::from_utf8_lossy(parent.name()),
                self.index.entry(self.index.root()).frn().0);
        }
        let mut full=format!("{}:\\",self.meta.drive.to_ascii_uppercase()).encode_utf16().collect::<Vec<_>>();
        full.extend(goz_core::wtf8::to_utf16(&relative).into_iter().map(|unit|if unit==b'/' as u16 {b'\\' as u16}else{unit}));
        ensure!(full.len()<=32767,"resolved path exceeds the Windows path limit");
        Ok(full)
    }
    /// Resolve the volume-relative link names returned by
    /// FindFirstFileNameW/FindNextFileNameW back to exact parent FRNs.
    pub fn layout_name_for_api_link(&self, link:&[u16]) -> Result<LayoutName> {
        ensure!(link.len()>=2 && link.len()<=32767 && link[0]==b'\\' as u16,
            "hard-link API returned a non-rooted or oversized volume-relative name");
        ensure!(!link.contains(&0),"hard-link API returned an embedded NUL");
        let components:Vec<&[u16]>=link.split(|unit|*unit==b'\\' as u16).collect();
        ensure!(components.first().is_some_and(|c|c.is_empty()) && components.len()>=2,
            "hard-link API path has no parent/name pair");
        let mut parent=self.index.root();
        for component in &components[1..components.len()-1] {
            ensure!(!component.is_empty() && *component!=[b'.' as u16] && *component!=[b'.' as u16,b'.' as u16],
                "hard-link API path contains an empty or traversal component");
            let mut bytes=Vec::new();
            goz_core::wtf8::from_utf16(component,&mut bytes);
            ensure!(!bytes.iter().any(|b|*b==b'/' || *b==b'\\' || *b==0),"invalid directory component in hard-link API path");
            parent=self.index.child_dir_exact(parent,&bytes)
                .ok_or_else(||anyhow::anyhow!("hard-link API parent directory is absent or ambiguous in the catalog"))?;
        }
        let name_units=components.last().copied().unwrap_or_default();
        ensure!(!name_units.is_empty() && name_units!=[b'.' as u16] && name_units!=[b'.' as u16,b'.' as u16],
            "hard-link API path has an invalid final component");
        let mut name=Vec::new();
        let name_lossy=goz_core::wtf8::from_utf16(name_units,&mut name);
        ensure!(!name.iter().any(|b|*b==b'/' || *b==b'\\' || *b==0),"invalid filename component in hard-link API path");
        Ok(LayoutName {parent_frn:self.index.entry(parent).frn(),name,name_lossy,dos_only:false})
    }
    /// Return stable, unique file IDs in FRN order. The bounded collector builds
    /// this once per run and resumes from a durable FRN cursor by binary search.
    pub fn file_frns_sorted(&self)->Vec<Frn> {
        let mut files=Vec::new();
        for idx in self.index.live_entries() {
            if self.index.is_synthetic(idx) {continue;}
            let entry=self.index.entry(idx);
            if entry.is_dir() || self.index.head_of(entry.frn())!=Some(idx) {continue;}
            files.push(entry.frn());
        }
        files.sort_unstable_by_key(|frn|frn.0);
        files
    }
    /// Return one stable FRN for each live directory so providers can perform
    /// bounded directory-batch reconciliation without issuing one name query
    /// per file. Synthetic roots are included; their path is supplied by the
    /// volume root rather than by the ordinary FRN path resolver.
    pub fn directory_frns_sorted(&self)->Vec<Frn> {
        let mut directories=Vec::new();
        let root=self.index.root();
        for idx in self.index.live_entries() {
            let entry=self.index.entry(idx);
            if entry.is_dir() && self.index.head_of(entry.frn())==Some(idx) &&
                (!self.index.is_synthetic(idx) || idx==root) {
                directories.push(entry.frn());
            }
        }
        directories.sort_unstable_by_key(|frn|frn.0);
        directories.dedup();
        directories
    }
    /// Windows protects System Volume Information from path-based hard-link
    /// enumeration. It is an explicit non-searchable metadata subtree; a
    /// multi-link file remains indexable when any link is outside that subtree.
    pub fn protected_system_metadata_frns(&self)->HashSet<Frn> {
        let mask=self.system_volume_information_mask();
        let mut protected=HashSet::new();
        for idx in self.index.live_entries() {
            let entry=self.index.entry(idx);
            if entry.is_dir() || self.index.head_of(entry.frn())!=Some(idx)
                || !mask.get(idx as usize).copied().unwrap_or(false) {continue}
            let mut link=idx;let mut all_protected=true;
            while link!=NIL {
                if !mask.get(link as usize).copied().unwrap_or(false) {all_protected=false;break}
                link=self.index.entry(link).next_link();
            }
            if all_protected {protected.insert(entry.frn());}
        }
        protected
    }
    fn system_volume_information_mask(&self)->Vec<bool> {
        let mut mask=vec![false;self.index.allocated()];
        let root=self.index.root();
        let Some(directory)=self.index.live_entries().find(|idx| {
            let entry=self.index.entry(*idx);
            entry.parent()==root && entry.is_dir() && entry.folded_name()==b"system volume information"
        }) else {return mask};
        mask[directory as usize]=true;
        self.index.for_each_subtree_entry(directory,|idx|mask[idx as usize]=true);
        mask
    }
    fn is_system_volume_information_entry(&self,idx:EntryIdx)->bool {
        let mut current=idx;
        for _ in 0..4096 {
            if current==self.index.root() || self.index.is_synthetic(current) {return false}
            let entry=self.index.entry(current);
            if entry.parent()==self.index.root() {
                return entry.is_dir() && entry.folded_name()==b"system volume information";
            }
            let parent=entry.parent();
            if parent==current || parent==NIL {return false}
            current=parent;
        }
        false
    }
    pub fn rebuild_query_index(&mut self, cancel: &Cancellation) -> Result<()> {
        self.rebuild_query_index_limited(cancel,256 * 1024 * 1024)
    }
    pub fn rebuild_query_index_limited(&mut self, cancel: &Cancellation, max_bytes: u64) -> Result<()> {
        self.grams.clear(); self.long_names.clear(); self.exe.clear(); self.workshop.clear();
        let protected_system_mask=self.system_volume_information_mask();
        let mut triples = Vec::with_capacity(64);
        for (n, idx) in self.index.live_entries().enumerate() {
            if n % 2048 == 0 {
                cancel.check()?;
                ensure!(self.query_index_bytes() <= max_bytes,"query posting memory budget exceeded");
            }
            if self.index.is_synthetic(idx) || protected_system_mask.get(idx as usize).copied().unwrap_or(false) { continue; }
            let entry = self.index.entry(idx);
            let name = entry.folded_name();
            if !entry.is_dir() && name.ends_with(b".exe") {
                self.exe.push(idx);
                if self.is_workshop(idx) { self.workshop.push(idx); }
            }
            // Long names are a separate complete fallback list, never partially indexed.
            if name.len() > 66 { self.long_names.push(idx); continue; }
            triples.clear();
            triples.extend(name.windows(3).map(|w| [w[0], w[1], w[2]]));
            triples.sort_unstable(); triples.dedup();
            for gram in &triples { self.grams.entry(*gram).or_default().push(idx); }
        }
        ensure!(self.query_index_bytes() <= max_bytes,"query posting memory budget exceeded");
        Ok(())
    }
    pub fn query_index_bytes(&self) -> u64 {
        (self.grams.values().map(|v| v.capacity() * size_of::<EntryIdx>()).sum::<usize>()
            + self.grams.capacity() * (size_of::<[u8;3]>() + size_of::<Vec<EntryIdx>>() + 8)
            + (self.long_names.capacity() + self.exe.capacity() + self.workshop.capacity()) * size_of::<EntryIdx>()) as u64
    }
    /// Build a bounded superset for path-only searches from name-component
    /// trigrams. A matching directory contributes its descendants; exact path
    /// predicates still run in `query`, and time/candidate caps set truncated.
    fn path_candidates(&self,path_words:&[Vec<u8>],started:Instant,budget_ms:u64,
        cancel:&Cancellation)->Result<Option<(Vec<EntryIdx>,bool)>> {
        let mut selected:Option<([u8;3],usize)>=None;
        for word in path_words {
            for gram in word.windows(3).filter(|g|!g.iter().any(|b|matches!(*b,b'\\'|b'/'|b':'))) {
                let key=[gram[0],gram[1],gram[2]];
                let len=self.grams.get(&key).map_or(0,Vec::len);
                if selected.is_none_or(|(_,best_len)|len<best_len) { selected=Some((key,len)); }
            }
        }
        let Some((gram,_))=selected else {return Ok(None)};
        let posting=self.grams.get(&gram).map(Vec::as_slice).unwrap_or(&[]);
        let mut entries=Vec::new();
        let mut seen=HashSet::new();
        let mut work=0usize;
        let mut stopped=false;
        for idx in posting.iter().copied() {
            if !add_path_candidate(&self.index,idx,true,started,budget_ms,cancel,&mut work,&mut seen,&mut entries) {
                stopped=true;break
            }
        }
        if !stopped {
            for idx in self.long_names.iter().copied() {
                if work%64==0 && (cancel.is_cancelled() || started.elapsed().as_millis()>=budget_ms as u128) {
                    stopped=true;break
                }
                work+=1;
                if contains(self.index.entry(idx).folded_name(),&gram) &&
                    !add_path_candidate(&self.index,idx,true,started,budget_ms,cancel,&mut work,&mut seen,&mut entries) {
                    stopped=true;break
                }
            }
        }
        cancel.check()?;
        Ok(Some((entries,stopped)))
    }
    fn chain(&self, frn: Frn) -> Result<Vec<EntryIdx>> {
        let mut out=Vec::new();
        let mut current=self.index.head_of(frn).unwrap_or(NIL);
        while current != NIL {
            ensure!(out.len()<1024,"hard-link chain exceeds budget");
            out.push(current); current=self.index.entry(current).next_link();
        }
        Ok(out)
    }
    /// Remove affected names before the core changes/reuses their entry slots.
    /// All callers mutate a private generation; failure must never publish it.
    pub fn detach_names(&mut self, frns: &[Frn], cancel: &Cancellation) -> Result<()> {
        let mut affected=HashSet::new();
        let mut grams=HashSet::new();
        for frn in frns {
            cancel.check()?;
            for idx in self.chain(*frn)? {
                if self.index.is_synthetic(idx) || self.is_system_volume_information_entry(idx) { continue; }
                affected.insert(idx);
                let entry=self.index.entry(idx); let name=entry.folded_name();
                if name.len()<=66 {
                    grams.extend(name.windows(3).map(|g|[g[0],g[1],g[2]]));
                }
            }
        }
        // Scan each affected posting once per batch, rather than once per name.
        for g in grams {
            cancel.check()?;
            if let Some(list)=self.grams.get_mut(&g) { list.retain(|v| !affected.contains(v)); }
            if self.grams.get(&g).is_some_and(Vec::is_empty) { self.grams.remove(&g); }
        }
        self.long_names.retain(|v| !affected.contains(v));
        self.exe.retain(|v| !affected.contains(v)); self.workshop.retain(|v| !affected.contains(v));
        Ok(())
    }
    pub fn attach_names(&mut self, frns: &[Frn], directories_changed: bool, cancel: &Cancellation) -> Result<()> {
        for frn in frns {
            cancel.check()?;
            for idx in self.chain(*frn)? {
                if self.index.is_synthetic(idx) { continue; }
                let e=self.index.entry(idx); let name=e.folded_name();
                if !e.is_dir() && name.ends_with(b".exe") {
                    self.exe.push(idx);
                    if self.is_workshop(idx) { self.workshop.push(idx); }
                }
                if name.len()>66 { self.long_names.push(idx); continue; }
                let mut grams:Vec<_>=name.windows(3).map(|g|[g[0],g[1],g[2]]).collect();
                grams.sort_unstable(); grams.dedup();
                for g in grams { self.grams.entry(g).or_default().push(idx); }
            }
        }
        // Directory moves change descendant paths without changing their names.
        // Only the .exe posting needs reclassification, not every file/gram.
        if directories_changed {
            let mut workshop=Vec::new();
            for (n,idx) in self.exe.iter().copied().enumerate() {
                if n%256==0 { cancel.check()?; }
                if self.is_workshop(idx) { workshop.push(idx); }
            }
            self.workshop=workshop;
        }
        Ok(())
    }
    fn is_workshop(&self, idx: EntryIdx) -> bool {
        if !self.meta.drive.eq_ignore_ascii_case(&'C') { return false; }
        let mut current = idx;
        for _ in 0..4096 {
            let e = self.index.entry(current);
            if e.parent() == self.index.root() {
                return e.is_dir() && e.folded_name() == b"workshop";
            }
            if current == self.index.root() || self.index.is_synthetic(current) || e.parent() == current { return false; }
            current = e.parent();
        }
        false
    }
    pub fn query(&self, q: &Query, cancel: &Cancellation) -> Result<QueryResult> {
        cancel.check()?;
        ensure!((1..=400).contains(&q.limit), "query limit must be 1..400");
        ensure!((1..=100).contains(&q.budget_ms), "query budget must be 1..100 ms");
        ensure!(q.words.len() + q.path_words.len() + q.excludes.len() <= 16, "too many query terms");
        ensure!(q.words.iter().chain(&q.path_words).chain(&q.excludes).all(|w| w.len() <= 1024), "query term too long");
        let started = Instant::now();
        let words: Vec<_> = q.words.iter().map(|s| folded(s.as_bytes())).collect();
        let fold_path_term=|s:&str| {
            let mut value=folded(s.as_bytes());
            for byte in &mut value { if *byte==b'/' { *byte=b'\\'; } }
            value
        };
        let path_words: Vec<_> = q.path_words.iter().map(|s| fold_path_term(s)).collect();
        let excludes: Vec<_> = q.excludes.iter().map(|s| fold_path_term(s)).collect();
        let extension = q.extension.as_ref().map(|s| folded(format!(".{}", s.trim_start_matches('.')).as_bytes()));
        let mut selected: Option<&[EntryIdx]> = if q.workshop_only { Some(&self.workshop) }
            else if extension.as_deref() == Some(b".exe") { Some(&self.exe) } else { None };
        let mut needs_long = false;
        if !q.workshop_only {
            for term in &words {
                for g in term.windows(3) {
                    let list = self.grams.get(&[g[0],g[1],g[2]]).map(Vec::as_slice).unwrap_or(&[]);
                    if selected.is_none_or(|s| list.len() < s.len()) { selected = Some(list); needs_long = true; }
                }
            }
        }
        let (path_candidates,path_preparation_truncated)=if selected.is_none() && !path_words.is_empty() {
            match self.path_candidates(&path_words,started,q.budget_ms,cancel)? {
                Some((entries,truncated))=>(Some(entries),truncated),
                None=>(None,false),
            }
        } else {(None,false)};
        let candidates: Box<dyn Iterator<Item=EntryIdx> + '_> = if let Some(list)=path_candidates.as_ref() {
            Box::new(list.iter().copied())
        } else if let Some(list) = selected {
            Box::new(list.iter().copied())
        } else { Box::new(self.index.live_entries()) };
        let long: &[EntryIdx] = if needs_long { &self.long_names } else { &[] };
        let mut best: BinaryHeap<Reverse<Hit>> = BinaryHeap::with_capacity(q.limit + 1);
        let mut seen = 0u64; let mut truncated = path_preparation_truncated; let mut path = Vec::new();
        // Workshop is always evaluated before general candidates, independent of Everything.
        let priority: &[EntryIdx] = if q.workshop_only { &[] } else { &self.workshop };
        for idx in priority.iter().copied().chain(candidates).chain(long.iter().copied()) {
            seen += 1;
            if seen == 1 || seen % 64 == 0 {
                cancel.check()?;
                if started.elapsed().as_millis() >= q.budget_ms as u128 { truncated = true; break; }
            }
            if self.index.is_synthetic(idx) { continue; }
            let e = self.index.entry(idx);
            if e.is_dir() != q.folders_only { continue; }
            if !words.iter().all(|w| contains(e.folded_name(), w)) { continue; }
            if extension.as_ref().is_some_and(|x| !e.folded_name().ends_with(x)) { continue; }
            let workshop = self.is_workshop(idx) && e.folded_name().ends_with(b".exe");
            if q.workshop_only && !workshop { continue; }
            path.clear();
            if self.index.path_of(idx, &mut path) != PathStatus::Ok { continue; }
            if path_is_system_volume_information(&path) { continue; }
            let mut full_bytes = format!("{}:\\", self.meta.drive.to_ascii_uppercase()).into_bytes();
            full_bytes.extend(path.iter().map(|b| if *b == b'/' { b'\\' } else { *b }));
            let folded_path = if path_words.is_empty() && excludes.is_empty() { Vec::new() } else { folded(&full_bytes) };
            if excludes.iter().any(|x| contains(e.folded_name(), x) || contains(&folded_path, x)) { continue; }
            if !path_words.is_empty() {
                if !path_words.iter().all(|w| contains(&folded_path, w)) { continue; }
            }
            let path_utf16 = goz_core::wtf8::to_utf16(&full_bytes);
            let full = String::from_utf8_lossy(&full_bytes).into_owned();
            // The preliminary pass overlaps the general posting, but the bounded result set cannot contain duplicates.
            if best.iter().any(|h| h.0.path_utf16 == path_utf16) { continue; }
            let name_score = words.iter().map(|w| if e.folded_name() == w { 1000 } else if e.folded_name().starts_with(w) { 500 } else { 100 }).sum();
            let parent = full.rsplit_once('\\').map(|(p,_)| p).unwrap_or("");
            let parent_name = parent.rsplit('\\').next().unwrap_or("");
            let backup = parent_name.to_ascii_lowercase().contains("backup");
            let version = parent_name.split('-').find(|s| s.chars().next().is_some_and(|c| c.is_ascii_digit()))
                .map(|s| s.split('.').filter_map(|s| s.parse::<u64>().ok()).collect()).unwrap_or_default();
            let hit = Hit { path: full, path_utf16, is_folder: e.is_dir(), workshop, name_score, version, backup };
            if best.len() < q.limit { best.push(Reverse(hit)); }
            else if best.peek().is_some_and(|smallest| hit > smallest.0) { best.pop(); best.push(Reverse(hit)); }
        }
        let mut hits: Vec<_> = best.into_iter().map(|r| r.0).collect(); hits.sort_by(|a,b| b.cmp(a));
        Ok(QueryResult { hits, elapsed_us: started.elapsed().as_micros() as u64, candidates_examined: seen, truncated, stale: self.meta.stale })
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    #[test] fn api_paths_reconstruct_and_resolve_exact_parent_frns() {
        let c=fixture();
        let api_path=c.api_path_for_frn(Frn(201)).unwrap();
        assert_eq!(String::from_utf16(&api_path).unwrap(),r"C:\workshop\Aura-1.10.0\Aura.exe");
        let resolved=c.layout_name_for_api_link(&r"\workshop\Aura-1.10.0\Aura.exe".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(resolved.parent_frn,Frn(111));assert_eq!(resolved.name,b"Aura.exe");
        let old=c.layout_name_for_api_link(&r"\workshop-old\old.exe".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(old.parent_frn,Frn(101));
        let root=c.layout_name_for_api_link(&r"\root.exe".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(root.parent_frn,NTFS_ROOT_FRN);
    }
    #[test] fn directory_frns_are_unique_and_sorted_for_batch_reconciliation() {
        let c=fixture();
        let actual=c.directory_frns_sorted();
        assert!(actual.windows(2).all(|pair|pair[0].0<pair[1].0));
        assert!(actual.contains(&NTFS_ROOT_FRN));
        assert!(actual.contains(&Frn(100)) && actual.contains(&Frn(111)));
        assert!(!actual.contains(&Frn(201)));
        for idx in c.index.live_entries().filter(|idx|c.index.is_synthetic(*idx) && *idx!=c.index.root()) {
            assert!(!actual.contains(&c.index.entry(idx).frn()));
        }
    }
    #[test] fn api_link_resolution_preserves_case_sensitive_components_and_wtf8_names() {
        let mut c=fixture();let root=NTFS_ROOT_FRN.0;
        c.index.insert_layout(&layout(400,root,b"case",true));
        c.index.insert_layout(&layout(401,root,b"Case",true));
        let exact=c.layout_name_for_api_link(&r"\Case\app.exe".encode_utf16().collect::<Vec<_>>()).unwrap();
        assert_eq!(exact.parent_frn,Frn(401));
        let mut path=r"\workshop\".encode_utf16().collect::<Vec<_>>();path.push(0xD800);path.extend(".exe".encode_utf16());
        let resolved=c.layout_name_for_api_link(&path).unwrap();
        assert_eq!(resolved.parent_frn,Frn(100));assert!(resolved.name_lossy);
        assert_eq!(goz_core::wtf8::to_utf16(&resolved.name),path[r"\workshop\".encode_utf16().count()..]);
    }
    #[test] fn api_link_resolution_rejects_unindexed_or_traversal_parents() {
        let c=fixture();
        for path in [r"\missing\a.exe",r"\workshop\..\a.exe",r"\workshop\\a.exe",r"workshop\a.exe"] {
            assert!(c.layout_name_for_api_link(&path.encode_utf16().collect::<Vec<_>>()).is_err(),"{path}");
        }
    }
    #[test] fn protected_system_volume_information_is_excluded_but_external_hardlinks_remain() {
        let mut c=Catalog::empty(VolumeMeta {identity:"fixture".into(),drive:'C',journal_id:1,next_usn:1,stale:false});
        let root=NTFS_ROOT_FRN.0;
        c.index.insert_layout(&layout(300,root,b"System Volume Information",true));
        c.index.insert_layout(&layout(301,300,b"WPSettings.dat",false));
        c.index.insert_layout(&layout(302,300,b"secret.exe",false));
        let mut linked=layout(303,300,b"linked.exe",false);
        linked.names.push(LayoutName {parent_frn:Frn(root),name:b"linked-outside.exe".to_vec(),name_lossy:false,dos_only:false});
        c.index.insert_layout(&linked);
        assert_eq!(String::from_utf16(&c.api_path_for_frn(Frn(303)).unwrap()).unwrap(),r"C:\linked-outside.exe");
        let protected_frns=c.protected_system_metadata_frns();
        assert!(protected_frns.contains(&Frn(301)) && protected_frns.contains(&Frn(302)));
        assert!(!protected_frns.contains(&Frn(303)));
        c.rebuild_query_index(&Cancellation::default()).unwrap();
        let protected=c.query(&Query {words:vec!["wpsettings".into()],..Default::default()},&Cancellation::default()).unwrap();
        assert!(protected.hits.is_empty());
        let executables=c.query(&Query {extension:Some("exe".into()),limit:20,..Default::default()},&Cancellation::default()).unwrap();
        assert!(executables.hits.iter().all(|h|!h.path.contains("System Volume Information")));
        assert!(executables.hits.iter().any(|h|h.path.ends_with("linked-outside.exe")));
        assert!(!executables.hits.iter().any(|h|h.path.ends_with("linked.exe")));
    }
    pub fn fixture() -> Catalog {
        let mut c = Catalog::empty(VolumeMeta { identity: "fixture".into(), drive: 'C', journal_id: 1, next_usn: 1, stale: false });
        let root = NTFS_ROOT_FRN.0;
        for f in [layout(100,root,b"workshop",true), layout(101,root,b"workshop-old",true),
            layout(110,100,b"Aura-1.2.0",true), layout(111,100,b"Aura-1.10.0",true),
            layout(112,100,b"Aura-1.10.0-backup",true), layout(200,110,b"Aura.exe",false),
            layout(201,111,b"Aura.exe",false), layout(202,112,b"Aura.exe",false),
            layout(203,101,b"Aura.exe",false), layout(204,100,b"Aura.txt",false)] { c.index.insert_layout(&f); }
        c.rebuild_query_index(&Cancellation::default()).unwrap(); c
    }
    #[test] fn workshop_boundary_extension_version_and_top_k() {
        let c = fixture();
        let r = c.query(&Query { words: vec!["aura".into()], workshop_only: true, limit: 2, ..Default::default() }, &Cancellation::default()).unwrap();
        assert_eq!(r.hits.len(),2); assert!(r.hits.iter().all(|h| h.workshop && h.path.ends_with(".exe")));
        assert!(r.hits[0].path.contains("Aura-1.10.0\\")); assert!(!r.hits[0].backup);
        assert!(r.hits.iter().all(|h| !h.path.contains("workshop-old")));
    }
    #[test] fn path_only_search_uses_component_postings_and_expands_matching_directories() {
        let c=fixture();
        let workshop=c.query(&Query {path_words:vec![r"C:\workshop\".into()],limit:20,..Default::default()},&Cancellation::default()).unwrap();
        assert_eq!(workshop.hits.len(),4);
        assert!(workshop.hits.iter().all(|hit|hit.path.to_ascii_lowercase().contains("workshop\\")));
        assert!(!workshop.hits.iter().any(|hit|hit.path.contains("workshop-old")));
        let version=c.query(&Query {path_words:vec!["Aura-1.10.0".into()],limit:20,..Default::default()},&Cancellation::default()).unwrap();
        assert_eq!(version.hits.len(),2);
        assert!(version.hits.iter().all(|hit|hit.path.contains("Aura-1.10.0")));
    }
    #[test] fn cancellation_precedes_query() {
        let c = fixture(); let cancel = Cancellation::default(); cancel.cancel();
        assert!(c.query(&Query::default(),&cancel).is_err());
    }
    #[test] fn long_names_remain_searchable_and_unicode_casefolds() {
        let mut c = fixture(); let name = format!("{}微信日报.EXE", "a".repeat(100));
        c.index.insert_layout(&layout(300,100,name.as_bytes(),false));
        c.rebuild_query_index(&Cancellation::default()).unwrap();
        let r = c.query(&Query { words: vec!["微信日报".into()], ..Default::default() },&Cancellation::default()).unwrap();
        assert_eq!(r.hits.len(),1);
    }
}
