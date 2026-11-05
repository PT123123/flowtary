# Flowtary experimental indexer

The persistent snapshot query protocol is connected to the C++ d/f route. The query helper runs only when `%LOCALAPPDATA%\Flowtary\index-C.fti` exists; the C++ side keeps one child alive, rejects stale or truncated results, and never starts a directory walker. Production snapshot generation and USN ownership are not enabled yet. Experimental `collect` and `probe-volume` commands acquire an exclusive per-user lease and a second shared lease under the protected ProgramData tree, keyed by canonical volume GUID, before opening a volume. The service has a matching ProgramData lease entry point, but the read-only query service does not yet start collection or acquire it. Lock files remain as small diagnostic records to avoid delete/recreate races; the open exclusive handle indicates active ownership, and the recorded PID can be stale after exit. It partially reuses the MIT-licensed GoZ core/Win32 modules at the revision recorded in `vendor/goz/UPSTREAM.md`. No upstream daemon is launched.

## Reproduce the offline measurements

From the repository root in PowerShell:

```powershell
./tools/indexer_bench.ps1 -Entries 100000,300000
```

This builds with the locked dependencies and one Cargo job, runs the indexer's tests, records an idle baseline, and measures each synthetic size sequentially. It creates only explicit benchmark files; it never enumerates a host directory tree, opens a raw volume, creates a journal, or installs a service. The script stops on a failed/guard-aborted run. It does not remove previous runs.

Every run retains `samples.jsonl` and `report.json`, the source fingerprint, guard limits, and counter failures. Reports include six whole-machine dimensions: commit/limit, available physical memory and page input, System handles, disk read/write/busy/queue, paged/nonpaged pools, and CPU. `memory_pressure_ratio` is a proxy computed as `1 - physical_available / physical_total`, clamped to 0–100%; commit ratio is reported separately. It is not a Windows pressure score. Process CPU/private bytes/working set/handles supplement these; they do not replace machine counters.

Use `tools/indexer_summarize.ps1 <report.json> [...]` to compare runs. It reports the six whole-machine dimensions, sample-window duration, peak Pages Input/sec, and how many samples crossed the paging guard; missing counter coverage remains visible in the counter-error count. It reports a first-quarter-to-last-quarter median growth rate for commit, System handles, and both kernel pools only when the observation spans at least 60 seconds; shorter benchmarks show `null` instead of a noisy rate.

CPU and disk averages integrate the preceding sampling intervals, excluding the incoming phase boundary, and report valid coverage seconds. Repeated phase names remain distinct chronological episodes. A very short phase can have little valid rate coverage; never treat missing coverage as zero activity. Disk-byte integrals are sampling estimates. PhysicalDisk `_Total` busy percent is an aggregate, not a per-drive utilization diagnosis. Whole-machine counters contain unrelated host work; the reports do not causally attribute every delta to Flowtary. Sub-second allocation peaks can be missed by the sampler.

Synthetic runs cover metadata graph construction, posting construction, warm general/workshop queries, bounded broad queries, 1,600 incremental renames in 50 batches, atomic snapshot write, restore, and cooldown. They do **not** prove real NTFS throughput or minifilter safety.

## Persistent query protocol

For the registered query host, run `tools/install_indexer_service.ps1` once per installation (and again only when upgrading the service binary). The script has one UAC transition for service registration; normal application startup and queries connect to the already-running service without elevation. The service is LocalSystem-hosted because it must serve the protected shared pipe, but SCM is configured to grant it only `SeChangeNotifyPrivilege` and `SeImpersonatePrivilege`. Installation currently serves an existing snapshot only: it does not scan volumes or create the snapshot. Removing/upgrading the service requires another administrator authorization. Do not repeatedly launch the installer to start the application.

```powershell
./indexer/target/release/flowtary-indexer.exe serve-snapshot --snapshot path/to/fixture.fti
```

One UTF-8 JSON request per line on stdin; one response per line on stdout. Readiness/diagnostics go to stderr. The snapshot is loaded once, so each query avoids reloading the cache. EOF closes the host. The pipe transport is used by the C++ client. It remains a per-user child process rather than a registered service; the production snapshot writer is still pending.

Measure the persistent pipe roundtrip with `tools/indexer_protocol_probe.ps1 -Snapshot <fixture.fti> -Output <report.json>`. This uses one hidden child, 20 warmups and 1,000 measured requests by default. Timing excludes client JSON decoding and UI painting.

For a sustained end-to-end host observation, build `indexer_client_test` and run `tools/indexer_cpp_integration_bench.ps1` with `-SustainedSeconds 65 -QueriesPerSecond 10`. This queries only the explicit offline snapshot, samples all six machine dimensions through startup, workload, and cooldown, and records first/last-window resource trends. Keep the rate and duration bounded; this remains a query-path fixture, not a volume-indexing measurement.

```json
{"schema":1,"id":42,"query":{"words":["flowtary"],"workshop_only":true,"limit":10,"budget_ms":25}}
```

Responses echo `id` and contain either `result` or `error`. Invalid JSON gets a null id. Requests are limited to 64 KiB, terms/results/time are bounded, and budget exhaustion sets `truncated`. This mode has no live journal connection; it serves the explicit snapshot state only.

## Native testing

The default `collect` mode requires the VM conditions in `VM_VALIDATION.md`. The user subsequently explicitly authorized testing on this physical host, accepting that a reboot might be necessary. Two separate opt-in modes implement that authorization: `collect --host-test-vhd` confines collection to a verified, at-most-256-MiB file-backed NTFS test disk with matching serial/GUID/drive and `FLOWTARY_TEST` label; `probe-volume --host-volume-probe` permits only 1–4 name-metadata pages from the explicitly identified physical volume, with no index publication. These modes retain telemetry, growth guards and memory headroom checks; they record rather than reject the existing abnormal absolute pool/handle baseline. They do not enable unrestricted physical-volume collection.

`tools/indexer_host_vhd.ps1` prepares a fresh 128-MiB VHD in a new workspace log directory, verifies its image-to-disk identity and RAW state before formatting only that new disk, and tests bootstrap/resume/USN/hardlinks. It explicitly creates a journal only on that new fixture volume. It detaches the image afterward and retains it for review. `tools/indexer_host_probe.ps1` performs the bounded C-volume probe and records the attached minifilters. Both need Windows administrator rights and execute only after explicit local-test authorization; neither changes the existing C-volume journal.

The VHD harness also probes `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` on four fixture directories. This is currently an isolated benchmark capped at 100,000 aggregate entries, not a production provider. It records batched request count, names, FileIds, and six machine resource series so we can validate file-ID/FRN mapping and hardlink aggregation before considering replacement of the per-file path API.

An opt-in initial-build candidate now uses that batched API to reconcile hard-link names by walking the catalog's directory FRNs, avoiding one `FindFirstFileNameW` call per file. Enable it only in a fresh VHD harness with `tools/indexer_host_validation_once.ps1 -DirectoryLinkWalk -ScaleFixtureCount 10000`. It holds one directory handle at a time, uses 64-KiB batches, checkpoints the current directory so restart safely replays that directory idempotently, skips reparse directories and the protected System Volume Information subtree, and fails closed on unresolved ordinary directories or API errors. Its current 120-second / 1,000,000-batch limits are provisional. The provider is restricted in code to an explicitly authorized test VHD with the forced USN provider; it is not enabled for production collection until the VHD completeness, restart, USN replay, and six-metric resource checks pass.

To avoid a separate UAC prompt for each provider, `tools/indexer_host_validation_once.ps1` now runs the validation matrix serially under one elevated parent process. Its default matrix is `file-layout`, `usn-fallback-resume`, and `directory-link-walk`; each case gets a new VHD and its own before/after resource gates, and a failed case stops the remaining matrix after the harness cleanup. The optional 10,000-file scale fixture applies to the directory-walk case. You can select a subset with a comma-separated `-Scenarios` value, for example `-Scenarios usn-fallback-resume,directory-link-walk`, and use `-PlanOnly` to preview the run with no preflight or UAC. The ordinary-privilege six-metric preflight still runs before the single UAC prompt. The child harnesses inherit the already elevated token and do not request elevation again.

`probe-volume --host-test-vhd --probe-directory-tree` now measures a complete bounded test-volume directory walk with the same batched FileId API. It is capped at 100,000 entries, 10,000 directories, depth 32, and 30 seconds; it opens one directory handle at a time, opens no ordinary file handles, skips reparse directories and the protected System Volume Information subtree, and reports repeated file IDs as hardlink candidates. The single-UAC host harness can add up to 10,000 small files across 100 fixture subdirectories with `-ScaleFixtureCount`. This remains a test-only probe, not the production provider. The first 10,000-file attempt was cancelled by the machine commit-growth guard during cooldown; its report did not retain the tree counts, so a successful complete result is still pending.

The MFT/FILE_RECORD probes are diagnostic only. The current `collect` fallback does not read one FILE record per FRN: when the first `FSCTL_QUERY_FILE_LAYOUT` call returns error 50, it enumerates names and parent FRNs with `FSCTL_ENUM_USN_DATA`, then reconciles hard links by either serial `FindFirstFileNameW/FindNextFileNameW` calls (default candidate) or an opt-in batched directory API restricted to authorized test VHDs. To run the test-VHD checkpoint/resume and USN mutation harness, use `tools/indexer_host_validation_once.ps1 -Scenarios usn-fallback-resume` or include `directory-link-walk` for the directory-batched provider. It requires one elevated host test session and a new VHD per run. Reports include all six machine-wide resource series plus provider I/O counts and timings. A 128-MiB VHD closed the initial/resume/USN/query loop at 278 entries with the serial path API, but this is not evidence of large-volume completeness or system cost.

On the current C: volume, `FSCTL_QUERY_FILE_LAYOUT` returns error 50; `FSCTL_ENUM_USN_DATA` works but does not prove every hardlink name. `FSCTL_GET_NTFS_VOLUME_DATA` returns geometry, but both direct volume reads (error 50) and opening `\\?\C:\$MFT` (error 5, including a bounded backup-read privilege attempt) failed in single-record probes. Therefore the current host cannot use the proposed bulk-MFT reader, and no complete C: index is enabled. Do not substitute unbounded per-file `FSCTL_GET_NTFS_FILE_RECORD` calls.

```powershell
./indexer/target/release/flowtary-indexer.exe collect --manifest isolated-test.json --snapshot test.fti --metrics initial.jsonl --watch-seconds 60
./indexer/target/release/flowtary-indexer.exe collect --manifest isolated-test.json --snapshot test.fti --metrics resume.jsonl --resume --watch-seconds 60
```

For an interrupted initial build, retain the matching `test.fti.partial.fti` and restart with:

```powershell
./indexer/target/release/flowtary-indexer.exe collect --manifest isolated-test.json --snapshot test.fti --metrics initial-resume.jsonl --resume-initial --checkpoint-seconds 30
```

`--checkpoint-seconds` accepts 1–300 seconds and defaults to 30. It controls partial-catalog persistence only; it does not extend the collector's 120-second enumeration deadline or 1,000,000-record budget. A journal discontinuity, changed volume identity, invalid checkpoint, or unsupported provider state stops the run. The partial file is private and cannot be queried. This command has compiled but has not yet been exercised through a real interruption/restart on the FLOWTARY_TEST VHD.

Initial collection tries name-only `FSCTL_QUERY_FILE_LAYOUT`. If its first call specifically returns Win32 error 50, the source switches to bounded `FSCTL_ENUM_USN_DATA` name/parent records. The default hard-link reconciliation resolves each indexed path from the in-memory parent graph and calls `FindFirstFileNameW/FindNextFileNameW` serially; it is capped at 1,000,000 calls and 120 seconds. The opt-in directory-batched provider reads names/FileIds with `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)`, holds at most one directory handle, and is currently restricted to the authorized test VHD. Both paths fail closed on unresolved ordinary paths, mismatched file identities, journal discontinuity, or resource cancellation; neither falls back to an unrestricted walker. A complete 128-MiB VHD run validated checkpoint recovery, USN mutation replay and query at 278 entries for the serial path provider. The 10,000-entry directory-batched run remains unverified. Resume verifies volume identity and replays only USN changes; it never silently starts another initial enumeration. Polling defaults to 1 second, with bounded catch-up and checkpoints no more frequently than every 30 seconds plus final publication. Full rescan requires a separate explicit run with a new snapshot path.

The collector persists interrupted initial builds as a distinct `FTPART01` partial catalog plus provider cursor/state; only a complete `FTNTFS01` generation is loadable by the query host. Restart with `collect --resume-initial`; journal identity and continuation state are validated before continuing, and discontinuity fails closed. On completion the normal snapshot is atomically published and the partial file is retired. `FSCTL_QUERY_FILE_LAYOUT` has no durable continuation token, so its fallback uses the USN provider state. Checkpoint recovery and USN changes have passed the small VHD loop; production service ownership, multi-million-entry initial collection, restart stress, and system resource behavior remain unverified.

Incremental updates modify affected name postings. Moving a directory reclassifies the executable posting for the workshop boundary; it does not rebuild all name grams. Very common grams still require scanning their posting during batched removal, and metadata/name-arena compaction needs further sustained-load work. This is not yet a proven constant-cost updater.

## Work still required

- Privilege-separated production lifecycle, IPC security, multiple-volume ownership and C++ UI integration.
- The C++ launch catalog now uses bounded Start Menu/App Paths/AppsFolder discovery, narrow notifications and asynchronous icons. Real installed-app/packaged-app and icon resource validation is still required.
- Sustained incremental compaction and resource behavior under long-running change workloads.
- Validate version-ranking parity and the `path_utf16` IPC field on a real snapshot; unpaired UTF-16 path units are now preserved by the protocol.
- Real-volume VM measurements and driver-stack evidence before any default enablement.

Killing this process cannot guarantee recovery of leaked kernel objects. I/O cancellation/deadlines cannot guarantee a faulty driver completes or releases resources. Measurements on the authorized physical host include its pre-existing abnormal kernel baseline and unrelated background work.

The current provisional stop guard also cancels after three consecutive approximately one-second samples above 85% machine CPU, 90% aggregate disk busy, or 10,000 Pages Input/sec, and treats missing CPU/disk readings as a violation. Commit ratio above 85% or available RAM below 1.5 GiB cancels immediately; three consecutive violations in commit growth, System handle growth, or paged/nonpaged pool growth cancel the run. These thresholds protect an experiment from sustained saturation; they are not proof of acceptable leak rates or safety.
