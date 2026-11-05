//! Windows service host for the per-user Flowtary query snapshot.
//!
//! The service currently exposes read-only snapshot queries over a SID-scoped
//! named pipe. It does not enumerate or open volumes. The collector is kept out
//! of this command until its NTFS provider has passed live completeness and
//! whole-machine resource gates.

#[cfg(windows)]
mod windows {
    use crate::{catalog::Catalog, policy::Cancellation, reloader::{self, SnapshotReloader}, session};
    use anyhow::{Context, Result, ensure};
    use std::{
        ffi::OsString,
        fs::{self, File},
        io::{BufReader, Write},
        os::windows::io::{FromRawHandle, RawHandle},
        path::{Path, PathBuf},
        sync::{Arc, OnceLock, RwLock},
        time::Duration,
    };
    use windows_service::{
        define_windows_service,
        service::{
            ServiceAccess, ServiceControl, ServiceControlAccept, ServiceErrorControl,
            ServiceExitCode, ServiceInfo, ServiceSidType, ServiceState, ServiceStartType,
            ServiceStatus, ServiceType,
        },
        service_control_handler::{self, ServiceControlHandlerResult},
        service_dispatcher,
        service_manager::{ServiceManager, ServiceManagerAccess},
    };
    use windows_sys::Win32::{
        Foundation::{ERROR_OPERATION_ABORTED, ERROR_PIPE_CONNECTED, GetLastError, HANDLE, INVALID_HANDLE_VALUE},
        Security::SECURITY_ATTRIBUTES,
        Storage::FileSystem::{FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_DUPLEX},
        System::Pipes::{ConnectNamedPipe, CreateNamedPipeW, PIPE_READMODE_BYTE, PIPE_TYPE_BYTE, PIPE_WAIT},
        System::Services::{
            ChangeServiceConfig2W, CloseServiceHandle, OpenSCManagerW, OpenServiceW,
            SC_MANAGER_CONNECT, SERVICE_CHANGE_CONFIG, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO,
            SERVICE_REQUIRED_PRIVILEGES_INFOW,
        },
    };

    const SERVICE_NAME: &str = "FlowtaryIndexer";
    const SERVICE_DISPLAY_NAME: &str = "Flowtary Index Service";
    const PIPE_NAME: &str = r"\\.\pipe\Flowtary.Indexer.v1";
    const PIPE_BUFFER_BYTES: u32 = 64 * 1024;

    #[derive(Clone)]
    struct Config {
        snapshot: PathBuf,
        owner_sid: String,
    }

    static CONFIG: OnceLock<Config> = OnceLock::new();

    /// Installs the service. The caller must be an administrator; the product
    /// installer is responsible for the single explicit UAC transition.
    pub fn install(snapshot: &Path, owner_sid: &str) -> Result<()> {
        ensure!(goz_winfs::is_elevated(), "service installation requires an administrator token");
        let snapshot = validate_snapshot_path(snapshot)?;
        let _pipe_security = goz_winfs::build_pipe_security_for_sid(owner_sid)?;
        let executable_path = std::env::current_exe().context("resolve service executable")?;
        validate_service_image_path(&executable_path)?;

        let manager = ServiceManager::local_computer(
            None::<&str>,
            ServiceManagerAccess::CONNECT | ServiceManagerAccess::CREATE_SERVICE,
        )?;
        let info = service_info(&executable_path, &snapshot, owner_sid);
        let service = manager.create_service(
            &info,
            ServiceAccess::CHANGE_CONFIG | ServiceAccess::QUERY_STATUS | ServiceAccess::START,
        ).context("create Flowtary Index Service (remove an existing service before reinstalling)")?;
        service.set_config_service_sid_info(ServiceSidType::Unrestricted)
            .context("enable Flowtary service SID")?;
        service.set_description("Serves Flowtary's read-only local search snapshot. Volume collection is not enabled.")?;
        restrict_service_privileges()?;
        service.start(&[] as &[OsString]).context("start Flowtary Index Service")?;
        println!("installed and started {SERVICE_NAME}; snapshot queries only, no volume access");
        Ok(())
    }

    /// LocalSystem is needed for stable service hosting, but its default token
    /// contains many privileges. The query-only service needs directory
    /// traversal and named-pipe client impersonation only.
    fn restrict_service_privileges() -> Result<()> {
        let manager = unsafe { OpenSCManagerW(std::ptr::null(), std::ptr::null(), SC_MANAGER_CONNECT) };
        ensure!(!manager.is_null(), "open SCM for privilege restriction: {}", std::io::Error::last_os_error());
        let name: Vec<u16> = SERVICE_NAME.encode_utf16().chain(Some(0)).collect();
        let service = unsafe { OpenServiceW(manager, name.as_ptr(), SERVICE_CHANGE_CONFIG) };
        if service.is_null() {
            let error = std::io::Error::last_os_error();
            unsafe { CloseServiceHandle(manager); }
            anyhow::bail!("open service for privilege restriction: {error}");
        }
        // REG_MULTI_SZ: two NUL-terminated privilege names and a final NUL.
        let mut privileges: Vec<u16> = ["SeChangeNotifyPrivilege", "SeImpersonatePrivilege"]
            .into_iter().flat_map(|value| value.encode_utf16().chain(Some(0))).collect();
        privileges.push(0);
        let mut info = SERVICE_REQUIRED_PRIVILEGES_INFOW { pmszRequiredPrivileges: privileges.as_mut_ptr() };
        let ok = unsafe { ChangeServiceConfig2W(service, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, (&mut info as *mut SERVICE_REQUIRED_PRIVILEGES_INFOW).cast()) };
        let result = if ok == 0 {
            Err(std::io::Error::last_os_error()).context("restrict Flowtary service privileges")
        } else { Ok(()) };
        unsafe { CloseServiceHandle(service); CloseServiceHandle(manager); }
        result
    }

    /// Stops and removes the service. This is an administrator operation.
    pub fn uninstall() -> Result<()> {
        ensure!(goz_winfs::is_elevated(), "service removal requires an administrator token");
        let manager = ServiceManager::local_computer(None::<&str>, ServiceManagerAccess::CONNECT)?;
        let service = manager.open_service(
            SERVICE_NAME,
            ServiceAccess::QUERY_STATUS | ServiceAccess::STOP | ServiceAccess::DELETE,
        ).context("open Flowtary Index Service")?;
        if service.query_status()?.current_state != ServiceState::Stopped {
            service.stop().context("send service stop")?;
            let deadline = std::time::Instant::now() + Duration::from_secs(20);
            while service.query_status()?.current_state != ServiceState::Stopped {
                ensure!(std::time::Instant::now() < deadline, "timed out waiting for Flowtary Index Service to stop");
                std::thread::sleep(Duration::from_millis(100));
            }
        }
        service.delete().context("delete Flowtary Index Service")?;
        println!("removed {SERVICE_NAME}");
        Ok(())
    }

    /// Enters the SCM dispatcher using the immutable service configuration in
    /// the ImagePath arguments.
    pub fn run_from_args(args: &[String]) -> Result<()> {
        let snapshot = option(args, "--snapshot").context("service-run requires --snapshot")?;
        let owner_sid = option(args, "--owner-sid").context("service-run requires --owner-sid")?;
        let config = Config { snapshot: validate_snapshot_path(Path::new(&snapshot))?, owner_sid };
        let _ = goz_winfs::build_pipe_security_for_sid(&config.owner_sid)?;
        CONFIG.set(config).map_err(|_| anyhow::anyhow!("service configuration already initialized"))?;
        service_dispatcher::start(SERVICE_NAME, ffi_service_main).context("connect to Windows Service Control Manager")?;
        Ok(())
    }

    fn option(args: &[String], key: &str) -> Option<String> {
        args.iter().position(|arg| arg == key).and_then(|index| args.get(index + 1)).cloned()
    }

    fn service_info(executable: &Path, snapshot: &Path, owner_sid: &str) -> ServiceInfo {
        ServiceInfo {
            name: OsString::from(SERVICE_NAME),
            display_name: OsString::from(SERVICE_DISPLAY_NAME),
            service_type: ServiceType::OWN_PROCESS,
            start_type: ServiceStartType::AutoStart,
            error_control: ServiceErrorControl::Normal,
            executable_path: executable.to_path_buf(),
            launch_arguments: vec![
                OsString::from("service-run"),
                OsString::from("--snapshot"),
                snapshot.as_os_str().to_owned(),
                OsString::from("--owner-sid"),
                OsString::from(owner_sid),
            ],
            dependencies: vec![],
            account_name: None, // LocalSystem; only fixed query operations are exposed.
            account_password: None,
        }
    }

    fn validate_snapshot_path(path: &Path) -> Result<PathBuf> {
        ensure!(path.is_absolute(), "service snapshot path must be absolute");
        let program_data = std::env::var_os("ProgramData").context("ProgramData is unavailable")?;
        let root = PathBuf::from(program_data).join("Flowtary").join("Indexes");
        let root = root.canonicalize().or_else(|_| {
            fs::create_dir_all(&root)?;
            root.canonicalize()
        }).context("resolve protected ProgramData index directory")?;
        let path = normalize_absolute(path)?;
        ensure!(path.starts_with(&root), "service snapshot must remain under ProgramData\\Flowtary\\Indexes");
        ensure!(path.file_name().is_some_and(|name| name.eq_ignore_ascii_case("index-C.fti")),
            "service snapshot file must be named index-C.fti");
        Ok(path)
    }

    fn normalize_absolute(path: &Path) -> Result<PathBuf> {
        let parent = path.parent().context("snapshot path has no parent directory")?;
        let parent = parent.canonicalize().context("resolve snapshot parent directory")?;
        let name = path.file_name().context("snapshot path has no file name")?;
        Ok(parent.join(name))
    }

    fn validate_service_image_path(path: &Path) -> Result<()> {
        let program_files = std::env::var_os("ProgramFiles").context("ProgramFiles is unavailable")?;
        let trusted_root = PathBuf::from(program_files).join("Flowtary").canonicalize()
            .context("service executable must be installed under Program Files\\Flowtary")?;
        let executable = path.canonicalize().context("canonicalize service executable")?;
        ensure!(executable.starts_with(&trusted_root), "refusing to register a service binary outside Program Files\\Flowtary");
        ensure!(executable.file_name().is_some_and(|name| name.eq_ignore_ascii_case("flowtary-indexer.exe")),
            "service image must be flowtary-indexer.exe");
        Ok(())
    }

    define_windows_service!(ffi_service_main, service_main);

    fn service_main(_arguments: Vec<OsString>) {
        let result = run_service();
        if let Err(error) = result {
            let _ = append_service_log(&format!("service stopped: {error:#}"));
        }
    }

    fn run_service() -> Result<()> {
        let config = CONFIG.get().context("service configuration was not initialized")?.clone();
        let cancel = Cancellation::default();
        let thread_io = Arc::new(goz_winfs::current_thread_io_handle()?);
        let control_cancel = cancel.clone();
        let control_io = thread_io.clone();
        let handler = move |event| match event {
            ServiceControl::Interrogate => ServiceControlHandlerResult::NoError,
            ServiceControl::Stop => {
                control_cancel.cancel();
                let _ = goz_winfs::cancel_synchronous_io(&control_io);
                ServiceControlHandlerResult::NoError
            }
            _ => ServiceControlHandlerResult::NotImplemented,
        };
        let status = service_control_handler::register(SERVICE_NAME, handler)?;
        status.set_service_status(ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: ServiceState::StartPending,
            controls_accepted: ServiceControlAccept::empty(),
            exit_code: ServiceExitCode::NO_ERROR,
            checkpoint: 1,
            wait_hint: Duration::from_secs(5),
            process_id: Some(std::process::id()),
        })?;
        status.set_service_status(ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: ServiceState::Running,
            controls_accepted: ServiceControlAccept::STOP,
            exit_code: ServiceExitCode::NO_ERROR,
            checkpoint: 0,
            wait_hint: Duration::default(),
            process_id: Some(std::process::id()),
        })?;

        let result = serve_pipe(&config, &cancel);
        status.set_service_status(ServiceStatus {
            service_type: ServiceType::OWN_PROCESS,
            current_state: ServiceState::Stopped,
            controls_accepted: ServiceControlAccept::empty(),
            exit_code: if result.is_ok() || cancel.is_cancelled() {
                ServiceExitCode::NO_ERROR
            } else {
                ServiceExitCode::ServiceSpecific(1)
            },
            checkpoint: 0,
            wait_hint: Duration::default(),
            process_id: None,
        })?;
        result
    }

    fn serve_pipe(config: &Config, cancel: &Cancellation) -> Result<()> {
        let security = goz_winfs::build_pipe_security_for_sid(&config.owner_sid)?;
        let pipe_name: Vec<u16> = PIPE_NAME.encode_utf16().chain(Some(0)).collect();
        let mut catalog: Option<Arc<RwLock<Catalog>>> = None;
        let mut _snapshot_reloader: Option<SnapshotReloader> = None;
        while !cancel.is_cancelled() {
            // FIRST_PIPE_INSTANCE makes a pre-created squatter a fatal error.
            // The owner SID DACL blocks all other unapproved local clients.
            let raw_pipe = unsafe {
                CreateNamedPipeW(
                    pipe_name.as_ptr(),
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    1,
                    PIPE_BUFFER_BYTES,
                    PIPE_BUFFER_BYTES,
                    0,
                    security.attributes_ptr().cast::<SECURITY_ATTRIBUTES>(),
                )
            };
            if raw_pipe == INVALID_HANDLE_VALUE {
                return Err(std::io::Error::last_os_error()).context("create first-instance index pipe");
            }
            // SAFETY: `raw_pipe` is a successful CreateNamedPipeW handle and is
            // transferred into `File`, which closes it exactly once.
            let server = unsafe { File::from_raw_handle(raw_pipe as RawHandle) };
            let connect_result = unsafe { ConnectNamedPipe(raw_pipe, std::ptr::null_mut()) };
            if connect_result == 0 {
                let code = unsafe { GetLastError() };
                if code == ERROR_OPERATION_ABORTED && cancel.is_cancelled() { break; }
                if code != ERROR_PIPE_CONNECTED {
                    let _ = append_service_log(&format!("named-pipe connect failed with Win32 {code}"));
                    continue;
                }
            }

            let mut writer = server.try_clone().context("duplicate connected pipe handle")?;
            let mut reader = BufReader::new(server);
            if catalog.is_none() {
                let telemetry_dir=PathBuf::from(std::env::var_os("ProgramData").context("ProgramData unavailable")?)
                    .join("Flowtary").join("Logs");
                match reloader::load_snapshot(&config.snapshot,&telemetry_dir,cancel,"service-initial-load") {
                    Ok((loaded,generation)) => {
                        let shared=Arc::new(RwLock::new(loaded));
                        _snapshot_reloader=Some(SnapshotReloader::start(config.snapshot.clone(),telemetry_dir,
                            generation,shared.clone(),cancel)?);
                        catalog=Some(shared);
                    }
                    Err(error) => {
                        send_not_ready(&mut writer)?;
                        let _ = append_service_log(&format!("initial snapshot load deferred: {error:#}"));
                        continue;
                    }
                }
            }
            let catalog = catalog.as_ref().expect("loaded above");
            let pipe_handle: HANDLE = raw_pipe;
            let result = session::serve_shared(catalog, &mut reader, &mut writer, cancel, || {
                // SAFETY: `pipe_handle` stays alive in `reader` until this
                // connection ends; the helper only borrows it during the SID check.
                let borrowed = unsafe { std::os::windows::io::BorrowedHandle::borrow_raw(pipe_handle) };
                ensure!(goz_winfs::pipe_client_sid_matches(borrowed, &config.owner_sid)?, "named-pipe client SID rejected");
                Ok(())
            });
            if let Err(error) = result {
                if !cancel.is_cancelled() {
                    let _ = append_service_log(&format!("query pipe closed: {error:#}"));
                }
            }
        }
        Ok(())
    }

    fn send_not_ready(writer:&mut impl Write)->Result<()> {
        let response = serde_json::json!({"schema":1,"ready":false,"error":"snapshot unavailable"});
        serde_json::to_writer(&mut *writer, &response)?;
        writer.write_all(b"\n")?;
        writer.flush()?;
        Ok(())
    }

    fn append_service_log(line: &str) -> Result<()> {
        let root = PathBuf::from(std::env::var_os("ProgramData").context("ProgramData unavailable")?)
            .join("Flowtary").join("Logs");
        fs::create_dir_all(&root).context("create service log directory")?;
        let path = root.join("indexer-service.log");
        let mut file = fs::OpenOptions::new().create(true).append(true).open(path)?;
        writeln!(file, "{} {line}", chrono_text())?;
        Ok(())
    }

    fn chrono_text() -> String {
        let unix_ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        format!("pid={} unix_ms={unix_ms}", std::process::id())
    }
}

#[cfg(windows)]
pub use windows::{install, run_from_args, uninstall};

#[cfg(not(windows))]
pub fn install(_: &std::path::Path, _: &str) -> anyhow::Result<()> {
    anyhow::bail!("the index service is available only on Windows")
}

#[cfg(not(windows))]
pub fn run_from_args(_: &[String]) -> anyhow::Result<()> {
    anyhow::bail!("the index service is available only on Windows")
}

#[cfg(not(windows))]
pub fn uninstall() -> anyhow::Result<()> {
    anyhow::bail!("the index service is available only on Windows")
}

