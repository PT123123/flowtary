# MFT record decoder provenance and scope

Dependency: `mft = "=0.9.2"`, `default-features = false`.

- Source repository: https://github.com/omerbenamram/mft
- Published crate VCS revision: `33c8730fd21c68e2ceaafa30d0effd0735396976`, verified through its `.cargo_vcs_info.json`.
- Published crate SHA-256: `f83bb611ffa2cd92c071b85fc17e446852bcb171685d38f588a19bb6f421b4cb`, pinned in Cargo.lock.
- License: MIT OR Apache-2.0; copies in `licenses/mft/`.

Flowtary currently uses the FILE-record/header/attribute parser only. The default `mft_dump` CLI feature is disabled. It does not invoke the upstream CLI, full-path cache/resolver, forensic-image reader or product workspace applications.

`src/mft_names.rs` accepts at most 64 KiB per record, validates the header and update sequence, filters to FILE_NAME attributes before interpreting attribute values, excludes DOS-only aliases, preserves all UTF-16 code units through GoZ WTF-8 conversion, and reports base/extension ownership and base-record hardlink count. DATA attributes and file streams are not interpreted.

Raw bytes use strict update-sequence validation. The explicit GET_FILE_RECORD decoding entry point also accepts a kernel-restored representation only when every sector tail matches its corresponding replacement in the update-sequence array. A mixture of tagged/restored or corrupted tails is rejected. This exception is not used for raw-volume bytes.

The returned `Fragment` is deliberately not a complete `LayoutFile`. Extension records may contain other names belonging to a base record. A future streaming assembler must merge them by full owner reference, validate expected links, then replay USN changes before publication. The normal raw-volume decoder is currently unused in release code because the bulk MFT data-run reader has not yet been implemented.

The bounded native compatibility probe may request one explicit record through FSCTL_GET_NTFS_FILE_RECORD and pass it to this decoder. That tests the interface and record format; it is not a per-file whole-volume indexing implementation. Full indexing must use bounded bulk metadata reads, not millions of individual record requests.
