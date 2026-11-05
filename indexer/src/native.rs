//! Experimental provider. VM tests are the default. Explicitly authorized local
//! collection tests use small file-backed disks; physical volumes permit only
//! an explicitly authorized bounded metadata probe.
use crate::{catalog::VolumeMeta, collector::MetadataSource, metrics::Sample, policy::{Cancellation, native_host_violations}};
use anyhow::{Context, Result, bail, ensure};
use goz_core::{index::{LinkTarget,WtfName},layout::{LayoutFile,LayoutName, QUERY_FILE_LAYOUT_INCLUDE_NAMES, walk_layout_buffer}, types::Frn,
    usn::{JournalInfo, JournalWalk, walk_journal_buffer}};
use goz_winfs::{JournalQuery, VolumeHandle};
use serde::{Deserialize, Serialize};
use std::{path::Path, ptr::null_mut, sync::{Arc, Mutex, atomic::{AtomicBool, AtomicU64, Ordering}}, thread, time::{Duration, Instant}};
use windows_sys::Win32::{Storage::FileSystem::{GetVolumeInformationW,GetVolumeNameForVolumeMountPointW,GetDiskFreeSpaceExW},
    System::Registry::{HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ, RegGetValueW}};

#[derive(Debug, Serialize, Deserialize)]
pub struct Manifest {
    pub schema: u32,
    pub machine_name: String,
    pub rollback_snapshot: String,
    pub volume_guid: String,
    pub drive: char,
    pub volume_label: String,
    #[serde(default)] pub authorized_host_test:bool,
    #[serde(default)] pub backing_vhd:Option<String>,
    #[serde(default)] pub volume_serial:Option<u32>,
    /// Explicit test-only switch to exercise the compatibility provider on a fresh host VHD.
    #[serde(default)] pub force_enum_usn:bool,
    /// Test-only switch for directory-batched hard-link reconciliation.
    #[serde(default)] pub directory_link_walk:bool,
}
#[derive(Clone,Copy,PartialEq,Eq)]
pub enum TestMode { VirtualMachine, AuthorizedHostVhd, AuthorizedHostVolumeProbe }
static VOLUME_OPEN_ATTEMPTS: AtomicU64 = AtomicU64::new(0);
pub fn volume_open_attempts() -> u64 { VOLUME_OPEN_ATTEMPTS.load(Ordering::Acquire) }
fn registry_string(name: &str) -> Result<String> {
    let sub: Vec<u16> = "HARDWARE\\DESCRIPTION\\System\\BIOS".encode_utf16().chain(Some(0)).collect();
    let field: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
    let mut data = [0u16;512]; let mut bytes = size_of_val(&data) as u32;
    // SAFETY: buffers and UTF-16 strings are valid for the complete synchronous call.
    let status = unsafe { RegGetValueW(HKEY_LOCAL_MACHINE,sub.as_ptr(),field.as_ptr(),RRF_RT_REG_SZ,
        null_mut(),data.as_mut_ptr() as _,&mut bytes) };
    ensure!(status == 0,"read BIOS identity failed: {status}");
    let n = data.iter().position(|v| *v == 0).unwrap_or(data.len());
    Ok(String::from_utf16_lossy(&data[..n]))
}
fn is_guest(manufacturer: &str, model: &str) -> bool {
    let m = manufacturer.to_ascii_lowercase(); let p = model.to_ascii_lowercase();
    (m == "microsoft corporation" && p.contains("virtual machine")) || m.contains("vmware") ||
        m.contains("qemu") || m.contains("innotek") || p.contains("virtualbox") || p.contains("kvm")
}
struct Permit { manifest: Manifest }
fn environment_checks(manifest:&Manifest,sample:&Sample,mode:TestMode,manufacturer:&str,model:&str)->Result<()> {
    ensure!(!manifest.force_enum_usn || mode==TestMode::AuthorizedHostVhd,
        "forced USN fallback mode is only allowed for a verified host VHD");
    ensure!(!manifest.directory_link_walk || (mode==TestMode::AuthorizedHostVhd && manifest.force_enum_usn),
        "directory-batched hard-link reconciliation is restricted to the forced USN provider on a verified host VHD");
    if mode==TestMode::VirtualMachine {
        ensure!(manifest.schema==1,"unsupported isolation manifest");
        ensure!(!manifest.rollback_snapshot.trim().is_empty(),"rollback snapshot reference is required");
        ensure!(is_guest(manufacturer,model),"native collection refused on a physical host ({manufacturer}, {model}); explicit local VHD testing requires --host-test-vhd");
        let violations=native_host_violations(sample);
        ensure!(violations.is_empty(),"native collection refused by machine-resource preflight: {}",violations.join("; "));
    } else {
        let schema=if mode==TestMode::AuthorizedHostVhd {2}else{3};
        ensure!(manifest.schema==schema && manifest.authorized_host_test,"local test requires explicit authorization in its matching manifest schema");
        ensure!(manifest.volume_serial.is_some(),"local test requires the volume serial");
        if mode==TestMode::AuthorizedHostVhd {ensure!(manifest.backing_vhd.is_some(),"local VHD test requires backing-file identity");}
        ensure!(sample.counter_errors.is_empty() && sample.system_handles.is_some(),"required host telemetry is unavailable");
        ensure!(sample.physical_available_bytes>=1536*1024*1024 && sample.commit_ratio<=0.85,"local test has insufficient memory/commit headroom");
    }
    Ok(())
}
fn preflight(manifest: Manifest, sample: &Sample, mode:TestMode) -> Result<Permit> {
    ensure!(manifest.machine_name.eq_ignore_ascii_case(&std::env::var("COMPUTERNAME").unwrap_or_default()),"manifest belongs to another machine");
    let manufacturer = registry_string("SystemManufacturer")?;
    let model = registry_string("SystemProductName")?;
    environment_checks(&manifest,sample,mode,&manufacturer,&model)?;
    ensure!(goz_winfs::is_elevated(),"native test collection requires an elevated session");
    if mode!=TestMode::AuthorizedHostVolumeProbe {ensure!(manifest.volume_label == "FLOWTARY_TEST","only FLOWTARY_TEST labelled volumes are eligible for collection");}
    ensure!(manifest.drive.is_ascii_alphabetic() && (mode==TestMode::AuthorizedHostVolumeProbe || !manifest.drive.eq_ignore_ascii_case(&'C')),"collection test volume must not be the system drive");
    let guid = &manifest.volume_guid;
    ensure!(guid.is_ascii() && guid.starts_with("\\\\?\\Volume{") && guid.ends_with("}\\") && guid.len() == 49,
        "a canonical volume GUID path is required, not a drive root or arbitrary device");
    ensure!(guid[11..47].bytes().all(|b| b.is_ascii_hexdigit() || b == b'-'),"invalid volume GUID");
    let wide: Vec<_> = guid.encode_utf16().chain(Some(0)).collect();
    let mut label = [0u16;128]; let mut filesystem = [0u16;128]; let mut serial=0;
    // SAFETY: checked GUID path and fixed-size output buffers; optional outputs may be null.
    let ok = unsafe { GetVolumeInformationW(wide.as_ptr(),label.as_mut_ptr(),label.len() as u32,
        &mut serial,null_mut(),null_mut(),filesystem.as_mut_ptr(),filesystem.len() as u32) };
    ensure!(ok != 0,"read test volume identity failed: {}",std::io::Error::last_os_error());
    let decode = |x: &[u16]| String::from_utf16_lossy(&x[..x.iter().position(|v| *v == 0).unwrap_or(x.len())]);
    ensure!(decode(&label) == manifest.volume_label && decode(&filesystem) == "NTFS","test volume label/filesystem mismatch");
    let mount:Vec<u16>=format!("{}:\\",manifest.drive).encode_utf16().chain(Some(0)).collect();
    let mut actual=[0u16;64];
    // SAFETY: canonical mount string and fixed writable GUID output buffer.
    ensure!(unsafe{GetVolumeNameForVolumeMountPointW(mount.as_ptr(),actual.as_mut_ptr(),actual.len() as u32)}!=0 && decode(&actual).eq_ignore_ascii_case(guid),"drive letter does not map to the declared volume GUID");
    if mode!=TestMode::VirtualMachine {ensure!(manifest.volume_serial==Some(serial),"local test volume serial mismatch");}
    if mode==TestMode::AuthorizedHostVhd {
        let backing=std::path::PathBuf::from(manifest.backing_vhd.as_ref().unwrap());
        ensure!(backing.is_absolute() && backing.extension().is_some_and(|e|e.eq_ignore_ascii_case("vhd") || e.eq_ignore_ascii_case("vhdx")),"local test requires an absolute VHD/VHDX backing path");
        let metadata=std::fs::metadata(&backing)?;
        ensure!(metadata.is_file() && metadata.len()<=256*1024*1024,"test backing file exceeds 256 MiB");
        let mut total=0u64;
        // SAFETY: valid volume GUID and writable total-size output; optional outputs are null.
        ensure!(unsafe{GetDiskFreeSpaceExW(wide.as_ptr(),null_mut(),&mut total,null_mut())}!=0 && total<=256*1024*1024,"local test volume must be at most 256 MiB");
    }
    Ok(Permit { manifest })
}

struct IoGuard {
    base: Instant,
    deadline: Arc<AtomicU64>,
    done: Arc<AtomicBool>,
    monitor: Option<thread::JoinHandle<()>>,
}
struct IoRequest(Arc<AtomicU64>);
impl Drop for IoRequest { fn drop(&mut self) { self.0.store(0,Ordering::Release); } }
impl IoGuard {
    fn new(cancel: Cancellation) -> Result<Self> {
        let handle = goz_winfs::current_thread_io_handle()?;
        let base = Instant::now(); let deadline = Arc::new(AtomicU64::new(0)); let done = Arc::new(AtomicBool::new(false));
        let dl = deadline.clone(); let quit = done.clone();
        let monitor = thread::spawn(move || {
            while !quit.load(Ordering::Acquire) {
                let limit = dl.load(Ordering::Acquire);
                if limit != 0 && base.elapsed().as_millis() as u64 >= limit { cancel.cancel(); }
                if cancel.is_cancelled() { let _ = goz_winfs::cancel_synchronous_io(&handle); }
                thread::sleep(Duration::from_millis(100));
            }
        });
        Ok(Self { base,deadline,done,monitor:Some(monitor) })
    }
    fn begin(&self) -> IoRequest {
        self.deadline.store(self.base.elapsed().as_millis() as u64 + 5000,Ordering::Release);
        IoRequest(self.deadline.clone())
    }
}
impl Drop for IoGuard {
    fn drop(&mut self) { self.done.store(true,Ordering::Release); if let Some(t)=self.monitor.take() { let _=t.join(); } }
}
pub struct NativeSource {
    handle: VolumeHandle,
    drive: char,
    buf: Vec<u8>,
    guard: IoGuard,
    cancel: Cancellation,
    enum_layout_active:bool,
    enum_next_frn:u64,
    enum_started:Option<Instant>,
    enum_link_cursor:Option<u64>,
    enum_link_requests:usize,
    enum_links_complete:bool,
    directory_link_walk:bool,
    directory_link_current:Option<u64>,
    capture_file_record_payload_on_error:bool,
    host_vhd_test:bool,
    io_stats: Arc<Mutex<NativeIoStats>>,
    // The cancellation handle belongs to the creating thread; do not permit cross-thread moves.
    same_thread: std::marker::PhantomData<std::rc::Rc<()>>,
}
/// Operation counts and wall time from the actual native provider. Machine-wide
/// counters alone cannot reveal per-record kernel call fan-out.
#[derive(Clone, Copy, Debug, Default, Serialize)]
pub struct NativeIoStats {
    pub file_record_requests: u64,
    pub file_record_elapsed_us: u64,
    pub file_id_open_requests: u64,
    pub file_id_open_elapsed_us: u64,
    pub hardlink_name_requests:u64,
    pub hardlink_name_elapsed_us:u64,
    pub usn_enum_page_requests: u64,
    pub usn_enum_elapsed_us: u64,
    pub usn_journal_read_requests: u64,
    pub usn_journal_read_elapsed_us: u64,
    pub initial_ntfs_metadata_records_skipped:u64,
    pub initial_protected_system_records_skipped:u64,
    pub layout_requests: u64,
    pub layout_elapsed_us: u64,
    pub directory_batch_requests:u64,
    pub directory_batch_elapsed_us:u64,
    pub directory_handles_opened:u64,
    pub directory_handles_active:u64,
    pub directory_handles_peak:u64,
    pub directory_entries_seen:u64,
    pub directory_reparse_points_skipped:u64,
}
struct TrackedDirectoryHandle {
    file:std::fs::File,
    stats:Arc<Mutex<NativeIoStats>>,
}
impl TrackedDirectoryHandle {
    fn new(file:std::fs::File,stats:Arc<Mutex<NativeIoStats>>)->Self {
        {
            let mut state=stats.lock().unwrap_or_else(|poisoned|poisoned.into_inner());
            state.directory_handles_opened=state.directory_handles_opened.saturating_add(1);
            state.directory_handles_active=state.directory_handles_active.saturating_add(1);
            state.directory_handles_peak=state.directory_handles_peak.max(state.directory_handles_active);
        }
        Self {file,stats}
    }
    fn raw_handle(&self)->windows_sys::Win32::Foundation::HANDLE {
        use std::os::windows::io::AsRawHandle;
        self.file.as_raw_handle() as _
    }
}
impl Drop for TrackedDirectoryHandle {
    fn drop(&mut self) {
        let mut state=self.stats.lock().unwrap_or_else(|poisoned|poisoned.into_inner());
        state.directory_handles_active=state.directory_handles_active.saturating_sub(1);
    }
}
struct DirectoryBatchItem { file_id:u64, attributes:u32, name:Vec<u16> }
fn decode_file_id_both_dir_buffer(buffer:&[u8])->Result<Vec<DirectoryBatchItem>> {
    use windows_sys::Win32::Storage::FileSystem::FILE_ID_BOTH_DIR_INFO;
    let header=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileName);
    let attr_offset=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileAttributes);
    let name_len_offset=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileNameLength);
    let file_id_offset=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileId);
    let mut out=Vec::new();let mut offset=0usize;
    loop {
        ensure!(offset.checked_add(header).is_some_and(|end|end<=buffer.len()),"truncated FILE_ID_BOTH_DIR_INFO header");
        let next=u32::from_le_bytes(buffer[offset..offset+4].try_into().unwrap()) as usize;
        let attributes=u32::from_le_bytes(buffer[offset+attr_offset..offset+attr_offset+4].try_into().unwrap());
        let name_bytes=u32::from_le_bytes(buffer[offset+name_len_offset..offset+name_len_offset+4].try_into().unwrap()) as usize;
        ensure!(name_bytes>0 && name_bytes%2==0 && header.checked_add(name_bytes).is_some_and(|n|n<=buffer.len()-offset),
            "invalid FILE_ID_BOTH_DIR_INFO filename length");
        if next!=0 {ensure!(next%8==0 && next>=header+name_bytes && next<=buffer.len()-offset,"invalid FILE_ID_BOTH_DIR_INFO next-entry offset");}
        let file_id=i64::from_le_bytes(buffer[offset+file_id_offset..offset+file_id_offset+8].try_into().unwrap()) as u64;
        let name=buffer[offset+header..offset+header+name_bytes].chunks_exact(2)
            .map(|unit|u16::from_le_bytes([unit[0],unit[1]])).collect();
        out.push(DirectoryBatchItem{file_id,attributes,name});
        if next==0 {break}
        offset+=next;
    }
    Ok(out)
}
/// Durable provider state for the cursor-based USN plus path-hardlink provider.
/// FILE_LAYOUT itself has no equivalent restart token and is never represented here.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct NativeResumeState {
    pub schema:u32,
    pub next_frn:u64,
    pub elapsed_ms:u64,
    pub link_cursor:Option<u64>,
    pub link_requests:u64,
    pub links_complete:bool,
    #[serde(default)] pub directory_link_walk:bool,
    #[serde(default)] pub current_directory:Option<u64>,
}
impl NativeSource {
    pub fn io_stats_handle(&self) -> Arc<Mutex<NativeIoStats>> { self.io_stats.clone() }
    /// Enable a one-record diagnostic dump only for an explicitly authorized
    /// file-backed test VHD probe. Collection and physical-volume probes never
    /// expose raw FILE record bytes.
    pub fn enable_file_record_payload_diagnostic(&mut self)->Result<()> {
        ensure!(self.host_vhd_test,"raw FILE record diagnostics are restricted to the authorized test VHD");
        self.capture_file_record_payload_on_error=true;
        Ok(())
    }
    fn enumerate_hardlink_paths(&mut self,path:&[u16])->Result<Vec<Vec<u16>>> {
        use windows_sys::Win32::{Foundation::{GetLastError,INVALID_HANDLE_VALUE,ERROR_HANDLE_EOF},
            Storage::FileSystem::{FindFirstFileNameW,FindNextFileNameW,FindClose}};
        self.cancel.check()?;
        ensure!(path.len()<=32767 && !path.is_empty() && !path.contains(&0),"invalid/oversized hard-link API input path");
        let mut wide=path.to_vec();wide.push(0);
        let started=Instant::now();
        self.io_stats.lock().unwrap().hardlink_name_requests+=1;
        let _request=self.guard.begin();
        let result=(||->Result<Vec<Vec<u16>>>{
            let mut name=vec![0u16;32768];let mut length=name.len() as u32;
            // SAFETY: the null-terminated input and fixed-capacity output buffer remain alive for the synchronous call.
            let raw=unsafe{FindFirstFileNameW(wide.as_ptr(),0,&mut length,name.as_mut_ptr())};
            ensure!(raw!=INVALID_HANDLE_VALUE,"FindFirstFileNameW failed: {}",std::io::Error::last_os_error());
            struct SearchHandle(windows_sys::Win32::Foundation::HANDLE);
            impl Drop for SearchHandle {fn drop(&mut self){unsafe{FindClose(self.0);}}}
            let handle=SearchHandle(raw);
            let mut names=Vec::new();
            loop {
                self.cancel.check()?;
                ensure!(names.len()<64,"hard-link API exceeded its 64-name per-file budget");
                let n=name.iter().position(|u|*u==0).unwrap_or(length.min(name.len() as u32) as usize);
                ensure!(n>0 && n<=32767,"hard-link API returned an invalid name length");
                names.push(name[..n].to_vec());
                length=name.len() as u32;
                // SAFETY: owned search handle and bounded writable name buffer are valid.
                if unsafe{FindNextFileNameW(handle.0,&mut length,name.as_mut_ptr())}==0 {
                    let error=unsafe{GetLastError()};
                    ensure!(error==ERROR_HANDLE_EOF,"FindNextFileNameW failed: {error}");
                    break;
                }
            }
            Ok(names)
        })();
        let mut stats=self.io_stats.lock().unwrap();
        stats.hardlink_name_elapsed_us=stats.hardlink_name_elapsed_us.saturating_add(started.elapsed().as_micros() as u64);
        result
    }
    /// Resolve every hard-link name for an already-indexed file. The supplied
    /// path is checked against the API's returned volume-relative link set so
    /// a reparse traversal to a different target fails closed.
    pub fn file_layout_by_path_api(&mut self,catalog:&crate::catalog::Catalog,frn:Frn)->Result<LayoutFile> {
        let path=catalog.api_path_for_frn(frn)?;
        ensure!(path.len()>=3,"resolved API path has no drive root");
        let expected=&path[2..];
        let returned=self.enumerate_hardlink_paths(&path).with_context(||format!(
            "enumerate hard-link names for FRN {:#018X} at {}",frn.0,String::from_utf16_lossy(&path)))?;
        ensure!(!returned.is_empty() && returned.iter().any(|name|name.as_slice()==expected),
            "hard-link API output does not contain the indexed input path; possible path race or reparse redirection");
        let mut seen=std::collections::HashSet::new();let mut names=Vec::with_capacity(returned.len());
        for link in returned {
            self.cancel.check()?;
            let resolved=catalog.layout_name_for_api_link(&link)?;
            ensure!(seen.insert((resolved.parent_frn,resolved.name.clone())),"hard-link API returned a duplicate name");
            names.push(resolved);
        }
        let idx=catalog.index.head_of(frn).ok_or_else(||anyhow::anyhow!("file disappeared from the private catalog"))?;
        let entry=catalog.index.entry(idx);
        ensure!(!entry.is_dir() && !catalog.index.is_synthetic(idx),"path-based hard-link reconciliation requires a real file");
        Ok(LayoutFile {frn,attributes:0,size:entry.size(),mtime_ft:entry.mtime(),names})
    }
    /// Bounded probe of the documented path-based NTFS hard-link enumeration API.
    /// The caller must provide one explicit file path from an authorized test volume.
    pub fn probe_hardlink_names(&mut self,path:&str)->Result<serde_json::Value> {
        use std::os::windows::ffi::OsStrExt;
        let wide:Vec<u16>=std::ffi::OsStr::new(path).encode_wide().chain(Some(0)).collect();
        ensure!(wide.len()<=32768,"hard-link probe path exceeds the Windows path buffer limit");
        let names=self.enumerate_hardlink_paths(&wide[..wide.len()-1])?;
        let names:Vec<_>=names.iter().map(|name|String::from_utf16_lossy(name)).collect();
        Ok(serde_json::json!({"path":path,"links":names,"link_count":names.len(),"api":"FindFirstFileNameW/FindNextFileNameW"}))
    }
    /// Read one authorized test-VHD directory through FileIdBothDirectoryInfo.
    /// This measures batched file-ID/name enumeration without changing the
    /// production provider. Reparse directories are rejected and never walked.
    pub fn probe_directory_batch(&mut self,relative:&str,entry_budget:usize)->Result<serde_json::Value> {
        use std::{collections::HashSet,os::windows::io::FromRawHandle};
        use windows_sys::Win32::{Foundation::{ERROR_NO_MORE_FILES,INVALID_HANDLE_VALUE},Storage::FileSystem::{
            CreateFileW,GetFileInformationByHandleEx,FILE_ATTRIBUTE_REPARSE_POINT,FILE_ATTRIBUTE_TAG_INFO,
            FILE_FLAG_BACKUP_SEMANTICS,FILE_FLAG_OPEN_REPARSE_POINT,FILE_ID_INFO,
            FILE_LIST_DIRECTORY,FILE_READ_ATTRIBUTES,FILE_SHARE_DELETE,FILE_SHARE_READ,FILE_SHARE_WRITE,
            FileAttributeTagInfo,FileIdBothDirectoryInfo,FileIdBothDirectoryRestartInfo,OPEN_EXISTING}};
        ensure!(self.host_vhd_test,"directory batch probe is restricted to an authorized test VHD");
        ensure!((1..=100_000).contains(&entry_budget),"directory batch probe requires a 1..100,000 aggregate entry budget");
        ensure!(relative.len()<=4096 && !relative.is_empty() && !relative.starts_with(['\\','/']) && !relative.contains([':', '*', '?']),
            "directory batch probe requires a bounded relative path");
        ensure!(relative.split(['\\','/']).all(|part|!part.is_empty() && part!="." && part!=".."),
            "directory batch probe path contains an invalid component");
        self.cancel.check()?;
        let path=format!("{}:\\{}",self.drive,relative.replace('/',"\\"));
        let wide:Vec<u16>=path.encode_utf16().chain(Some(0)).collect();
        let started=Instant::now();
        let _open_request=self.guard.begin();
        let raw=unsafe{CreateFileW(wide.as_ptr(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,null_mut(),OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,null_mut())};
        ensure!(raw!=INVALID_HANDLE_VALUE,"open test directory for batched enumeration failed: {}",std::io::Error::last_os_error());
        // SAFETY: CreateFileW returned one owned synchronous HANDLE; File closes it on every exit path.
        let directory=TrackedDirectoryHandle::new(unsafe{std::fs::File::from_raw_handle(raw as _)},self.io_stats.clone());
        drop(_open_request);
        let handle=directory.raw_handle();
        let mut tag=FILE_ATTRIBUTE_TAG_INFO::default();
        let _attribute_request=self.guard.begin();
        ensure!(unsafe{GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&mut tag as *mut _ as _,std::mem::size_of_val(&tag) as u32)}!=0,
            "query test directory attributes failed: {}",std::io::Error::last_os_error());
        drop(_attribute_request);
        ensure!(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT==0,"directory batch probe refuses reparse points");
        let mut identity=FILE_ID_INFO::default();
        let _identity_request=self.guard.begin();
        ensure!(unsafe{GetFileInformationByHandleEx(handle,windows_sys::Win32::Storage::FileSystem::FileIdInfo,&mut identity as *mut _ as _,std::mem::size_of_val(&identity) as u32)}!=0,
            "query directory FileId failed: {}",std::io::Error::last_os_error());
        drop(_identity_request);
        ensure!(identity.FileId.Identifier[8..].iter().all(|b|*b==0),"NTFS directory ID is not a zero-extended 64-bit file reference");
        let directory_file_id=u64::from_le_bytes(identity.FileId.Identifier[..8].try_into().unwrap());
        let mut buffer=vec![0u64;8192];
        let mut first=true;let mut requests=0u64;let mut batches=0u64;let mut entries=0u64;let mut files=0u64;let mut directories=0u64;
        let mut reparses=0u64;let mut unique=HashSet::new();let mut duplicate_ids=0u64;let mut examples=Vec::new();
        loop {
            self.cancel.check()?;
            ensure!(started.elapsed()<Duration::from_secs(30),"directory batch probe exceeded its 30-second limit");
            ensure!(entries<entry_budget as u64,"directory batch probe exceeded the aggregate entry limit");
            buffer.fill(0);
            let class=if first{FileIdBothDirectoryRestartInfo}else{FileIdBothDirectoryInfo};
            let call_started=Instant::now();
            let _request=self.guard.begin();
            requests+=1;
            let ok=unsafe{GetFileInformationByHandleEx(handle,class,buffer.as_mut_ptr() as _,(buffer.len()*8) as u32)};
            drop(_request);
            {let mut stats=self.io_stats.lock().unwrap();stats.directory_batch_requests+=1;
                stats.directory_batch_elapsed_us=stats.directory_batch_elapsed_us.saturating_add(call_started.elapsed().as_micros() as u64);}
            if ok==0 {
                let error=unsafe{windows_sys::Win32::Foundation::GetLastError()};
                if error==ERROR_NO_MORE_FILES{break}
                anyhow::bail!("batched directory enumeration failed: {error}")
            }
            first=false;batches+=1;
            let bytes=unsafe{std::slice::from_raw_parts(buffer.as_ptr() as *const u8,buffer.len()*8)};
            let mut batch_entries=0u64;
            for item in decode_file_id_both_dir_buffer(bytes)? {
                if item.name.as_slice()==[b'.' as u16] || item.name.as_slice()==[b'.' as u16,b'.' as u16] {continue}
                ensure!(entries<entry_budget as u64,"directory batch probe exceeded the aggregate entry limit");
                entries+=1;
                batch_entries+=1;
                if item.attributes&0x10!=0 {directories+=1}else{files+=1}
                if item.attributes&FILE_ATTRIBUTE_REPARSE_POINT!=0 {reparses+=1}
                if !unique.insert(item.file_id){duplicate_ids+=1;}
                if examples.len()<16 {examples.push(serde_json::json!({"file_id":item.file_id,"attributes":item.attributes,
                    "name":String::from_utf16_lossy(&item.name)}));}
            }
            self.io_stats.lock().unwrap().directory_entries_seen+=batch_entries;
        }
        let elapsed_us=started.elapsed().as_micros() as u64;
        Ok(serde_json::json!({"relative_directory":relative,"directory_file_id":directory_file_id,"entries":entries,
            "files":files,"directories":directories,"reparse_entries_not_followed":reparses,"unique_file_ids":unique.len(),
            "repeated_file_ids_within_directory":duplicate_ids,"api_requests":requests,"data_batches":batches,"elapsed_us":elapsed_us,
            "entries_per_second":entries as f64/(elapsed_us as f64/1_000_000.0).max(0.000001),"sample_entries":examples,
            "api":"GetFileInformationByHandleEx(FileIdBothDirectoryInfo)"}))
    }
    /// Bounded read-only directory-tree walk for the authorized test VHD.
    /// It opens one directory at a time and never opens ordinary files. IDs
    /// are collected from the batched directory records so repeated IDs expose
    /// hard links without per-file path API calls.
    pub fn probe_directory_tree(&mut self,max_entries:usize,max_directories:usize,max_depth:u32,max_seconds:u64)->Result<serde_json::Value> {
        use std::{collections::HashSet,os::windows::io::FromRawHandle};
        use windows_sys::Win32::{Foundation::{ERROR_NO_MORE_FILES,INVALID_HANDLE_VALUE},Storage::FileSystem::{
            CreateFileW,GetFileInformationByHandleEx,FILE_ATTRIBUTE_DIRECTORY,FILE_ATTRIBUTE_REPARSE_POINT,
            FILE_ATTRIBUTE_TAG_INFO,FILE_FLAG_BACKUP_SEMANTICS,FILE_FLAG_OPEN_REPARSE_POINT,FILE_ID_INFO,
            FILE_LIST_DIRECTORY,FILE_READ_ATTRIBUTES,FILE_SHARE_DELETE,FILE_SHARE_READ,FILE_SHARE_WRITE,
            FileAttributeTagInfo,FileIdBothDirectoryInfo,FileIdBothDirectoryRestartInfo,OPEN_EXISTING}};
        ensure!(self.host_vhd_test,"directory-tree probe is restricted to an authorized test VHD");
        ensure!((1..=100_000).contains(&max_entries),"directory-tree entry budget must be 1..100,000");
        ensure!((1..=10_000).contains(&max_directories),"directory-tree directory budget must be 1..10,000");
        ensure!((1..=32).contains(&max_depth),"directory-tree depth must be 1..32");
        ensure!((1..=30).contains(&max_seconds),"directory-tree time limit must be 1..30 seconds");
        self.cancel.check()?;
        let root=format!("{}:\\",self.drive);
        let started=Instant::now();
        let mut pending=vec![(root.clone(),0u32)];
        let mut seen_files=HashSet::<u64>::new();
        let mut seen_directories=HashSet::<u64>::new();
        let mut entries=0usize;let mut files=0usize;let mut directory_entries=0usize;let mut directories_opened=0usize;
        let mut reparse_directories=0usize;let mut protected_directories=0usize;
        let mut repeated_file_ids=0usize;let mut depth_limited_directories=0usize;
        let mut batches=0u64;let mut requests=0u64;let mut complete=true;
        while let Some((path,depth))=pending.pop() {
            self.cancel.check()?;
            if started.elapsed()>=Duration::from_secs(max_seconds) || directories_opened>=max_directories {
                complete=false;break
            }
            ensure!(path.encode_utf16().count()<32767,"directory-tree path exceeded the Win32 path limit");
            let wide:Vec<u16>=path.encode_utf16().chain(Some(0)).collect();
            let _open_request=self.guard.begin();
            // SAFETY: path is a bounded NUL-terminated volume-relative path;
            // this read-only directory handle is immediately wrapped in RAII.
            let raw=unsafe{CreateFileW(wide.as_ptr(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,null_mut(),OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,null_mut())};
            ensure!(raw!=INVALID_HANDLE_VALUE,"open test directory for tree enumeration failed at {path}: {}",std::io::Error::last_os_error());
            // SAFETY: CreateFileW returned one uniquely owned synchronous handle.
            let directory=TrackedDirectoryHandle::new(unsafe{std::fs::File::from_raw_handle(raw as _)},self.io_stats.clone());
            drop(_open_request);
            let handle=directory.raw_handle();
            let mut tag=FILE_ATTRIBUTE_TAG_INFO::default();
            let _tag_request=self.guard.begin();
            ensure!(unsafe{GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&mut tag as *mut _ as _,std::mem::size_of_val(&tag) as u32)}!=0,
                "query test directory attributes failed at {path}: {}",std::io::Error::last_os_error());
            drop(_tag_request);
            ensure!(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY!=0 && tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT==0,
                "directory-tree walk refused a non-directory/reparse path: {path}");
            let mut identity=FILE_ID_INFO::default();
            let _id_request=self.guard.begin();
            ensure!(unsafe{GetFileInformationByHandleEx(handle,windows_sys::Win32::Storage::FileSystem::FileIdInfo,&mut identity as *mut _ as _,std::mem::size_of_val(&identity) as u32)}!=0,
                "query test directory FileId failed at {path}: {}",std::io::Error::last_os_error());
            drop(_id_request);
            ensure!(identity.FileId.Identifier[8..].iter().all(|b|*b==0),"NTFS directory ID is not a zero-extended 64-bit file reference");
            let directory_id=u64::from_le_bytes(identity.FileId.Identifier[..8].try_into().unwrap());
            ensure!(seen_directories.insert(directory_id),"directory-tree walk found a repeated directory FileId at {path}");
            directories_opened+=1;
            let mut buffer=vec![0u64;8192];let mut first=true;
            loop {
                self.cancel.check()?;
                if started.elapsed()>=Duration::from_secs(max_seconds) || entries>=max_entries {complete=false;break}
                buffer.fill(0);
                let class=if first{FileIdBothDirectoryRestartInfo}else{FileIdBothDirectoryInfo};
                let call_started=Instant::now();let _request=self.guard.begin();requests+=1;
                // SAFETY: the directory handle and initialized 64-KiB output buffer
                // remain valid for the complete synchronous call.
                let ok=unsafe{GetFileInformationByHandleEx(handle,class,buffer.as_mut_ptr() as _,(buffer.len()*8) as u32)};
                drop(_request);
                {let mut stats=self.io_stats.lock().unwrap();stats.directory_batch_requests+=1;
                    stats.directory_batch_elapsed_us=stats.directory_batch_elapsed_us.saturating_add(call_started.elapsed().as_micros() as u64);}
                if ok==0 {
                    let error=unsafe{windows_sys::Win32::Foundation::GetLastError()};
                    if error==ERROR_NO_MORE_FILES {break}
                    anyhow::bail!("batched directory-tree enumeration failed at {path}: {error}")
                }
                first=false;batches+=1;
                let bytes=unsafe{std::slice::from_raw_parts(buffer.as_ptr() as *const u8,buffer.len()*8)};
                let mut batch_entries=0u64;
                for item in decode_file_id_both_dir_buffer(bytes)? {
                    if item.name.as_slice()==[b'.' as u16] || item.name.as_slice()==[b'.' as u16,b'.' as u16] {continue}
                    if entries>=max_entries {complete=false;break}
                    entries+=1;
                    batch_entries+=1;
                    let is_directory=item.attributes&FILE_ATTRIBUTE_DIRECTORY!=0;
                    let is_reparse=item.attributes&FILE_ATTRIBUTE_REPARSE_POINT!=0;
                    if is_directory {directory_entries+=1;}else{files+=1;}
                    if is_directory {
                        if is_reparse {reparse_directories+=1;self.io_stats.lock().unwrap().directory_reparse_points_skipped+=1;continue}
                        let name=String::from_utf16(&item.name).context("directory-tree entry name is not valid UTF-16")?;
                        if depth==0 && name.eq_ignore_ascii_case("System Volume Information") {
                            protected_directories+=1;continue
                        }
                        if depth>=max_depth {depth_limited_directories+=1;complete=false;continue}
                        if pending.len()+directories_opened>=max_directories {complete=false;continue}
                        pending.push((format!("{}{}\\",path,name),depth+1));
                    } else if !seen_files.insert(item.file_id) {repeated_file_ids+=1;}
                }
                self.io_stats.lock().unwrap().directory_entries_seen+=batch_entries;
                if !complete {break}
            }
            drop(directory);
            if !complete {break}
        }
        if !pending.is_empty(){complete=false}
        let elapsed_us=started.elapsed().as_micros() as u64;
        Ok(serde_json::json!({"complete":complete,"entries":entries,"files":files,"directory_entries":directory_entries,
            "directories_opened":directories_opened,
            "reparse_directories_not_followed":reparse_directories,"protected_system_directories_skipped":protected_directories,
            "depth_limited_directories":depth_limited_directories,"unique_file_ids":seen_files.len(),
            "repeated_file_ids_hardlinks":repeated_file_ids,"api_requests":requests,"data_batches":batches,
            "elapsed_us":elapsed_us,"entries_per_second":entries as f64/(elapsed_us as f64/1_000_000.0).max(0.000001),
            "directory_handles_peak":1,"ordinary_file_handles_opened":0,"entry_budget":max_entries,
            "directory_budget":max_directories,"max_depth":max_depth,"time_budget_seconds":max_seconds,
            "api":"GetFileInformationByHandleEx(FileIdBothDirectoryInfo)"}))
    }
    pub fn resume_state(&self)->Option<NativeResumeState> {
        (self.enum_layout_active && self.enum_next_frn>0).then(||NativeResumeState {
            schema:if self.directory_link_walk {3}else{2},next_frn:self.enum_next_frn,
            elapsed_ms:self.enum_started.map_or(0,|t|t.elapsed().as_millis() as u64),
            link_cursor:self.enum_link_cursor,link_requests:self.enum_link_requests as u64,
            links_complete:self.enum_links_complete,
            directory_link_walk:self.directory_link_walk,current_directory:self.directory_link_current,
        })
    }
    pub fn restore_resume_state(&mut self,state:NativeResumeState)->Result<()> {
        ensure!(self.enum_next_frn==0,"provider resume state can only be restored before the first FRN page is consumed");
        ensure!((state.schema==2 || state.schema==3) && state.next_frn>0 && state.link_requests<=1_000_000 && state.elapsed_ms<86_400_000,
            "invalid or expired native enumeration checkpoint");
        ensure!(state.directory_link_walk==self.directory_link_walk &&
            (state.schema==3 || (!state.directory_link_walk && state.current_directory.is_none())),
            "initial checkpoint hard-link provider does not match the selected provider");
        self.enum_layout_active=true;self.enum_next_frn=state.next_frn;
        self.enum_link_cursor=state.link_cursor;
        self.enum_link_requests=state.link_requests as usize;self.enum_links_complete=state.links_complete;
        self.directory_link_current=state.current_directory;
        // Deadlines are per invocation; durable link_cursor makes continuation
        // safe without carrying an expired process-local timer across restarts.
        self.enum_started=Some(Instant::now());
        Ok(())
    }
    pub fn for_test(path: &Path, sample: &Sample, cancel: Cancellation,mode:TestMode) -> Result<(Self,VolumeMeta)> {
        let manifest: Manifest = serde_json::from_slice(&std::fs::read(path)?).context("read isolation manifest")?;
        let permit = preflight(manifest,sample,mode)?;
        let force_enum_usn=permit.manifest.force_enum_usn;
        let directory_link_walk=permit.manifest.directory_link_walk;
        cancel.check()?;
        let guard = IoGuard::new(cancel.clone())?;
        let request = guard.begin();
        VOLUME_OPEN_ATTEMPTS.fetch_add(1,Ordering::Release);
        let handle = goz_winfs::open_volume(&permit.manifest.volume_guid)?;
        if mode==TestMode::AuthorizedHostVhd {
            let query=[0u32;3];let mut descriptor=[0u8;1024];let mut returned=0u32;
            // SAFETY: owned synchronous volume handle, STORAGE_PROPERTY_QUERY input,
            // and bounded writable STORAGE_DEVICE_DESCRIPTOR output buffer.
            let ok=unsafe{windows_sys::Win32::System::IO::DeviceIoControl(handle.raw() as _,
                windows_sys::Win32::System::Ioctl::IOCTL_STORAGE_QUERY_PROPERTY,query.as_ptr() as _,12,
                descriptor.as_mut_ptr() as _,descriptor.len() as u32,&mut returned,null_mut())};
            ensure!(ok!=0 && returned>=32,"could not verify local test storage bus: {}",std::io::Error::last_os_error());
            let bus=u32::from_le_bytes(descriptor[28..32].try_into().unwrap());
            ensure!(bus==15,"local host tests require a file-backed virtual disk (bus 15), got {bus}");
        }
        drop(request);
        let meta = VolumeMeta { identity:permit.manifest.volume_guid,drive:permit.manifest.drive,
            journal_id:0,next_usn:0,stale:true };
        let buffer_bytes=match mode {TestMode::VirtualMachine=>4*1024*1024,TestMode::AuthorizedHostVhd=>512*1024,TestMode::AuthorizedHostVolumeProbe=>256*1024};
        Ok((Self { handle,drive:permit.manifest.drive,buf:vec![0;buffer_bytes],guard,cancel,
            enum_layout_active:force_enum_usn,enum_next_frn:0,enum_started:force_enum_usn.then(Instant::now),
            enum_link_cursor:None,enum_link_requests:0,enum_links_complete:false,directory_link_walk,directory_link_current:None,
            capture_file_record_payload_on_error:false,
            host_vhd_test:mode==TestMode::AuthorizedHostVhd,
            io_stats:Arc::new(Mutex::new(NativeIoStats::default())),same_thread:std::marker::PhantomData },meta))
    }
}
impl NativeSource {
    /// Probe whether NTFS will expose the `$MFT` stream through a file-ID open.
    /// Reads only the first FILE record and is never used by normal collection.
    pub fn probe_mft_stream_by_file_id(&mut self) -> Result<serde_json::Value> {
        use std::os::windows::io::{AsRawHandle, FromRawHandle};
        use windows_sys::Win32::Storage::FileSystem::{
            FILE_FLAG_BACKUP_SEMANTICS, FILE_ID_DESCRIPTOR, FILE_ID_DESCRIPTOR_0, FileIdType,
            FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, OpenFileById,
        };
        use windows_sys::Win32::Foundation::{GetLastError, GENERIC_READ};
        use windows_sys::Win32::Storage::FileSystem::ReadFile;
        self.cancel.check()?;
        let fragment=self.mft_name_fragment(Frn(0))?.context("NTFS did not return allocated MFT record 0")?;
        ensure!(fragment.is_base && fragment.record.record_number()==0,"FRN 0 resolved to an unexpected FILE record");
        let descriptor=FILE_ID_DESCRIPTOR { dwSize:std::mem::size_of::<FILE_ID_DESCRIPTOR>() as u32,
            Type:FileIdType, Anonymous:FILE_ID_DESCRIPTOR_0 { FileId:fragment.record.0 as i64 } };
        let started=Instant::now();
        self.io_stats.lock().unwrap().file_id_open_requests+=1;
        let _request=self.guard.begin();
        // SAFETY: volume handle and FILE_ID_DESCRIPTOR remain valid during the
        // synchronous call; access is read-only and sharing permits concurrent use.
        let raw=unsafe { OpenFileById(self.handle.raw() as _,&descriptor,GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,null_mut(),FILE_FLAG_BACKUP_SEMANTICS) };
        let open_error=if raw==windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE {Some(unsafe{GetLastError()})}else{None};
        {let mut stats=self.io_stats.lock().unwrap();stats.file_id_open_elapsed_us=stats.file_id_open_elapsed_us.saturating_add(started.elapsed().as_micros() as u64);}
        let Some(open_error)=open_error else {
            // SAFETY: OpenFileById returned one owned file HANDLE.
            let file=unsafe {std::fs::File::from_raw_handle(raw as _)};
            let mut bytes=vec![0u8;64*1024];let mut read=0u32;
            // SAFETY: the owned file handle is synchronous and the output is a
            // fixed 64 KiB writable buffer; only the first MFT stream block is read.
            ensure!(unsafe{ReadFile(file.as_raw_handle() as _,bytes.as_mut_ptr(),bytes.len() as u32,&mut read,null_mut())}!=0,
                "read through MFT file-ID handle failed: {}",std::io::Error::last_os_error());
            self.cancel.check()?;
            ensure!(read>=48,"MFT file-ID read returned too few bytes: {read}");
            ensure!(&bytes[..4]==b"FILE","MFT file-ID stream does not begin with a FILE record");
            let record_bytes=u32::from_le_bytes(bytes[28..32].try_into().unwrap()) as usize;
            ensure!((512..=65536).contains(&record_bytes) && record_bytes.is_power_of_two() && record_bytes<=read as usize,
                "MFT file-ID stream has an invalid/short first FILE record: {record_bytes}/{read}");
            let decoded=crate::mft_names::decode(&bytes[..record_bytes],0)?.context("MFT stream record 0 is unallocated")?;
            ensure!(decoded.is_base && decoded.record.record_number()==0,"MFT stream first record has an unexpected owner");
            return Ok(serde_json::json!({"opened":true,"frn":fragment.record.0,"stream_size_bytes":file.metadata()?.len(),
                "bytes_read":read,"record_bytes":record_bytes,"first_record_valid":true,
                "names":decoded.names.len(),"fixup":"strict_raw"}));
        };
        Ok(serde_json::json!({"opened":false,"frn":fragment.record.0,"win32_error":open_error}))
    }

    /// Read NTFS geometry and FILE record 0 directly from the volume device.
    /// This is intentionally a one-record compatibility probe, not a scanner.
    pub fn probe_mft_stream_record_zero(&mut self) -> Result<crate::mft_names::Fragment> {
        use windows_sys::Win32::Storage::FileSystem::{ReadFile, SetFilePointerEx, FILE_BEGIN};
        use windows_sys::Win32::System::Ioctl::FSCTL_GET_NTFS_VOLUME_DATA;
        self.cancel.check()?;
        let _request = self.guard.begin();
        let mut geometry = [0u8; 96];
        let mut returned = 0u32;
        // SAFETY: FSCTL writes the documented fixed NTFS_VOLUME_DATA_BUFFER
        // into this 96-byte initialized output buffer.
        let ok = unsafe { windows_sys::Win32::System::IO::DeviceIoControl(
            self.handle.raw() as _, FSCTL_GET_NTFS_VOLUME_DATA, null_mut(), 0,
            geometry.as_mut_ptr() as _, geometry.len() as u32, &mut returned, null_mut()) };
        ensure!(ok != 0 && returned >= 80, "FSCTL_GET_NTFS_VOLUME_DATA failed: {}", std::io::Error::last_os_error());
        let i64_at = |at: usize| i64::from_le_bytes(geometry[at..at + 8].try_into().unwrap());
        let u32_at = |at: usize| u32::from_le_bytes(geometry[at..at + 4].try_into().unwrap());
        let sectors = i64_at(8);
        let bytes_per_sector = u32_at(40);
        let bytes_per_cluster = u32_at(44);
        let record_bytes = u32_at(48);
        let mft_lcn = i64_at(64);
        ensure!(sectors > 0 && (512..=4096).contains(&bytes_per_sector) && bytes_per_sector.is_power_of_two(), "invalid NTFS sector geometry");
        ensure!(bytes_per_cluster >= bytes_per_sector && bytes_per_cluster <= 2 * 1024 * 1024 && bytes_per_cluster.is_power_of_two(), "invalid NTFS cluster geometry");
        ensure!((512..=65536).contains(&record_bytes) && record_bytes.is_power_of_two() && record_bytes % bytes_per_sector == 0, "invalid NTFS file-record size");
        ensure!(mft_lcn >= 0, "invalid NTFS MFT starting LCN");
        let offset = (mft_lcn as u64).checked_mul(bytes_per_cluster as u64).context("MFT offset overflow")?;
        let volume_bytes = (sectors as u64).checked_mul(bytes_per_sector as u64).context("volume size overflow")?;
        ensure!(offset.checked_add(record_bytes as u64).is_some_and(|end| end <= volume_bytes), "MFT record 0 lies outside volume bounds");

        // Raw volume reads are rejected by some Windows filter stacks. Probe
        // the NTFS metadata stream through its file namespace; it
        // still reads one bounded record and never walks ordinary directories.
        use std::os::windows::io::{AsRawHandle,FromRawHandle};
        let path: Vec<u16> = format!(r"\\?\{}:\$MFT", self.drive).encode_utf16().chain(Some(0)).collect();
        // SAFETY: canonical drive-derived path, read-only access, shared open,
        // and a null security/template handle.
        let raw_handle = unsafe { windows_sys::Win32::Storage::FileSystem::CreateFileW(
            path.as_ptr(), 0x8000_0000u32,
            windows_sys::Win32::Storage::FileSystem::FILE_SHARE_READ | windows_sys::Win32::Storage::FileSystem::FILE_SHARE_WRITE | windows_sys::Win32::Storage::FileSystem::FILE_SHARE_DELETE,
            null_mut(), windows_sys::Win32::Storage::FileSystem::OPEN_EXISTING,
            windows_sys::Win32::Storage::FileSystem::FILE_FLAG_BACKUP_SEMANTICS, null_mut()) };
        ensure!(raw_handle != windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE,
            "open NTFS $MFT stream failed: {}", std::io::Error::last_os_error());
        // SAFETY: successful CreateFileW returned a unique owned HANDLE.
        let mft_file = unsafe { std::fs::File::from_raw_handle(raw_handle as _) };
        let mut position = 0i64;
        // SAFETY: owned synchronous $MFT handle and valid output pointer.
        ensure!(unsafe { SetFilePointerEx(mft_file.as_raw_handle() as _, 0, &mut position, FILE_BEGIN) } != 0,
            "seek within NTFS $MFT stream failed: {}", std::io::Error::last_os_error());
        ensure!(position == 0, "$MFT stream seek returned an unexpected offset");
        let mut record = vec![0u8; record_bytes as usize];
        let mut read = 0u32;
        // SAFETY: the seek is bounded by validated volume geometry; record is
        // writable for exactly its declared size, and the call is synchronous.
        ensure!(unsafe { ReadFile(mft_file.as_raw_handle() as _, record.as_mut_ptr(), record.len() as u32, &mut read, null_mut()) } != 0,
            "$MFT stream record read failed: {}", std::io::Error::last_os_error());
        self.cancel.check()?;
        ensure!(read == record.len() as u32, "short read of MFT record 0: {read}/{}", record.len());
        let fragment = crate::mft_names::decode(&record, 0)?.context("MFT record 0 is unallocated/empty")?;
        ensure!(fragment.is_base && fragment.record.record_number() == 0, "decoded MFT record 0 has an unexpected owner");
        Ok(fragment)
    }


    pub fn mft_name_fragment(&mut self,frn:Frn)->Result<Option<crate::mft_names::Fragment>> {
        self.cancel.check()?;let _request=self.guard.begin();let started=Instant::now();
        self.io_stats.lock().unwrap().file_record_requests+=1;
        let record=frn.record_number();let mut returned=0u32;
        // SAFETY: owned volume handle, NTFS_FILE_RECORD_INPUT_BUFFER (one u64),
        // bounded output buffer; this requests metadata for one explicit file ID.
        let ok=unsafe{windows_sys::Win32::System::IO::DeviceIoControl(self.handle.raw() as _,
            windows_sys::Win32::System::Ioctl::FSCTL_GET_NTFS_FILE_RECORD,&record as *const u64 as _,8,
            self.buf.as_mut_ptr() as _,self.buf.len() as u32,&mut returned,null_mut())};
        {let mut stats=self.io_stats.lock().unwrap();stats.file_record_elapsed_us=stats.file_record_elapsed_us.saturating_add(started.elapsed().as_micros() as u64);}
        ensure!(ok!=0,"FSCTL_GET_NTFS_FILE_RECORD failed: {}",std::io::Error::last_os_error());
        self.cancel.check()?;
        ensure!(returned>=12 && returned as usize<=self.buf.len(),"invalid FILE record ioctl output length");
        let actual=u64::from_le_bytes(self.buf[..8].try_into().unwrap());
        ensure!(Frn(actual).record_number()==record,"requested FILE record changed or vanished");
        let bytes=u32::from_le_bytes(self.buf[8..12].try_into().unwrap()) as usize;
        ensure!(bytes<=65536 && 12+bytes<=returned as usize,"FILE record payload exceeds output buffer");
        let payload=&self.buf[12..12+bytes];
        let parsed=match crate::mft_names::decode_fsctl(payload,record) {
            Ok(parsed)=>parsed,
            Err(error) if self.capture_file_record_payload_on_error && payload.len()<=4096 => {
                let mut hex=String::with_capacity(payload.len()*2);
                for byte in payload { use std::fmt::Write as _; let _=write!(hex,"{byte:02x}"); }
                anyhow::bail!("{error}; bounded_test_vhd_record_payload_hex={hex}")
            }
            Err(error)=>return Err(error),
        };
        if let Some(fragment)=&parsed {
            // Sequence zero is used only by the bounded record-zero probe to
            // discover the current full reference before OpenFileById. For all
            // enumerated FRNs, retain strict sequence validation.
            ensure!(fragment.record.record_number()==record && (frn.0>>48==0 || fragment.record==frn),
                "FILE record sequence changed since enumeration");
        }
        Ok(parsed)
    }
    pub fn enum_names_page(&mut self,next_frn:u64)->Result<Option<(u64,Vec<goz_core::usn::ParsedUsnRecord>)>> {
        self.cancel.check()?;let _request=self.guard.begin();let started=Instant::now();
        self.io_stats.lock().unwrap().usn_enum_page_requests+=1;
        let result=goz_winfs::enum_usn_data(&self.handle,next_frn,&mut self.buf);
        {let mut stats=self.io_stats.lock().unwrap();stats.usn_enum_elapsed_us=stats.usn_enum_elapsed_us.saturating_add(started.elapsed().as_micros() as u64);}
        let Some(n)=result? else{return Ok(None)};
        self.cancel.check()?;
        let parsed=walk_journal_buffer(&self.buf[..n])?;
        ensure!(parsed.skipped.unknown_version==0,"unknown MFT enumeration record version");
        ensure!(n>=8,"MFT enumeration page is missing its continuation cursor");
        let next=u64::from_le_bytes(self.buf[..8].try_into().unwrap());
        ensure!(next>next_frn,"MFT enumeration made no forward progress");
        Ok(Some((next,parsed.records)))
    }
    fn enum_layout_page(&mut self)->Result<Option<Vec<LayoutFile>>> {
        self.cancel.check()?;
        let started=self.enum_started.context("USN enumeration was not initialized")?;
        ensure!(started.elapsed()<Duration::from_secs(120),"USN enumeration exceeded its 120-second per-run budget");
        let Some((next,records))=self.enum_names_page(self.enum_next_frn)? else {
            ensure!(self.enum_next_frn>0,"USN enumeration reached EOF before consuming its first page");
            return Ok(None);
        };
        let mut files=Vec::with_capacity(records.len());
        for record in records {
            self.cancel.check()?;
            ensure!(started.elapsed()<Duration::from_secs(120),"USN enumeration exceeded its 120-second per-run budget");
            // The root FRN sequence is volume-specific. Do not treat MFT
            // record 5 as a child even when its sequence differs from the
            // historical constant; the collector derives the actual root ID
            // from child parent references on this first page.
            if record.frn.record_number()==5 {
                ensure!(record.attributes&0x10!=0,"USN record 5 is not a directory");
                continue;
            }
            ensure!(!record.name.is_empty() && record.name.len()<=1024,"USN enumeration returned an invalid name component");
            files.push(LayoutFile {frn:record.frn,attributes:record.attributes,size:None,mtime_ft:None,
                names:vec![LayoutName {parent_frn:record.parent_frn,name:record.name,name_lossy:record.name_lossy,dos_only:false}]});
        }
        self.enum_next_frn=next;
        Ok(Some(files))
    }
}

impl NativeSource {
    fn finish_initial_links_by_directory<F>(&mut self,catalog:&mut crate::catalog::Catalog,cancel:&Cancellation,
        checkpoint:&mut F)->Result<()>
    where F:FnMut(&Self,&crate::catalog::Catalog)->Result<()> {
        use std::{os::windows::io::FromRawHandle,time::Instant};
        use windows_sys::Win32::{Foundation::{ERROR_NO_MORE_FILES,INVALID_HANDLE_VALUE},Storage::FileSystem::{
            CreateFileW,GetFileInformationByHandleEx,FILE_ATTRIBUTE_DIRECTORY,FILE_ATTRIBUTE_REPARSE_POINT,
            FILE_ATTRIBUTE_TAG_INFO,FILE_FLAG_BACKUP_SEMANTICS,FILE_FLAG_OPEN_REPARSE_POINT,FILE_ID_INFO,
            FILE_LIST_DIRECTORY,FILE_READ_ATTRIBUTES,FILE_SHARE_DELETE,FILE_SHARE_READ,FILE_SHARE_WRITE,
            FileAttributeTagInfo,FileIdBothDirectoryInfo,FileIdBothDirectoryRestartInfo,FileIdInfo,OPEN_EXISTING}};
        ensure!(self.directory_link_walk && self.enum_layout_active,
            "directory hard-link reconciliation requires the explicit USN test provider");
        if self.enum_links_complete {return Ok(())}
        let started=Instant::now();
        let directories=catalog.directory_frns_sorted();
        let offset=if let Some(current)=self.directory_link_current {
            directories.partition_point(|frn|frn.0<current)
        } else if let Some(cursor)=self.enum_link_cursor {
            directories.partition_point(|frn|frn.0<=cursor)
        } else {0};
        if let Some(current)=self.directory_link_current {
            ensure!(directories.get(offset).is_some_and(|frn|frn.0==current),
                "checkpoint directory FRN {current:#018X} is absent from the restored catalog");
        }
        for frn in &directories[offset..] {
            cancel.check()?;
            ensure!(started.elapsed()<Duration::from_secs(120),"initial directory hard-link walk exceeded its 120-second per-run budget");
            self.directory_link_current=Some(frn.0);

            let path=if *frn==catalog.index.entry(catalog.index.root()).frn() {
                format!("{}:\\",self.drive).encode_utf16().collect::<Vec<_>>()
            } else {
                match catalog.api_path_for_frn(*frn) {
                    Ok(path)=>path,
                    Err(_error) if catalog.index.is_unreachable_dollar_metadata(*frn)=>{
                        self.io_stats.lock().unwrap().initial_ntfs_metadata_records_skipped+=1;
                        self.enum_link_cursor=Some(frn.0);self.directory_link_current=None;
                        checkpoint(self,catalog)?;continue
                    }
                    Err(error)=>return Err(error).with_context(||format!("cannot resolve directory FRN {:#018X} for batched reconciliation",frn.0)),
                }
            };
            let path_text=String::from_utf16_lossy(&path);
            let lower=path_text.to_ascii_lowercase();
            let svi=format!("{}:\\system volume information",self.drive.to_ascii_lowercase());
            if lower==svi || lower.strip_prefix(&svi).is_some_and(|tail|tail.starts_with('\\')) {
                self.io_stats.lock().unwrap().initial_protected_system_records_skipped+=1;
                self.enum_link_cursor=Some(frn.0);self.directory_link_current=None;
                checkpoint(self,catalog)?;continue
            }
            ensure!(path.len()>=3 && path.len()<32767 && path[1]==b':' as u16 && path[2]==b'\\' as u16 && !path.contains(&0),
                "directory path is not a bounded drive-absolute Windows path");
            let mut wide=r"\\?\".encode_utf16().collect::<Vec<_>>();
            wide.extend(path);wide.push(0);
            let _open_request=self.guard.begin();
            // SAFETY: the path is derived from the private catalog on the verified
            // test VHD; the handle is read-only and immediately wrapped in RAII.
            let raw=unsafe{CreateFileW(wide.as_ptr(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,null_mut(),OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,null_mut())};
            ensure!(raw!=INVALID_HANDLE_VALUE,"open directory FRN {:#018X} failed: {}",frn.0,std::io::Error::last_os_error());
            // SAFETY: CreateFileW returned one owned synchronous HANDLE.
            let directory=TrackedDirectoryHandle::new(unsafe{std::fs::File::from_raw_handle(raw as _)},self.io_stats.clone());
            drop(_open_request);
            let handle=directory.raw_handle();
            let mut tag=FILE_ATTRIBUTE_TAG_INFO::default();
            let _tag_request=self.guard.begin();
            ensure!(unsafe{GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&mut tag as *mut _ as _,std::mem::size_of_val(&tag) as u32)}!=0,
                "query directory attributes for FRN {:#018X} failed: {}",frn.0,std::io::Error::last_os_error());
            drop(_tag_request);
            ensure!(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY!=0,"FRN {:#018X} no longer resolves to a directory",frn.0);
            if tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT!=0 {
                self.io_stats.lock().unwrap().directory_reparse_points_skipped+=1;
                drop(directory);
                self.enum_link_cursor=Some(frn.0);self.directory_link_current=None;
                checkpoint(self,catalog)?;continue
            }
            let mut identity=FILE_ID_INFO::default();
            let _identity_request=self.guard.begin();
            ensure!(unsafe{GetFileInformationByHandleEx(handle,FileIdInfo,&mut identity as *mut _ as _,std::mem::size_of_val(&identity) as u32)}!=0,
                "query directory identity for FRN {:#018X} failed: {}",frn.0,std::io::Error::last_os_error());
            drop(_identity_request);
            ensure!(identity.FileId.Identifier[8..].iter().all(|byte|*byte==0),"directory FRN is not a zero-extended 64-bit NTFS file ID");
            let actual=u64::from_le_bytes(identity.FileId.Identifier[..8].try_into().unwrap());
            ensure!(actual==frn.0,"directory path FRN changed during initial build ({:#018X} != {:#018X})",actual,frn.0);

            let mut buffer=vec![0u64;8192];let mut first=true;
            loop {
                cancel.check()?;
                ensure!(started.elapsed()<Duration::from_secs(120),"initial directory hard-link walk exceeded its 120-second per-run budget");
                ensure!(self.enum_link_requests<1_000_000,"directory batch API request budget exceeded");
                buffer.fill(0);
                let class=if first{FileIdBothDirectoryRestartInfo}else{FileIdBothDirectoryInfo};
                let call_started=Instant::now();let _request=self.guard.begin();
                // SAFETY: one owned directory handle and a writable 64-KiB
                // aligned buffer remain valid for this synchronous API call.
                let ok=unsafe{GetFileInformationByHandleEx(handle,class,buffer.as_mut_ptr() as _,(buffer.len()*8) as u32)};
                drop(_request);
                {let mut stats=self.io_stats.lock().unwrap();stats.directory_batch_requests+=1;
                    stats.directory_batch_elapsed_us=stats.directory_batch_elapsed_us.saturating_add(call_started.elapsed().as_micros() as u64);}
                if ok==0 {
                    let error=unsafe{windows_sys::Win32::Foundation::GetLastError()};
                    if error==ERROR_NO_MORE_FILES {break}
                    anyhow::bail!("batched hard-link reconciliation failed for directory FRN {:#018X}: {error}",frn.0)
                }
                first=false;self.enum_link_requests+=1;
                let bytes=unsafe{std::slice::from_raw_parts(buffer.as_ptr() as *const u8,buffer.len()*8)};
                let mut batch_entries=0u64;
                for item in decode_file_id_both_dir_buffer(bytes)? {
                    if item.name.as_slice()==[b'.' as u16] || item.name.as_slice()==[b'.' as u16,b'.' as u16] {continue}
                    ensure!(!item.name.contains(&0) && !item.name.contains(&(b'\\' as u16)) && !item.name.contains(&(b'/' as u16)),
                        "directory API returned an invalid filename component");
                    let mut name=Vec::new();
                    let name_lossy=goz_core::wtf8::from_utf16(&item.name,&mut name);
                    let file=LayoutFile {frn:Frn(item.file_id),attributes:item.attributes,size:None,mtime_ft:None,
                        names:vec![LayoutName {parent_frn:*frn,name,name_lossy,dos_only:false}]};
                    catalog.index.enrich(&file);
                    batch_entries+=1;
                }
                self.io_stats.lock().unwrap().directory_entries_seen+=batch_entries;
                checkpoint(self,catalog)?;
            }
            drop(directory);
            self.enum_link_cursor=Some(frn.0);self.directory_link_current=None;
            checkpoint(self,catalog)?;
        }
        self.enum_links_complete=true;
        checkpoint(self,catalog)?;
        Ok(())
    }
}

impl MetadataSource for NativeSource {
    fn journal(&mut self) -> Result<JournalInfo> {
        self.cancel.check()?; let _io = self.guard.begin();
        match goz_winfs::query_usn_journal(&self.handle)? {
            JournalQuery::Active(j) => Ok(JournalInfo { journal_id:j.journal_id, first_usn:j.first_usn,
                next_usn:j.next_usn, lowest_valid_usn:j.lowest_valid_usn,max_usn:j.max_usn }),
            _ => bail!("USN journal is unavailable; Flowtary never creates/resizes it or starts a directory walker"),
        }
    }
    fn layout_page(&mut self, restart: bool) -> Result<Option<Vec<LayoutFile>>> {
        self.cancel.check()?;
        if self.enum_layout_active {return self.enum_layout_page()}
        self.io_stats.lock().unwrap().layout_requests+=1;
        let layout_started=Instant::now();
        let query_result={
            let _io=self.guard.begin();
            goz_winfs::query_file_layout(&self.handle,QUERY_FILE_LAYOUT_INCLUDE_NAMES,restart,&mut self.buf)
        };
        {let mut stats=self.io_stats.lock().unwrap();stats.layout_elapsed_us=stats.layout_elapsed_us.saturating_add(layout_started.elapsed().as_micros() as u64);}
        match query_result {
            Ok(None)=>Ok(None),
            Ok(Some(n))=>{self.cancel.check()?;Ok(Some(walk_layout_buffer(&self.buf[..n])?))},
            Err(error) if restart && error.code==50=>{
                // C: rejects QUERY_FILE_LAYOUT. Switch once, before publishing
                // any page, to bounded USN name enumeration. Hard-link names
                // are reconciled through the selected path-based or test-only
                // directory-batched API; failures leave the old snapshot intact.
                self.enum_layout_active=true;self.enum_started=Some(Instant::now());
                self.enum_layout_page()
            }
            Err(error)=>Err(error.into()),
        }
    }
    fn changes(&mut self, cursor: i64, journal_id: u64) -> Result<JournalWalk> {
        self.cancel.check()?; let _io = self.guard.begin();
        // Name/topology changes only. No DATA/CLOSE activity or per-file stat enrichment.
        let reason_mask = 0x100 | 0x200 | 0x1000 | 0x2000 | 0x10000;
        let started=Instant::now();self.io_stats.lock().unwrap().usn_journal_read_requests+=1;
        let result = goz_winfs::read_usn_journal(&self.handle,cursor,journal_id,reason_mask,0,&mut self.buf);
        {let mut stats=self.io_stats.lock().unwrap();stats.usn_journal_read_elapsed_us=stats.usn_journal_read_elapsed_us.saturating_add(started.elapsed().as_micros() as u64);}
        let n=result?;
        self.cancel.check()?;
        Ok(walk_journal_buffer(&self.buf[..n])?)
    }
    fn file_layout(&mut self, frn: Frn) -> Result<LayoutFile> {
        self.cancel.check()?;
        if self.enum_layout_active {
            bail!("path context is required for the bounded USN hard-link provider")
        }
        self.io_stats.lock().unwrap().layout_requests+=1;
        let layout_started=Instant::now();
        let query_result={
            let _io=self.guard.begin();
            goz_winfs::query_file_layout_raw(&self.handle,QUERY_FILE_LAYOUT_INCLUDE_NAMES,true,2,1,[frn.0,frn.0],&mut self.buf)
        };
        {let mut stats=self.io_stats.lock().unwrap();stats.layout_elapsed_us=stats.layout_elapsed_us.saturating_add(layout_started.elapsed().as_micros() as u64);}
        let n=match query_result {
            Ok(Some(n))=>n,
            Ok(None)=>anyhow::bail!("file record vanished during hard-link reconciliation"),
            Err(error) if error.code==50=>return Err(error.into()),
            Err(error)=>return Err(error.into()),
        };
        walk_layout_buffer(&self.buf[..n])?.into_iter().find(|f| f.frn == frn).context("requested file ID was not returned")
    }
    fn file_layout_with_catalog(&mut self,frn:Frn,catalog:&crate::catalog::Catalog)->Result<LayoutFile> {
        if self.enum_layout_active { return self.file_layout_by_path_api(catalog,frn) }
        match self.file_layout(frn) {
            Ok(file)=>Ok(file),
            Err(error) if error.downcast_ref::<goz_winfs::WinError>().is_some_and(|e|e.code==50) => {
                self.enum_layout_active=true;
                self.file_layout_by_path_api(catalog,frn)
            }
            Err(error)=>Err(error),
        }
    }
    fn finish_initial_links<F>(&mut self,catalog:&mut crate::catalog::Catalog,cancel:&Cancellation,checkpoint:&mut F)->Result<()>
    where F:FnMut(&Self,&crate::catalog::Catalog)->Result<()> {
        if !self.enum_layout_active || self.enum_links_complete {return Ok(())}
        if self.directory_link_walk {
            return self.finish_initial_links_by_directory(catalog,cancel,checkpoint)
        }
        let started=Instant::now();
        let protected_system_files=catalog.protected_system_metadata_frns();
        let files=catalog.file_frns_sorted();
        let mut offset=self.enum_link_cursor.map_or(0,|cursor|files.partition_point(|frn|frn.0<=cursor));
        loop {
            cancel.check()?;
            ensure!(started.elapsed()<Duration::from_secs(120),"initial hard-link enumeration exceeded its 120-second per-run budget");
            if offset>=files.len() {
                self.enum_links_complete=true;
                checkpoint(self,catalog)?;
                return Ok(())
            }
            let end=(offset+128).min(files.len());
            for frn in &files[offset..end] {
                cancel.check()?;
                ensure!(started.elapsed()<Duration::from_secs(120),"initial hard-link enumeration exceeded its 120-second per-run budget");
                ensure!(self.enum_link_requests<1_000_000,"initial hard-link API request budget exceeded");
                if protected_system_files.contains(frn) {
                    self.io_stats.lock().unwrap().initial_protected_system_records_skipped+=1;
                    self.enum_link_cursor=Some(frn.0);
                    continue;
                }
                if let Err(error)=catalog.api_path_for_frn(*frn) {
                    ensure!(catalog.index.is_unreachable_dollar_metadata(*frn),
                        "cannot resolve an ordinary file during complete hard-link reconciliation: {error:#}");
                    self.io_stats.lock().unwrap().initial_ntfs_metadata_records_skipped+=1;
                    self.enum_link_cursor=Some(frn.0);
                    continue;
                }
                let file=self.file_layout_by_path_api(catalog,*frn)?;
                let mut targets=Vec::with_capacity(file.names.len());
                for name in file.names {
                    let parent=catalog.index.head_of(name.parent_frn)
                        .ok_or_else(||anyhow::anyhow!("hard-link parent FRN is absent from the private catalog"))?;
                    ensure!(catalog.index.entry(parent).is_dir(),"hard-link parent FRN does not identify a directory");
                    targets.push(LinkTarget {parent,name:WtfName::new(name.name,name.name_lossy)});
                }
                ensure!(!targets.is_empty(),"hard-link API returned no searchable names");
                catalog.index.reconcile_links(*frn,&targets);
                self.enum_link_cursor=Some(frn.0);
                self.enum_link_requests+=1;
            }
            offset=end;
            checkpoint(self,catalog)?;
        }
    }
    fn checkpoint_state(&self)->Result<Option<serde_json::Value>> {
        self.resume_state().map(serde_json::to_value).transpose().map_err(Into::into)
    }
    fn restore_checkpoint_state(&mut self,state:serde_json::Value)->Result<()> {
        self.restore_resume_state(serde_json::from_value(state)?)
    }
    fn layout_pages_are_fragments(&self)->bool { self.enum_layout_active }
    fn requires_observed_root(&self)->bool { self.enum_layout_active }
}

#[cfg(test)] mod tests {
    use super::*;
    fn put32(buffer:&mut[u8],offset:usize,value:u32){buffer[offset..offset+4].copy_from_slice(&value.to_le_bytes());}
    fn put64(buffer:&mut[u8],offset:usize,value:u64){buffer[offset..offset+8].copy_from_slice(&value.to_le_bytes());}
    fn directory_item(buffer:&mut[u8],offset:usize,next:u32,file_id:u64,attributes:u32,name:&str){
        use windows_sys::Win32::Storage::FileSystem::FILE_ID_BOTH_DIR_INFO;
        let header=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileName);
        let attr=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileAttributes);
        let name_len=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileNameLength);
        let id=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileId);
        let units:Vec<u16>=name.encode_utf16().collect();
        put32(buffer,offset,next);put32(buffer,offset+attr,attributes);
        put32(buffer,offset+name_len,(units.len()*2) as u32);put64(buffer,offset+id,file_id);
        for(i,unit)in units.iter().enumerate(){buffer[offset+header+i*2..offset+header+i*2+2].copy_from_slice(&unit.to_le_bytes());}
    }
    #[test] fn decodes_batched_directory_names_and_file_ids(){
        use windows_sys::Win32::Storage::FileSystem::FILE_ID_BOTH_DIR_INFO;
        let header=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileName);
        let first_span=(header+"alpha.exe".encode_utf16().count()*2+7)&!7;
        let total=first_span+header+"beta.exe".encode_utf16().count()*2;
        let mut raw=vec![0;total];
        directory_item(&mut raw,0,first_span as u32,0x0001_0000_0000_002a,0x80,"alpha.exe");
        directory_item(&mut raw,first_span,0,0x0002_0000_0000_002a,0x10,"beta.exe");
        let decoded=decode_file_id_both_dir_buffer(&raw).unwrap();
        assert_eq!(decoded.len(),2);assert_eq!(decoded[0].file_id,0x0001_0000_0000_002a);
        assert_eq!(decoded[0].attributes,0x80);assert_eq!(String::from_utf16(&decoded[0].name).unwrap(),"alpha.exe");
        assert_eq!(decoded[1].file_id,0x0002_0000_0000_002a);assert_eq!(decoded[1].attributes,0x10);
    }
    #[test] fn rejects_malformed_batched_directory_offsets_and_names(){
        use windows_sys::Win32::Storage::FileSystem::FILE_ID_BOTH_DIR_INFO;
        let header=std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileName);
        let mut raw=vec![0;header+4];put32(&mut raw,0,8);
        assert!(decode_file_id_both_dir_buffer(&raw).is_err());
        put32(&mut raw,0,0);put32(&mut raw,std::mem::offset_of!(FILE_ID_BOTH_DIR_INFO,FileNameLength),3);
        assert!(decode_file_id_both_dir_buffer(&raw).is_err());
    }
    #[test] fn tracked_directory_handles_balance_active_count_on_drop(){
        let path=std::env::current_exe().unwrap();
        let stats=Arc::new(Mutex::new(NativeIoStats::default()));
        let first=TrackedDirectoryHandle::new(std::fs::File::open(&path).unwrap(),stats.clone());
        assert_eq!(stats.lock().unwrap().directory_handles_active,1);
        let second=TrackedDirectoryHandle::new(std::fs::File::open(path).unwrap(),stats.clone());
        assert_eq!(stats.lock().unwrap().directory_handles_peak,2);
        drop(second);
        assert_eq!(stats.lock().unwrap().directory_handles_active,1);
        drop(first);
        let state=stats.lock().unwrap();
        assert_eq!(state.directory_handles_opened,2);
        assert_eq!(state.directory_handles_active,0);
        assert_eq!(state.directory_handles_peak,2);
    }
    #[test] fn hypervisor_enabled_physical_hosts_are_not_test_vms() {
        assert!(!is_guest("LENOVO","21LE"));
        assert!(is_guest("Microsoft Corporation","Virtual Machine"));
        assert!(is_guest("VMware, Inc.","VMware Virtual Platform"));
    }
    #[test] fn local_host_authorization_does_not_disable_telemetry_or_memory_gates() {
        let mut manifest=Manifest { schema:2,machine_name:"test".into(),rollback_snapshot:String::new(),volume_guid:String::new(),drive:'T',volume_label:"FLOWTARY_TEST".into(),authorized_host_test:false,backing_vhd:Some("C:\\test.vhd".into()),volume_serial:Some(1),force_enum_usn:false,directory_link_walk:false };
        let mut sample=Sample {physical_available_bytes:4*1024*1024*1024,commit_ratio:0.5,system_handles:Some(2_500_000),..Default::default()};
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVhd,"LENOVO","21LE").is_err());
        manifest.authorized_host_test=true;
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVhd,"LENOVO","21LE").is_ok());
        manifest.directory_link_walk=true;
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVhd,"LENOVO","21LE").is_err());
        manifest.force_enum_usn=true;
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVhd,"LENOVO","21LE").is_ok());
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVolumeProbe,"LENOVO","21LE").is_err());
        assert!(environment_checks(&manifest,&sample,TestMode::VirtualMachine,"LENOVO","21LE").is_err());
        sample.physical_available_bytes=1024;
        assert!(environment_checks(&manifest,&sample,TestMode::AuthorizedHostVhd,"LENOVO","21LE").is_err());
    }
}
