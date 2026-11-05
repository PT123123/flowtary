//! Named-pipe security: builds the `SECURITY_ATTRIBUTES` the daemon passes to
//! the pipe server so an unelevated client can connect to a pipe created by an
//! elevated (or SYSTEM) process.
//!
//! The general daemon DACL grants SYSTEM and Administrators full control, and
//! Authenticated Users `0x12019b` = `FILE_GENERIC_READ | FILE_GENERIC_WRITE`
//! minus `FILE_CREATE_PIPE_INSTANCE (0x4)`. A separate SID-scoped builder is
//! available for privileged per-user services; Authenticated Users must not be
//! used for those service pipes.

use core::ffi::c_void;
use core::ptr;

use windows_sys::Win32::Foundation::LocalFree;
use windows_sys::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;

use crate::error::{WinError, last_error};
use crate::util::to_wide_nul;

/// `(0x120089 FILE_GENERIC_READ | 0x120116 FILE_GENERIC_WRITE) & !0x4`: read +
/// write to the pipe, but not `FILE_CREATE_PIPE_INSTANCE`.
const AUTH_USERS_ACCESS: &str = "0x12019b";
const SDDL_REVISION_1: u32 = 1;

/// Owns a self-relative security descriptor and a `SECURITY_ATTRIBUTES` that
/// points at it. Free-on-drop; keep it alive for as long as the pipe server
/// uses the attributes pointer.
pub struct PipeSecurity {
    descriptor: *mut c_void,
    attributes: Box<SECURITY_ATTRIBUTES>,
}

// SAFETY: `PipeSecurity` owns a self-relative security descriptor that
// `ConvertStringSecurityDescriptorToSecurityDescriptorW` allocated via LocalAlloc,
// a plain byte blob with no thread affinity. The value can therefore be moved
// to another thread (the tokio accept task) and its `LocalFree`-on-drop is valid
// on whichever thread finally drops it. The `SECURITY_ATTRIBUTES`/descriptor
// pointers are only read (never written) by the pipe server. `PipeSecurity` is
// intentionally left auto-`!Sync` (no `Sync` impl): the raw pointers must not be
// shared across threads concurrently.
unsafe impl Send for PipeSecurity {}

impl PipeSecurity {
    /// A raw pointer to the `SECURITY_ATTRIBUTES`, for
    /// `ServerOptions::create_with_security_attributes_raw`. Valid while `self`
    /// is alive.
    pub fn attributes_ptr(&self) -> *mut c_void {
        ptr::from_ref(self.attributes.as_ref()) as *mut c_void
    }
}

impl Drop for PipeSecurity {
    fn drop(&mut self) {
        if !self.descriptor.is_null() {
            // SAFETY: `descriptor` was allocated by
            // ConvertStringSecurityDescriptorToSecurityDescriptorW, which
            // documents LocalFree as the matching deallocator.
            unsafe {
                LocalFree(self.descriptor);
            }
        }
    }
}

/// Builds the pipe `SECURITY_ATTRIBUTES` from the goz SDDL.
pub fn build_pipe_security() -> Result<PipeSecurity, WinError> {
    let sddl = format!("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;{AUTH_USERS_ACCESS};;;AU)");
    build_from_sddl(&sddl)
}

/// Builds a pipe DACL for one caller SID, plus SYSTEM and Administrators.
/// The caller receives pipe read/write without `FILE_CREATE_PIPE_INSTANCE`.
/// This restricts who can connect; a privileged server must still impersonate
/// clients and verify their token SID before honoring requests.
pub fn build_pipe_security_for_sid(sid: &str) -> Result<PipeSecurity, WinError> {
    let sddl = scoped_pipe_sddl(sid)?;
    build_from_sddl(&sddl)
}

fn scoped_pipe_sddl(sid: &str) -> Result<String, WinError> {
    if !is_canonical_sid_text(sid) {
        return Err(WinError {
            code: 87,
            context: "invalid pipe trustee SID",
        });
    }
    Ok(format!("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;{AUTH_USERS_ACCESS};;;{sid})"))
}

fn is_canonical_sid_text(sid: &str) -> bool {
    if sid.len() > 184 || !sid.is_ascii() {
        return false;
    }
    let mut parts = sid.split('-');
    if parts.next() != Some("S") || parts.next() != Some("1") {
        return false;
    }
    let rest: Vec<_> = parts.collect();
    if rest.is_empty() || rest.len() > 16 {
        return false;
    }
    rest.iter()
        .all(|part| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit()))
}

fn build_from_sddl(sddl: &str) -> Result<PipeSecurity, WinError> {
    let wide = to_wide_nul(&sddl);

    let mut descriptor: *mut c_void = ptr::null_mut();
    // SAFETY: `wide` is a NUL-terminated SDDL string; on success `descriptor`
    // receives a LocalAlloc'd self-relative SD which we free on drop. The size
    // out-param is not needed (SECURITY_ATTRIBUTES does not carry it).
    let ok = unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            wide.as_ptr(),
            SDDL_REVISION_1,
            &mut descriptor,
            ptr::null_mut(),
        )
    };
    if ok == 0 {
        return Err(last_error(
            "ConvertStringSecurityDescriptorToSecurityDescriptorW",
        ));
    }

    let attributes = Box::new(SECURITY_ATTRIBUTES {
        nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: descriptor,
        bInheritHandle: 0,
    });
    Ok(PipeSecurity {
        descriptor,
        attributes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const FILE_CREATE_PIPE_INSTANCE: u32 = 0x4;
    const FILE_GENERIC_READ: u32 = 0x12_0089;
    const FILE_GENERIC_WRITE: u32 = 0x12_0116;

    fn auth_users_mask() -> u32 {
        u32::from_str_radix(AUTH_USERS_ACCESS.trim_start_matches("0x"), 16)
            .expect("AUTH_USERS_ACCESS must be a 0x-prefixed hex mask")
    }

    /// The security invariant: an unelevated client must never be able to add a
    /// competing instance to the daemon's pipe name. This is the assert that
    /// must survive any future DACL edit.
    #[test]
    fn auth_users_never_get_create_pipe_instance() {
        assert_eq!(
            auth_users_mask() & FILE_CREATE_PIPE_INSTANCE,
            0,
            "AU grant {AUTH_USERS_ACCESS} includes FILE_CREATE_PIPE_INSTANCE: \
             an unelevated process could squat the pipe name"
        );
    }

    /// Pins the grant to the formula documented on `AUTH_USERS_ACCESS`, so
    /// widening AU beyond read + write trips here rather than in production.
    #[test]
    fn auth_users_access_matches_documented_formula() {
        let expected = (FILE_GENERIC_READ | FILE_GENERIC_WRITE) & !FILE_CREATE_PIPE_INSTANCE;
        assert_eq!(auth_users_mask(), expected);
    }

    /// Proves the SDDL the daemon ships actually parses. Without this a typo in
    /// the format string surfaces only when the pipe server starts.
    #[test]
    fn goz_sddl_parses() {
        let security = build_pipe_security().expect("goz pipe SDDL must parse");
        assert!(!security.attributes_ptr().is_null());
    }

    #[test]
    fn sid_scoped_service_pipe_dacl_parses_and_rejects_injection() {
        let sid = "S-1-5-21-1234-5678-9012-1001";
        let sddl = scoped_pipe_sddl(sid).expect("valid SID should create a scoped DACL");
        assert_eq!(
            sddl,
            format!("D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;{AUTH_USERS_ACCESS};;;{sid})")
        );
        assert!(!sddl.contains(";;;AU)"), "scoped DACL must not allow all authenticated users");
        let security = build_pipe_security_for_sid(sid)
            .expect("canonical user SID must parse in a scoped DACL");
        assert!(!security.attributes_ptr().is_null());
        assert!(build_pipe_security_for_sid("S-1-5-21-1);(A;;GA;;;AU").is_err());
        assert!(build_pipe_security_for_sid("AU").is_err());
    }
}
