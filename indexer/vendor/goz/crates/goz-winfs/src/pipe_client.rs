//! Server-side named-pipe client identity verification.
//!
//! A privileged pipe server must check the connected client after receiving a
//! request. The DACL is only the first gate; this function impersonates the
//! named-pipe client, reads its token user SID, and always reverts the thread.

use std::{
    ffi::c_void,
    mem::size_of,
    os::windows::io::{AsRawHandle, BorrowedHandle},
    ptr,
};

use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, LocalFree},
    Security::Authorization::ConvertSidToStringSidW,
    Security::{GetTokenInformation, RevertToSelf, TOKEN_QUERY, TOKEN_USER, TokenUser},
    System::{
        Pipes::ImpersonateNamedPipeClient,
        Threading::{GetCurrentThread, OpenThreadToken},
    },
};

use crate::error::{WinError, last_error};

struct OwnedHandle(HANDLE);

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: this wrapper takes ownership of a successful OpenThreadToken handle.
            unsafe { CloseHandle(self.0) };
        }
    }
}

struct LocalAllocation(*mut c_void);

impl Drop for LocalAllocation {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: this wrapper owns memory returned by ConvertSidToStringSidW.
            unsafe { LocalFree(self.0) };
        }
    }
}

struct RevertOnDrop;

impl Drop for RevertOnDrop {
    fn drop(&mut self) {
        // SAFETY: paired with a successful ImpersonateNamedPipeClient on this thread.
        unsafe { RevertToSelf() };
    }
}

/// Returns whether the connected named-pipe client has `expected_sid`.
///
/// Call only after the client has sent a request on this pipe. Impersonation is
/// scoped to this call and reverted on both success and failure. The caller
/// must keep the pipe handle valid for the duration of the call.
pub fn pipe_client_sid_matches(
    pipe: BorrowedHandle<'_>,
    expected_sid: &str,
) -> Result<bool, WinError> {
    let expected_sid = expected_sid.to_ascii_uppercase();
    if expected_sid.len() > 184 || !expected_sid.is_ascii() {
        return Err(WinError {
            code: 87,
            context: "invalid expected client SID",
        });
    }

    // SAFETY: `pipe` is a live named-pipe handle with a client request available.
    if unsafe { ImpersonateNamedPipeClient(pipe.as_raw_handle()) } == 0 {
        return Err(last_error("ImpersonateNamedPipeClient"));
    }
    let revert_guard = RevertOnDrop;
    let client_sid = client_sid_from_thread_token();
    // Revert before returning any token-reading error to the service loop.
    // SAFETY: the thread is impersonating the client from the successful call above.
    if unsafe { RevertToSelf() } == 0 {
        return Err(last_error("RevertToSelf"));
    }
    std::mem::forget(revert_guard);
    Ok(client_sid?.eq_ignore_ascii_case(&expected_sid))
}

fn client_sid_from_thread_token() -> Result<String, WinError> {
    let mut token: HANDLE = ptr::null_mut();
    // SAFETY: GetCurrentThread returns a valid pseudo-handle; `token` is a live out-pointer.
    if unsafe { OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, 1, &mut token) } == 0 {
        return Err(last_error("OpenThreadToken"));
    }
    let token = OwnedHandle(token);
    sid_from_token(token.0)
}

fn sid_from_token(token: HANDLE) -> Result<String, WinError> {
    let mut required = 0u32;
    // SAFETY: a null buffer and zero size request the required TOKEN_USER size.
    unsafe {
        GetTokenInformation(token, TokenUser, ptr::null_mut(), 0, &mut required);
    }
    if required < size_of::<TOKEN_USER>() as u32 {
        return Err(last_error("GetTokenInformation(size)"));
    }
    let word_count = (required as usize).div_ceil(size_of::<usize>());
    let mut storage = vec![0usize; word_count];
    let mut returned = 0u32;
    // SAFETY: `storage` is aligned for TOKEN_USER and has at least `required` bytes.
    if unsafe {
        GetTokenInformation(
            token,
            TokenUser,
            storage.as_mut_ptr().cast(),
            (storage.len() * size_of::<usize>()) as u32,
            &mut returned,
        )
    } == 0
    {
        return Err(last_error("GetTokenInformation(TokenUser)"));
    }
    if returned < size_of::<TOKEN_USER>() as u32 {
        return Err(WinError {
            code: 13,
            context: "short TOKEN_USER result",
        });
    }
    // SAFETY: successful GetTokenInformation wrote a TOKEN_USER at the aligned
    // start of `storage`, and `storage` remains alive while the SID is converted.
    let user = unsafe { &*storage.as_ptr().cast::<TOKEN_USER>() };
    if user.User.Sid.is_null() {
        return Err(WinError {
            code: 13,
            context: "TOKEN_USER has null SID",
        });
    }

    let mut sid_text: *mut u16 = ptr::null_mut();
    // SAFETY: `user.User.Sid` points inside the live token information buffer;
    // the API allocates the returned string for LocalFree.
    if unsafe { ConvertSidToStringSidW(user.User.Sid, &mut sid_text) } == 0 {
        return Err(last_error("ConvertSidToStringSidW"));
    }
    let sid_text = LocalAllocation(sid_text.cast());
    // SID text is bounded by the API's documented string representation size.
    let mut length = 0usize;
    // SAFETY: `sid_text` is a NUL-terminated UTF-16 string returned by Windows.
    unsafe {
        while length <= 184 && *sid_text.0.cast::<u16>().add(length) != 0 {
            length += 1;
        }
    }
    if length > 184 {
        return Err(WinError {
            code: 13,
            context: "oversized client SID",
        });
    }
    // SAFETY: the preceding bounded walk found a terminator; this slice covers
    // exactly the initialized UTF-16 code units before it.
    let wide = unsafe { std::slice::from_raw_parts(sid_text.0.cast::<u16>(), length) };
    String::from_utf16(wide).map_err(|_| WinError {
        code: 13,
        context: "invalid client SID UTF-16",
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use windows_sys::Win32::{
        Foundation::{
            CloseHandle, ERROR_PIPE_CONNECTED, GENERIC_READ, GENERIC_WRITE, HANDLE,
            INVALID_HANDLE_VALUE,
        },
        Security::SECURITY_ATTRIBUTES,
        Storage::FileSystem::{
            CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING, PIPE_ACCESS_DUPLEX,
        },
        System::{
            Pipes::{
                ConnectNamedPipe, CreateNamedPipeW, PIPE_READMODE_BYTE, PIPE_TYPE_BYTE, PIPE_WAIT,
            },
            Threading::{GetCurrentProcess, OpenProcessToken},
        },
    };

    fn current_process_sid() -> String {
        let mut token: HANDLE = ptr::null_mut();
        // SAFETY: GetCurrentProcess returns a pseudo-handle; `token` is a live out-pointer.
        assert_ne!(
            unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) },
            0
        );
        let token = OwnedHandle(token);
        sid_from_token(token.0).expect("read current process token SID")
    }

    #[test]
    fn pipe_server_matches_client_sid_and_reverts_impersonation() {
        let expected_sid = current_process_sid();
        let security = crate::pipe_sd::build_pipe_security_for_sid(&expected_sid).unwrap();
        let name = format!(r"\\.\pipe\flowtary-sid-test-{}", std::process::id());
        let wide: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
        // SAFETY: `wide` is NUL-terminated; security remains alive until the
        // server handle is closed, and all output buffers are internal to the API.
        let server = unsafe {
            CreateNamedPipeW(
                wide.as_ptr(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                security.attributes_ptr().cast::<SECURITY_ATTRIBUTES>(),
            )
        };
        assert_ne!(server, INVALID_HANDLE_VALUE, "CreateNamedPipeW failed");
        let (release_client, wait_for_release) = mpsc::channel::<()>();
        let client_name = wide.clone();
        let client = std::thread::spawn(move || {
            // SAFETY: `client_name` is a valid local pipe path and all unused
            // security/template pointers are null by API contract.
            let pipe = unsafe {
                CreateFileW(
                    client_name.as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    ptr::null(),
                    OPEN_EXISTING,
                    0,
                    ptr::null_mut(),
                )
            };
            if pipe == INVALID_HANDLE_VALUE {
                return false;
            }
            let request = [b'Q'];
            let mut written = 0u32;
            // SAFETY: `request` is a live one-byte buffer and `written` is a live out-pointer.
            let ok = unsafe {
                windows_sys::Win32::Storage::FileSystem::WriteFile(
                    pipe,
                    request.as_ptr().cast(),
                    1,
                    &mut written,
                    ptr::null_mut(),
                )
            } != 0;
            if ok {
                let _ = wait_for_release.recv();
            }
            // SAFETY: this thread owns the successful CreateFileW handle.
            unsafe { CloseHandle(pipe) };
            ok && written == 1
        });

        // SAFETY: `server` is a live pipe instance; a client thread is connecting.
        let connected = unsafe { ConnectNamedPipe(server, ptr::null_mut()) } != 0
            || unsafe { windows_sys::Win32::Foundation::GetLastError() } == ERROR_PIPE_CONNECTED;
        assert!(connected, "ConnectNamedPipe failed");
        let mut request = [0u8; 1];
        let mut read = 0u32;
        // SAFETY: `server` is connected and the client wrote one byte.
        assert_ne!(
            unsafe {
                windows_sys::Win32::Storage::FileSystem::ReadFile(
                    server,
                    request.as_mut_ptr().cast(),
                    1,
                    &mut read,
                    ptr::null_mut(),
                )
            },
            0
        );
        assert_eq!(request, [b'Q']);
        assert_eq!(read, 1);
        // SAFETY: the server handle is live and the request has been read.
        let borrowed = unsafe { BorrowedHandle::borrow_raw(server) };
        assert!(pipe_client_sid_matches(borrowed, &expected_sid).unwrap());
        assert!(!pipe_client_sid_matches(borrowed, &format!("{expected_sid}-99")).unwrap());
        let process_token = OwnedHandle(current_process_token());
        assert_eq!(
            sid_from_token(process_token.0).unwrap(),
            expected_sid,
            "the server thread must be back under its own token after each check"
        );
        release_client.send(()).unwrap();
        assert!(client.join().unwrap());
        // SAFETY: test owns the connected named-pipe server handle and closes it once.
        unsafe { CloseHandle(server) };
    }

    fn current_process_token() -> HANDLE {
        let mut token: HANDLE = ptr::null_mut();
        // SAFETY: GetCurrentProcess returns a pseudo-handle; `token` is a live out-pointer.
        assert_ne!(
            unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) },
            0
        );
        token
    }
}
