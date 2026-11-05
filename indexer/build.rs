use std::{fs, path::Path};
fn main() {
    let mut hash = 0xcbf29ce484222325u64;
    for path in ["Cargo.toml","Cargo.lock","src/main.rs","src/catalog.rs","src/metrics.rs",
        "src/policy.rs","src/collector.rs","src/native.rs","src/snapshot.rs","src/bench.rs","src/session.rs","src/mft_names.rs"] {
        println!("cargo:rerun-if-changed={path}");
        for byte in fs::read(Path::new(path)).unwrap_or_default() { hash ^= byte as u64; hash=hash.wrapping_mul(0x100000001b3); }
    }
    println!("cargo:rustc-env=FLOWTARY_SOURCE_FINGERPRINT={hash:016x}");
}
