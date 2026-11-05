//! Cross-process per-volume ownership. A second collector/probe must fail
//! before opening the volume, so overlapping jobs cannot multiply filter-stack
//! activity or race snapshot publication.
use anyhow::{Context, Result, ensure};
use std::{fs::{self, File, OpenOptions}, io::Write, path::{Path, PathBuf}};

#[derive(Debug)]
pub struct VolumeOwner {
    _files: Vec<File>,
}

impl VolumeOwner {
    /// Acquire the per-user owner lock beneath `%LOCALAPPDATA%\Flowtary`.
    pub fn acquire(identity: &str) -> Result<Self> {
        let local = std::env::var_os("LOCALAPPDATA").context("LOCALAPPDATA is unavailable; cannot establish volume ownership")?;
        let program_data = std::env::var_os("ProgramData")
            .context("ProgramData is unavailable; cannot establish shared volume ownership")?;
        Self::acquire_with_shared_root(
            &PathBuf::from(local).join("Flowtary").join("VolumeOwners"),
            &service_lock_root(Path::new(&program_data)),
            identity,
        )
    }

    /// Acquire the service-wide owner lock beneath the protected Flowtary
    /// ProgramData tree. This coordinates the LocalSystem index service with
    /// every per-user experimental collector instead of relying on separate
    /// LocalAppData lock namespaces.
    #[allow(dead_code)] // Used when the privileged service gains its explicit collection command.
    pub fn acquire_service(identity: &str) -> Result<Self> {
        let program_data = std::env::var_os("ProgramData")
            .context("ProgramData is unavailable; cannot establish service volume ownership")?;
        Self::acquire_in(&service_lock_root(Path::new(&program_data)), identity)
    }

    fn acquire_in(root: &Path, identity: &str) -> Result<Self> {
        let key = volume_key(identity)?;
        fs::create_dir_all(root).with_context(|| format!("create volume owner directory {}", root.display()))?;
        let lock_path = root.join(format!("Volume-{key}.lock"));
        #[cfg(not(windows))]
        anyhow::bail!("volume ownership locks are implemented only for Windows");
        #[cfg(windows)]
        {
            use std::os::windows::fs::OpenOptionsExt;
            let mut options = OpenOptions::new();
            options.read(true).write(true).create(true).truncate(false).share_mode(0);
            // No sharing is the cross-process lease. Dropping File releases it
            // on success, cancellation, panic unwinding, or process exit.
            let mut file = options.open(&lock_path)
                .with_context(|| format!("another Flowtary worker owns this volume, or the owner lock is unavailable: {}", lock_path.display()))?;
            file.set_len(0).context("reset volume owner diagnostic record")?;
            writeln!(file, "pid={} identity={identity}", std::process::id()).context("write volume owner diagnostic record")?;
            file.flush().context("flush volume owner diagnostic record")?;
            Ok(Self { _files: vec![file] })
        }
    }

    fn acquire_with_shared_root(private_root: &Path, shared_root: &Path, identity: &str) -> Result<Self> {
        let private = Self::acquire_in(private_root, identity)?;
        let shared = Self::acquire_in(shared_root, identity)?;
        let mut files = private._files;
        files.extend(shared._files);
        Ok(Self { _files: files })
    }
}

fn service_lock_root(program_data: &Path) -> PathBuf {
    program_data.join("Flowtary").join("Indexes").join("VolumeOwners")
}

fn volume_key(identity: &str) -> Result<String> {
    const PREFIX: &str = "\\\\?\\Volume{";
    ensure!(identity.is_ascii() && identity.len() == 49 && identity.starts_with(PREFIX) && identity.ends_with("}\\"),
        "volume owner requires a canonical volume GUID path");
    let guid = &identity[PREFIX.len()..identity.len() - 2];
    ensure!(guid.len() == 36, "invalid volume GUID length");
    for (index, byte) in guid.bytes().enumerate() {
        if [8, 13, 18, 23].contains(&index) { ensure!(byte == b'-', "invalid volume GUID separator"); }
        else { ensure!(byte.is_ascii_hexdigit(), "invalid volume GUID digit"); }
    }
    Ok(guid.to_ascii_uppercase())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    const ID: &str = "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\";

    #[test]
    fn volume_owner_is_exclusive_and_released_by_raii() {
        let root = std::env::temp_dir().join(format!("flowtary-volume-owner-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let lock_path = root.join(format!("Volume-{}.lock", volume_key(ID).unwrap()));
        let first = VolumeOwner::acquire_in(&root, ID).unwrap();
        assert!(lock_path.is_file());
        assert!(VolumeOwner::acquire_in(&root, ID).is_err(), "second owner must be rejected");
        drop(first);
        let second = VolumeOwner::acquire_in(&root, ID).unwrap();
        drop(second);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn volume_owner_rejects_untrusted_lock_names() {
        let root = std::env::temp_dir().join(format!("flowtary-volume-owner-invalid-{}", std::process::id()));
        assert!(VolumeOwner::acquire_in(&root, "..\\outside").is_err());
        assert!(!root.exists());
    }

    #[test]
    fn service_owner_uses_the_shared_protected_programdata_namespace() {
        let program_data = std::env::temp_dir().join(format!("flowtary-service-owner-{}", std::process::id()));
        let root = service_lock_root(&program_data);
        assert_eq!(root, program_data.join("Flowtary").join("Indexes").join("VolumeOwners"));
        let first = VolumeOwner::acquire_in(&root, ID).unwrap();
        let user_root = program_data.join("user-local").join("VolumeOwners");
        assert!(VolumeOwner::acquire_with_shared_root(&user_root, &root, ID).is_err(),
            "a per-user worker must also acquire the service-wide lease");
        drop(first);
        let next = VolumeOwner::acquire_with_shared_root(&user_root, &root, ID).unwrap();
        assert!(VolumeOwner::acquire_in(&root, ID).is_err(), "the service must see a user worker's shared lease");
        drop(next);
        fs::remove_dir_all(program_data).unwrap();
    }

    #[test]
    fn volume_owner_exclusion_is_enforced_across_processes() {
        let root = std::env::temp_dir().join(format!("flowtary-volume-owner-process-{}", std::process::id()));
        let private_root = root.join("user-local").join("VolumeOwners");
        let shared_root = root.join("ProgramData").join("Flowtary").join("Indexes").join("VolumeOwners");
        if let Some(root) = std::env::var_os("FLOWTARY_OWNER_CHILD_ROOT") {
            let identity = std::env::var("FLOWTARY_OWNER_CHILD_IDENTITY").unwrap();
            let expected_locked = std::env::var("FLOWTARY_OWNER_CHILD_LOCKED").unwrap() == "1";
            let attempt = VolumeOwner::acquire_in(Path::new(&root), &identity);
            assert_eq!(attempt.is_err(), expected_locked, "child lease result disagreed with parent-held state");
            return;
        }

        let owner = VolumeOwner::acquire_with_shared_root(&private_root, &shared_root, ID).unwrap();
        let child = |owner_is_held: bool| {
            let status = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "owner::tests::volume_owner_exclusion_is_enforced_across_processes"])
                .env("FLOWTARY_OWNER_CHILD_ROOT", &shared_root)
                .env("FLOWTARY_OWNER_CHILD_IDENTITY", ID)
                .env("FLOWTARY_OWNER_CHILD_LOCKED", if owner_is_held { "1" } else { "0" })
                .status()
                .unwrap();
            assert!(status.success(), "child lease probe exited with {status}");
        };
        child(true);
        drop(owner);
        child(false);
        fs::remove_dir_all(root).unwrap();
    }
}
