//! Streaming, checksummed snapshots: bounded buffers, no second full copy of the index.
use crate::{catalog::{Catalog, VolumeMeta}, policy::Cancellation};
use anyhow::{Context, Result, ensure};
use goz_core::{layout::{LayoutFile, LayoutName}, types::{Frn, NIL}};
use std::{fs::{self, File}, io::{BufReader, BufWriter, Read, Write}, path::Path, time::SystemTime};

const MAGIC: &[u8; 8] = b"FTNTFS01";
const PARTIAL_MAGIC: &[u8; 8] = b"FTPART01";
const MAX_BYTES: u64 = 512 * 1024 * 1024;
// The 4m-record nested-hardlink fixture peaks above 512 MiB while the
// VolumeIndex is being assembled, then compacts to about 464 MiB. Snapshot
// loading remains bounded by the reloader's machine-wide 1 GiB commit-growth
// guard and 3 GiB available-memory floor.
const MAX_INDEX_ALLOCATED_BYTES: u64 = 768 * 1024 * 1024;
const MAX_QUERY_POSTING_BYTES: u64 = 256 * 1024 * 1024;
const MAX_CHECKPOINT_STATE: usize = 128 * 1024 * 1024;
const MAX_RECORDS: u64 = 8_000_000;
#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize)]
pub struct FileStamp { pub len: u64, pub created: SystemTime, pub modified: SystemTime }
/// Identifies a published snapshot generation without reading its contents.
/// Atomic replacement changes at least its modification time or file length.
pub fn file_stamp(path:&Path)->Result<FileStamp> {
    let metadata=fs::metadata(path).with_context(||format!("read snapshot metadata {}",path.display()))?;
    ensure!(metadata.is_file(),"snapshot path is not a regular file");
    Ok(FileStamp{len:metadata.len(),created:metadata.created().context("snapshot creation time unavailable")?,
        modified:metadata.modified().context("snapshot modification time unavailable")?})
}
struct HashWrite<W> { inner: W, crc: crc32fast::Hasher, bytes: u64 }
impl<W: Write> HashWrite<W> {
    fn put(&mut self, bytes: &[u8]) -> Result<()> {
        let next = self.bytes.checked_add(bytes.len() as u64).context("snapshot size overflow")?;
        ensure!(next <= MAX_BYTES - 4, "snapshot exceeds 512 MiB while writing");
        self.inner.write_all(bytes)?;
        self.crc.update(bytes);
        self.bytes = next;
        Ok(())
    }
}
struct HashRead<R> { inner: R, crc: crc32fast::Hasher }
impl<R: Read> HashRead<R> {
    fn take(&mut self, n: usize) -> Result<Vec<u8>> {
        self.take_bounded(n,4096)
    }
    fn take_bounded(&mut self,n:usize,maximum:usize)->Result<Vec<u8>> {
        ensure!(n <= maximum, "snapshot field exceeds its byte budget");
        let mut bytes = vec![0; n]; self.inner.read_exact(&mut bytes)?; self.crc.update(&bytes); Ok(bytes)
    }
    fn u64(&mut self) -> Result<u64> { Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap())) }
}
pub fn save(c: &Catalog, path: &Path, cancel: &Cancellation) -> Result<()> {
    save_internal(c,path,cancel,None)
}
/// Atomically persist an unpublished partial generation and its provider cursor.
/// The distinct magic prevents query hosts from serving this file as a snapshot.
pub fn save_partial(c:&Catalog,path:&Path,state:&serde_json::Value,cancel:&Cancellation)->Result<()> {
    save_internal(c,path,cancel,Some(state))
}
fn save_internal(c:&Catalog,path:&Path,cancel:&Cancellation,state:Option<&serde_json::Value>)->Result<()> {
    let tmp = path.with_extension("tmp");
    let result = (|| {
        let file = File::create(&tmp)?;
        let mut w = HashWrite { inner: BufWriter::new(file), crc: crc32fast::Hasher::new(), bytes: 0 };
        w.put(if state.is_some(){PARTIAL_MAGIC}else{MAGIC})?;
        let meta = serde_json::to_vec(&c.meta)?;
        ensure!(meta.len() <= 4096, "snapshot metadata exceeds budget");
        w.put(&(meta.len() as u32).to_le_bytes())?; w.put(&meta)?;
        if let Some(state)=state {
            ensure!(c.meta.stale,"partial generations must stay stale/unpublished");
            let bytes=serde_json::to_vec(state)?;
            ensure!(!bytes.is_empty() && bytes.len()<=MAX_CHECKPOINT_STATE,"provider checkpoint state exceeds byte budget");
            w.put(&(bytes.len() as u32).to_le_bytes())?;w.put(&bytes)?;
        }
        let is_head = |idx| !c.index.is_synthetic(idx) && c.index.head_of(c.index.entry(idx).frn()) == Some(idx);
        let count = c.index.live_entries().filter(|&idx| is_head(idx)).count() as u64;
        ensure!(count <= MAX_RECORDS, "snapshot record budget exceeded");
        w.put(&count.to_le_bytes())?;
        for (n, idx) in c.index.live_entries().enumerate() {
            if n % 2048 == 0 { cancel.check()?; }
            if !is_head(idx) { continue; }
            let e = c.index.entry(idx);
            ensure!(e.name().len() <= 4096, "snapshot name exceeds budget");
            w.put(&e.frn().0.to_le_bytes())?;
            w.put(&[u8::from(e.is_dir())])?;
            let mut names = Vec::new(); let mut link = idx;
            while link != NIL {
                ensure!(names.len() < 1024, "snapshot hard-link budget exceeded");
                names.push(link); link = c.index.entry(link).next_link();
            }
            w.put(&(names.len() as u16).to_le_bytes())?;
            for link in names {
                let name = c.index.entry(link);
                ensure!(name.name().len() <= 4096, "snapshot name exceeds budget");
                w.put(&c.index.entry(name.parent()).frn().0.to_le_bytes())?;
                w.put(&(name.name().len() as u16).to_le_bytes())?; w.put(name.name())?;
            }
        }
        let crc = w.crc.finalize(); w.inner.write_all(&crc.to_le_bytes())?;
        w.inner.flush()?; w.inner.get_ref().sync_all()?; drop(w.inner);
        ensure!(fs::metadata(&tmp)?.len() <= MAX_BYTES, "snapshot byte budget exceeded");
        cancel.check()?;
        atomic_replace(&tmp, path)?;
        Ok(())
    })();
    if result.is_err() { let _ = fs::remove_file(&tmp); }
    result
}
#[cfg(windows)]
fn atomic_replace(from: &Path, to: &Path) -> Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH};
    let f: Vec<_> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let t: Vec<_> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    // SAFETY: both paths are null terminated and remain alive for the entire call.
    ensure!(unsafe { MoveFileExW(f.as_ptr(), t.as_ptr(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) } != 0,
        "publish snapshot failed: {}", std::io::Error::last_os_error());
    Ok(())
}
pub fn load(path: &Path, cancel: &Cancellation) -> Result<Catalog> {
    load_internal(path,cancel,false,true,MAX_INDEX_ALLOCATED_BYTES,MAX_QUERY_POSTING_BYTES).map(|(c,_)|c)
}
/// Load an offline capacity fixture with separate index and posting budgets.
/// Production query hosts continue through `load` with the machine-guarded limits.
pub(crate) fn load_with_budgets(path:&Path,cancel:&Cancellation,max_index_bytes:u64,max_query_index_bytes:u64)->Result<Catalog> {
    load_internal(path,cancel,false,true,max_index_bytes,max_query_index_bytes).map(|(c,_)|c)
}
pub fn load_partial(path:&Path,cancel:&Cancellation)->Result<(Catalog,serde_json::Value)> {
    let (c,state)=load_internal(path,cancel,true,false,MAX_INDEX_ALLOCATED_BYTES,MAX_QUERY_POSTING_BYTES)?;
    Ok((c,state.context("partial snapshot omitted its provider checkpoint")?))
}
fn load_internal(path:&Path,cancel:&Cancellation,partial:bool,rebuild_query_index:bool,max_index_bytes:u64,max_query_index_bytes:u64)->Result<(Catalog,Option<serde_json::Value>)> {
    ensure!(fs::metadata(path)?.len() <= MAX_BYTES, "snapshot exceeds byte budget");
    let mut r = HashRead { inner: BufReader::new(File::open(path)?), crc: crc32fast::Hasher::new() };
    ensure!(r.take(8)? == if partial {PARTIAL_MAGIC}else{MAGIC}, "snapshot kind/header mismatch");
    let len = u32::from_le_bytes(r.take(4)?.try_into().unwrap()) as usize;
    let meta: VolumeMeta = serde_json::from_slice(&r.take(len)?).context("snapshot metadata")?;
    ensure!(meta.drive.is_ascii_alphabetic() && meta.identity.len() <= 512, "invalid volume identity");
    let state=if partial {
        ensure!(meta.stale,"partial snapshot is unexpectedly marked queryable");
        let n=u32::from_le_bytes(r.take(4)?.try_into().unwrap()) as usize;
        ensure!(n>0,"empty partial provider checkpoint");
        Some(serde_json::from_slice(&r.take_bounded(n,MAX_CHECKPOINT_STATE)?).context("partial provider checkpoint")?)
    }else{None};
    let count = r.u64()?;
    ensure!(count <= MAX_RECORDS, "snapshot record count exceeds budget");
    let mut c = Catalog::empty(meta);
    for n in 0..count {
        if n % 2048 == 0 {
            cancel.check()?;
            ensure!(c.index.memory().total().allocated <= max_index_bytes,"snapshot index allocation budget exceeded");
        }
        let frn = Frn(r.u64()?);
        let flags = r.take(1)?[0]; ensure!(flags <= 1, "invalid snapshot flags");
        let count = u16::from_le_bytes(r.take(2)?.try_into().unwrap()) as usize;
        ensure!((1..=1024).contains(&count), "invalid snapshot hard-link count");
        let mut names = Vec::with_capacity(count);
        for _ in 0..count {
            let parent_frn = Frn(r.u64()?);
            let length = u16::from_le_bytes(r.take(2)?.try_into().unwrap()) as usize;
            let name = r.take(length)?;
            ensure!(!name.is_empty() && !name.contains(&0) && !name.contains(&b'\\') && !name.contains(&b'/'), "invalid component in snapshot");
            let name_lossy = std::str::from_utf8(&name).is_err();
            names.push(LayoutName { parent_frn, name, name_lossy, dos_only: false });
        }
        c.index.insert_layout(&LayoutFile { frn, attributes: if flags == 1 { 0x10 } else { 0 }, size: None, mtime_ft: None, names });
    }
    let mut check = [0;4]; r.inner.read_exact(&mut check)?;
    ensure!(u32::from_le_bytes(check) == r.crc.finalize(), "snapshot checksum mismatch");
    let mut trailing = [0]; ensure!(r.inner.read(&mut trailing)? == 0, "trailing data in snapshot");
    c.index.optimize_storage(); if rebuild_query_index {c.rebuild_query_index_limited(cancel,max_query_index_bytes)?;}
    Ok((c,state))
}

#[cfg(test)] mod tests {
    use super::*;
    use crate::catalog::{Query, layout};
    use goz_core::index::NTFS_ROOT_FRN;
    #[test] fn snapshot_writer_enforces_size_budget_before_writing_overflow() {
        let mut writer = HashWrite { inner: Vec::new(), crc: crc32fast::Hasher::new(), bytes: MAX_BYTES - 8 };
        writer.put(&[1, 2, 3, 4]).unwrap();
        assert_eq!(writer.inner.len(), 4);
        assert!(writer.put(&[5; 5]).is_err());
        assert_eq!(writer.inner.len(), 4, "over-budget bytes must not reach the temporary snapshot");
    }
    #[test] fn file_stamp_changes_when_a_new_generation_is_published() {
        let dir=std::env::temp_dir().join(format!("flowtary-snapshot-stamp-{}",std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path=dir.join("index.fti");
        fs::write(&path,b"first generation").unwrap();
        let before=file_stamp(&path).unwrap();
        let replacement=dir.join("index.fti.tmp");
        fs::write(&replacement,b"second snapshot generation").unwrap();
        fs::rename(&replacement,&path).unwrap();
        let after=file_stamp(&path).unwrap();
        assert_ne!(before,after,"atomic replacement must be distinguishable even when a publisher reuses the path");
        fs::remove_file(path).unwrap();fs::remove_dir(dir).unwrap();
    }
    #[test] fn round_trip_preserves_hardlinks_and_corruption_is_rejected() {
        let dir = std::env::temp_dir().join(format!("flowtary-snapshot-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap(); let p = dir.join("index.fti");
        let cancel = Cancellation::default();
        let mut c = Catalog::empty(VolumeMeta { identity: "test".into(), drive: 'C', journal_id: 7, next_usn: 99, stale: false });
        let root = NTFS_ROOT_FRN.0;
        c.index.insert_layout(&layout(100,root,b"workshop",true));
        let mut f = layout(200,100,b"app.exe",false); f.names.push(goz_core::layout::LayoutName {
            parent_frn: goz_core::types::Frn(root), name: b"app-linked.exe".to_vec(), name_lossy: false, dos_only: false });
        c.index.insert_layout(&f); c.rebuild_query_index(&cancel).unwrap();
        save(&c,&p,&cancel).unwrap(); let loaded = load(&p,&cancel).unwrap();
        assert_eq!(loaded.meta.journal_id,7);
        let r = loaded.query(&Query { words: vec!["app".into()], ..Default::default() },&cancel).unwrap();
        assert_eq!(r.hits.len(),2); assert!(r.hits[0].workshop);
        let before = fs::read(&p).unwrap(); let mut corrupt = before.clone(); corrupt[24] ^= 1; fs::write(&p,&corrupt).unwrap();
        assert!(load(&p,&cancel).is_err()); fs::write(&p,&before).unwrap();
        cancel.cancel(); assert!(save(&c,&p,&cancel).is_err()); assert_eq!(fs::read(&p).unwrap(),before);
        fs::remove_file(p).unwrap(); fs::remove_dir(dir).unwrap();
    }
}
