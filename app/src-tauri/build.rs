fn main() {
    tauri_build::build();

    // Windows-GNU only: link an application manifest requesting Common
    // Controls v6. MSVC's link.exe does this automatically; GNU ld does not,
    // and without it a binary using the file dialogs starts against comctl32
    // v5, where TaskDialogIndirect does not exist
    // (STATUS_ENTRYPOINT_NOT_FOUND before main). This must stay the
    // all-targets `rustc-link-arg`: the test harnesses need it (unit tests
    // live in src/, and cargo rejects the `-tests`-scoped key for packages
    // without an explicit [[test]] target), and real binaries merely end up
    // with the same dependency twice -- tauri-build embeds its own copy on
    // every toolchain -- which the linker reports as a benign
    // duplicate-manifest warning. MSVC, macOS, Linux and mobile builds never
    // reach this branch.
    if std::env::var("TARGET").unwrap_or_default().contains("windows-gnu") {
        let manifest_dir = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
        let rc = manifest_dir.join("app.manifest.rc");
        let out =
            std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("app_manifest.o");
        match std::process::Command::new("windres").arg(rc).arg("-o").arg(&out).status() {
            Ok(status) if status.success() => {
                println!("cargo:rustc-link-arg={}", out.display());
                println!("cargo:rerun-if-changed=app.manifest");
                println!("cargo:rerun-if-changed=app.manifest.rc");
            }
            _ => {
                println!(
                    "cargo:warning=windres not found: windows-gnu binaries will lack the \
                     comctl32 v6 manifest and may fail at startup with STATUS_ENTRYPOINT_NOT_FOUND"
                );
            }
        }
    }
}
