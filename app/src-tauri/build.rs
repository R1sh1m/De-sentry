fn main() {
    let target = std::env::var("TARGET").unwrap_or_default();
    let is_windows_gnu = target.contains("windows-gnu");

    let attrs = if is_windows_gnu {
        let windows = tauri_build::WindowsAttributes::new_without_app_manifest();
        tauri_build::Attributes::new().windows_attributes(windows)
    } else {
        tauri_build::Attributes::new()
    };

    tauri_build::try_build(attrs).expect("failed to run tauri-build");

    // Windows-GNU only: link an application manifest requesting Common
    // Controls v6. MSVC's link.exe does this automatically; GNU ld does not,
    // and without it a binary using the file dialogs starts against comctl32
    // v5, where TaskDialogIndirect does not exist
    // (STATUS_ENTRYPOINT_NOT_FOUND before main). Since we disabled Tauri's
    // built-in manifest above, linking this via rustc-link-arg provides the
    // sole manifest for both binaries and test harnesses without any
    // "multiple non-default manifests" duplicate linker warnings.
    if is_windows_gnu {
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
