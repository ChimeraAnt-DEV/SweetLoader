//! Reading and safely extracting `.universe` archives.
//!
//! The archive is untrusted input: its entry names are attacker-controlled, and a name with `..` or
//! a leading `/` would extract outside the sandbox. Every entry is therefore checked against a
//! lexical rule (a relative path with no `..`, no drive/UNC prefix, no NUL) before any file is
//! created, and the extraction is staged and renamed into place only when complete, so a truncated
//! or hostile archive cannot leave a half-written mod behind.
//!
//! The manifest is read straight out of the archive without extracting it, so validation (including
//! the version gate and the signature check the caller does) happens before a single byte is written
//! to disk.

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

use crate::manifest::{self, UniverseManifest, MANIFEST_NAME};
use crate::{Error, Result};

/// The most entries an archive may hold. A real package has a manifest, a handful of libraries and
/// some assets; a thousand is generous and stops a hostile archive exhausting the entry table.
pub const MAX_ENTRIES: usize = 4096;

/// The most bytes an archive may expand to. A zip bomb is bounded by this before it is written.
pub const MAX_UNCOMPRESSED_BYTES: u64 = 256 * 1024 * 1024;

/// The most bytes an archive file may be. The whole file is read into memory to parse it.
pub const MAX_ARCHIVE_BYTES: u64 = 512 * 1024 * 1024;

/// The longest path a package entry or a sandbox root may name. Linux `PATH_MAX`.
pub const MAX_PATH_LENGTH: usize = 4096;

/// True when `path` is a safe relative path: no root, no parent segments, no NUL.
///
/// Accepts both `/` and `\` as separators (a ZIP written on Windows), treating `\` as `/`.
pub fn is_safe_relative_path(path: &str) -> bool {
    if path.is_empty() || path.len() > MAX_PATH_LENGTH {
        return false;
    }
    if path.contains('\0') {
        return false;
    }
    let normalised = path.replace('\\', "/");
    if normalised.starts_with('/') {
        return false;
    }
    // A Windows drive letter (`C:`) or a UNC prefix is an absolute path in disguise.
    if normalised.len() >= 2 && normalised.as_bytes()[1] == b':' {
        return false;
    }
    !normalised.split('/').any(|segment| segment == "..")
}

/// True when `path` is an absolute path with no `..` segment and no NUL.
pub fn is_safe_absolute_path(path: &str) -> bool {
    if path.is_empty() || path.len() > MAX_PATH_LENGTH {
        return false;
    }
    if path.contains('\0') {
        return false;
    }
    if !path.starts_with('/') {
        return false;
    }
    !path.split('/').any(|segment| segment == "..")
}

/// Resolves `entry` under `root` and refuses anything that escapes it, on both the lexical path and,
/// where the filesystem can answer, the canonical one (which catches a symlinked parent).
pub fn resolve_within(root: &Path, entry: &str) -> Result<PathBuf> {
    if !is_safe_relative_path(entry) {
        return Err(Error::new(format!(
            "archive entry escapes the sandbox: {entry}"
        )));
    }
    let normalised = entry.replace('\\', "/");
    let mut destination = root.to_path_buf();
    for segment in normalised.split('/') {
        if segment.is_empty() || segment == "." {
            continue;
        }
        destination.push(segment);
    }
    // Defence in depth: a lexical `..` cannot survive `is_safe_relative_path`, but a symlinked
    // directory inside the staging root could still redirect the write. Compare the resolved parent
    // when it exists.
    if let Some(parent) = destination.parent() {
        if parent.exists() {
            if let (Ok(real_root), Ok(real_parent)) = (root.canonicalize(), parent.canonicalize()) {
                if !real_parent.starts_with(&real_root) {
                    return Err(Error::new(format!(
                        "archive entry resolves outside the sandbox: {entry}"
                    )));
                }
            }
        }
    }
    Ok(destination)
}

/// Refuses a sandbox root that is not an absolute path inside `allowed_root`.
pub fn validate_sandbox_root(sandbox_root: &str, allowed_root: &str) -> Result<()> {
    if !is_safe_absolute_path(sandbox_root) {
        return Err(Error::new(
            "the sandbox root must be an absolute path with no parent segments",
        ));
    }
    if !is_safe_absolute_path(allowed_root) {
        return Err(Error::new(
            "the allowed root must be an absolute path with no parent segments",
        ));
    }
    let sandbox = Path::new(sandbox_root);
    let allowed = Path::new(allowed_root);
    if !lexically_within(allowed, sandbox) {
        return Err(Error::new(
            "the sandbox root must live inside the app's private directory",
        ));
    }
    // Where both exist, also require the canonical containment, so a symlink cannot point the
    // sandbox out of the allowed tree.
    if allowed.exists() && sandbox.exists() {
        if let (Ok(real_allowed), Ok(real_sandbox)) =
            (allowed.canonicalize(), sandbox.canonicalize())
        {
            if !real_sandbox.starts_with(&real_allowed) {
                return Err(Error::new(
                    "the sandbox root resolves outside the app's private directory",
                ));
            }
        }
    }
    Ok(())
}

/// True when `candidate` is `root` or a descendant of it, compared component by component.
fn lexically_within(root: &Path, candidate: &Path) -> bool {
    let mut root_components = root.components();
    let mut candidate_components = candidate.components();
    loop {
        match (root_components.next(), candidate_components.next()) {
            (None, _) => return true,
            (Some(expected), Some(actual)) => {
                if expected != actual {
                    return false;
                }
            }
            (Some(_), None) => return false,
        }
    }
}

/// Opens an archive and returns its entry names, refusing a malformed or oversized one.
fn open_archive(archive_path: &str) -> Result<zip::ZipArchive<fs::File>> {
    let metadata = fs::metadata(archive_path)
        .map_err(|e| Error::new(format!("could not stat the package: {e}")))?;
    if metadata.len() > MAX_ARCHIVE_BYTES {
        return Err(Error::new("the package is larger than the size limit"));
    }
    let file = fs::File::open(archive_path)
        .map_err(|e| Error::new(format!("could not open the package: {e}")))?;
    let archive = zip::ZipArchive::new(file)
        .map_err(|e| Error::new(format!("the package is not a valid ZIP archive: {e}")))?;
    if archive.len() > MAX_ENTRIES {
        return Err(Error::new("the package has too many entries"));
    }
    Ok(archive)
}

/// Finds the manifest entry index: a root `manifest.json`, or the shallowest nested one.
fn manifest_index(archive: &mut zip::ZipArchive<fs::File>) -> Option<usize> {
    let mut best: Option<(usize, usize)> = None; // (depth, index)
    for index in 0..archive.len() {
        let Ok(file) = archive.by_index(index) else {
            continue;
        };
        let name = file.name().replace('\\', "/");
        if name == MANIFEST_NAME || name.ends_with(&format!("/{MANIFEST_NAME}")) {
            let depth = name.matches('/').count();
            if best.map(|(d, _)| depth < d).unwrap_or(true) {
                best = Some((depth, index));
            }
        }
    }
    best.map(|(_, index)| index)
}

/// Reads and validates the manifest straight out of an archive.
pub fn read_manifest(archive_path: &str) -> Result<UniverseManifest> {
    let text = read_manifest_text(archive_path)?;
    manifest::parse(&text)
}

/// Reads the manifest entry's raw text out of an archive.
///
/// Shared with the `.singularity` engine, whose manifest is a superset of the `.universe` one: it
/// reads the same entry, then parses it with its own schema.
pub fn read_manifest_text(archive_path: &str) -> Result<String> {
    let mut archive = open_archive(archive_path)?;
    let index = manifest_index(&mut archive)
        .ok_or_else(|| Error::new(format!("the package has no {MANIFEST_NAME}")))?;
    let mut file = archive
        .by_index(index)
        .map_err(|e| Error::new(format!("could not read {MANIFEST_NAME}: {e}")))?;
    if file.size() > 4 * 1024 * 1024 {
        return Err(Error::new("the manifest is implausibly large"));
    }
    let mut text = String::new();
    file.read_to_string(&mut text)
        .map_err(|e| Error::new(format!("could not read {MANIFEST_NAME}: {e}")))?;
    Ok(text)
}

/// Extracts an archive into `<sandbox_root>/<id>/`, returning the mod's directory.
///
/// The archive is staged as `<sandbox_root>/.<id>.extracting` and renamed into place only when the
/// whole package has been written and every declared native library is present, so a partial or
/// hostile archive leaves no loadable mod behind.
///
/// The id and native-library list are read from the manifest with a minimal header schema, so the
/// same extractor serves `.universe` (whose manifest requires `native_libs`) and `.singularity`
/// (whose manifest is a superset and may declare no native libraries at all).
pub fn extract(archive_path: &str, sandbox_root: &str) -> Result<PathBuf> {
    #[derive(serde::Deserialize)]
    struct Header {
        id: String,
        #[serde(default)]
        native_libs: Vec<String>,
    }
    let text = read_manifest_text(archive_path)?;
    let header: Header = serde_json::from_str(&text)
        .map_err(|e| Error::new(format!("{MANIFEST_NAME} has no usable id: {e}")))?;
    crate::manifest::validate_id(&header.id)?;
    extract_with(archive_path, sandbox_root, &header.id, &header.native_libs)
}

/// Extracts an archive into `<sandbox_root>/<id>/`, requiring each of `native_libs` to be present.
pub fn extract_with(
    archive_path: &str,
    sandbox_root: &str,
    id: &str,
    native_libs: &[String],
) -> Result<PathBuf> {
    extract_with_limit(
        archive_path,
        sandbox_root,
        id,
        native_libs,
        MAX_UNCOMPRESSED_BYTES,
        &|| false,
    )
}

/// [`extract`] that stops early when `is_cancelled` returns true.
///
/// The cancellation flag the C ABI/JNI exposes was read by nothing, so "cancel" did not cancel an
/// in-progress extraction. The check runs between entries and the staging directory is removed on
/// a cancel, so no partial install is left behind.
pub fn extract_cancellable(
    archive_path: &str,
    sandbox_root: &str,
    is_cancelled: &dyn Fn() -> bool,
) -> Result<PathBuf> {
    #[derive(serde::Deserialize)]
    struct Header {
        id: String,
        #[serde(default)]
        native_libs: Vec<String>,
    }
    let text = read_manifest_text(archive_path)?;
    let header: Header = serde_json::from_str(&text)
        .map_err(|e| Error::new(format!("{MANIFEST_NAME} has no usable id: {e}")))?;
    crate::manifest::validate_id(&header.id)?;
    extract_with_limit(
        archive_path,
        sandbox_root,
        &header.id,
        &header.native_libs,
        MAX_UNCOMPRESSED_BYTES,
        is_cancelled,
    )
}

/// Copies `reader` into `destination`, streaming rather than buffering the whole entry in memory.
fn copy_to_file<R: Read>(reader: &mut R, destination: &Path) -> std::io::Result<u64> {
    let mut out = fs::File::create(destination)?;
    std::io::copy(reader, &mut out)
}

/// [`extract_with`] with an explicit uncompressed-size budget, so the bound is testable without a
/// 256 MB fixture, and a cancellation check so a long extraction can be stopped.
fn extract_with_limit(
    archive_path: &str,
    sandbox_root: &str,
    id: &str,
    native_libs: &[String],
    limit: u64,
    is_cancelled: &dyn Fn() -> bool,
) -> Result<PathBuf> {
    crate::manifest::validate_id(id)?;
    let mut archive = open_archive(archive_path)?;

    let root = PathBuf::from(sandbox_root);
    fs::create_dir_all(&root)
        .map_err(|e| Error::new(format!("could not create the sandbox: {e}")))?;
    let stage = root.join(format!(".{id}.extracting"));
    let target = root.join(id);
    let _ = fs::remove_dir_all(&stage);
    fs::create_dir_all(&stage)
        .map_err(|e| Error::new(format!("could not create the staging directory: {e}")))?;

    let mut total: u64 = 0;
    for index in 0..archive.len() {
        if is_cancelled() {
            let _ = fs::remove_dir_all(&stage);
            return Err(Error::new("the extraction was cancelled"));
        }
        let mut file = archive
            .by_index(index)
            .map_err(|e| Error::new(format!("could not read an archive entry: {e}")))?;
        let name = file.name().to_string();
        if name.is_empty() {
            continue;
        }
        let is_dir = file.is_dir() || name.ends_with('/');
        let destination = match resolve_within(&stage, &name) {
            Ok(path) => path,
            Err(error) => {
                let _ = fs::remove_dir_all(&stage);
                return Err(error);
            }
        };
        if is_dir {
            if let Err(e) = fs::create_dir_all(&destination) {
                let _ = fs::remove_dir_all(&stage);
                return Err(Error::new(format!("could not create a directory: {e}")));
            }
            continue;
        }
        if let Some(parent) = destination.parent() {
            if let Err(e) = fs::create_dir_all(parent) {
                let _ = fs::remove_dir_all(&stage);
                return Err(Error::new(format!("could not create a directory: {e}")));
            }
        }
        // Bound the write by the *remaining* budget, not the entry's declared size: that size is
        // attacker-controlled, so a package that lies about it must still be stopped. The entry is
        // streamed to disk through `take(remaining + 1)`, so at most one byte past the budget is
        // ever produced and an oversized entry is refused without buffering it whole.
        let remaining = limit.saturating_sub(total);
        let mut limited = (&mut file).take(remaining + 1);
        let written = match copy_to_file(&mut limited, &destination) {
            Ok(bytes) => bytes,
            Err(e) => {
                let _ = fs::remove_dir_all(&stage);
                return Err(Error::new(format!("could not decompress {name}: {e}")));
            }
        };
        if written > remaining {
            let _ = fs::remove_dir_all(&stage);
            return Err(Error::new("the package expands beyond the size limit"));
        }
        total += written;
    }

    // Every declared native library must be present, or the package is refused rather than loaded
    // with a missing entry point that would fault at dlopen time.
    for lib in native_libs {
        if !stage.join(lib).is_file() {
            let _ = fs::remove_dir_all(&stage);
            return Err(Error::new(format!(
                "native_libs names {lib}, which the package does not contain"
            )));
        }
    }

    // Replace the old install without ever leaving the mod missing: move the current install aside
    // first, then move the new one in, then drop the old. The old sequence (`remove_dir_all` then
    // `rename`) deleted a working mod before the new one was in place, so a failed rename lost it.
    let backup = root.join(format!(".{id}.old"));
    let _ = fs::remove_dir_all(&backup);
    if target.exists() {
        if let Err(e) = fs::rename(&target, &backup) {
            let _ = fs::remove_dir_all(&stage);
            return Err(Error::new(format!(
                "could not move the existing mod aside: {e}"
            )));
        }
    }
    if let Err(e) = fs::rename(&stage, &target) {
        // Put the old install back, so a failed update leaves the previous mod in place rather than
        // deleting it.
        let _ = fs::rename(&backup, &target);
        let _ = fs::remove_dir_all(&stage);
        return Err(Error::new(format!(
            "could not move the extracted mod into place: {e}"
        )));
    }
    let _ = fs::remove_dir_all(&backup);
    Ok(target)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_relative_paths_are_accepted() {
        assert!(is_safe_relative_path("manifest.json"));
        assert!(is_safe_relative_path("lib/libexample.so"));
        assert!(is_safe_relative_path("assets/textures/a.png"));
    }

    #[test]
    fn escaping_paths_are_refused() {
        assert!(!is_safe_relative_path(""));
        assert!(!is_safe_relative_path("../etc/passwd"));
        assert!(!is_safe_relative_path("lib/../../x.so"));
        assert!(!is_safe_relative_path("/etc/passwd"));
        assert!(!is_safe_relative_path("\\windows\\system32"));
        assert!(!is_safe_relative_path("C:/windows"));
        assert!(!is_safe_relative_path("a\0b"));
    }

    #[test]
    fn absolute_paths_are_checked() {
        assert!(is_safe_absolute_path(
            "/data/user/0/org.chimeramc.client/files/universe"
        ));
        assert!(!is_safe_absolute_path("relative/path"));
        assert!(!is_safe_absolute_path("/data/../etc"));
        assert!(!is_safe_absolute_path(""));
    }

    #[test]
    fn sandbox_root_must_live_under_the_allowed_root() {
        let allowed = "/data/user/0/org.chimeramc.client/files";
        assert!(
            validate_sandbox_root("/data/user/0/org.chimeramc.client/files/universe", allowed)
                .is_ok()
        );
        assert!(validate_sandbox_root("/sdcard/universe", allowed).is_err());
        assert!(
            validate_sandbox_root("/data/user/0/org.chimeramc.client/files2/x", allowed).is_err()
        );
        assert!(validate_sandbox_root("relative", allowed).is_err());
    }

    #[test]
    fn resolve_within_rejects_traversal() {
        let root = Path::new("/tmp/sandbox");
        assert!(resolve_within(root, "lib/x.so").is_ok());
        assert!(resolve_within(root, "../x.so").is_err());
        assert!(resolve_within(root, "/etc/passwd").is_err());
    }

    #[test]
    fn lexically_within_is_component_wise() {
        assert!(lexically_within(Path::new("/a/b"), Path::new("/a/b/c")));
        assert!(lexically_within(Path::new("/a/b"), Path::new("/a/b")));
        assert!(!lexically_within(Path::new("/a/b"), Path::new("/a/bc")));
        assert!(!lexically_within(Path::new("/a/b"), Path::new("/a")));
    }

    fn temp_dir(name: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!(
            "universe-archive-test-{name}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// Writes a zip holding `manifest.json` (with `native_libs` naming `lib/x.so`) and an entry
    /// `lib/x.so` of `payload` bytes.
    fn write_archive(dir: &Path, payload: &[u8]) -> PathBuf {
        let path = dir.join("package.universe");
        let file = fs::File::create(&path).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let options: zip::write::FileOptions<()> = zip::write::FileOptions::default();
        let manifest = r#"{"id":"example","version":"1.0.0","min_client_version":"1.0.0","native_libs":["lib/x.so"],"berry_api_dependencies":[],"hooks":[]}"#;
        zip.start_file("manifest.json", options).unwrap();
        std::io::Write::write_all(&mut zip, manifest.as_bytes()).unwrap();
        zip.start_file("lib/x.so", options).unwrap();
        std::io::Write::write_all(&mut zip, payload).unwrap();
        zip.finish().unwrap();
        path
    }

    #[test]
    fn an_oversized_entry_is_refused_and_leaves_no_staging_behind() {
        // The declared size is attacker-controlled, so the bound must be enforced on the bytes that
        // are actually produced, not on what the archive claims. A 4 KB entry with a 1 KB budget is
        // the smallest version of the zip bomb the old check let through.
        let dir = temp_dir("bomb");
        let archive = write_archive(&dir, &vec![0u8; 4096]);
        let sandbox = dir.join("sandbox");
        let error = extract_with_limit(
            archive.to_str().unwrap(),
            sandbox.to_str().unwrap(),
            "example",
            &["lib/x.so".to_string()],
            1024,
            &|| false,
        )
        .unwrap_err();
        assert!(
            error.to_string().contains("size limit"),
            "the refusal should name the size limit: {error}"
        );
        assert!(
            !sandbox.join("example").exists(),
            "a refused package must leave no extracted mod behind"
        );
        assert!(
            !sandbox.join(".example.extracting").exists(),
            "a refused package must leave no staging directory behind"
        );
    }

    #[test]
    fn a_package_within_the_budget_extracts() {
        let dir = temp_dir("ok");
        let archive = write_archive(&dir, &vec![7u8; 512]);
        let sandbox = dir.join("sandbox");
        let target = extract_with_limit(
            archive.to_str().unwrap(),
            sandbox.to_str().unwrap(),
            "example",
            &["lib/x.so".to_string()],
            1024,
            &|| false,
        )
        .unwrap();
        assert_eq!(fs::read(target.join("lib/x.so")).unwrap().len(), 512);
    }

    #[test]
    fn a_second_extract_replaces_the_first_without_a_gap() {
        // The old sequence removed the existing install before renaming the new one in, so a failed
        // rename lost the mod. Re-extracting the same id must leave a complete install either way.
        let dir = temp_dir("replace");
        let sandbox = dir.join("sandbox");
        let first = write_archive(&dir, &vec![1u8; 256]);
        let target = extract_with_limit(
            first.to_str().unwrap(),
            sandbox.to_str().unwrap(),
            "example",
            &["lib/x.so".to_string()],
            1024,
            &|| false,
        )
        .unwrap();
        assert_eq!(fs::read(target.join("lib/x.so")).unwrap().len(), 256);

        let second = dir.join("package2.universe");
        {
            let file = fs::File::create(&second).unwrap();
            let mut zip = zip::ZipWriter::new(file);
            let options: zip::write::FileOptions<()> = zip::write::FileOptions::default();
            let manifest = r#"{"id":"example","version":"2.0.0","min_client_version":"1.0.0","native_libs":["lib/x.so"],"berry_api_dependencies":[],"hooks":[]}"#;
            zip.start_file("manifest.json", options).unwrap();
            std::io::Write::write_all(&mut zip, manifest.as_bytes()).unwrap();
            zip.start_file("lib/x.so", options).unwrap();
            std::io::Write::write_all(&mut zip, &vec![2u8; 128]).unwrap();
            zip.finish().unwrap();
        }
        let target = extract_with_limit(
            second.to_str().unwrap(),
            sandbox.to_str().unwrap(),
            "example",
            &["lib/x.so".to_string()],
            1024,
            &|| false,
        )
        .unwrap();
        assert_eq!(fs::read(target.join("lib/x.so")).unwrap().len(), 128);
        assert!(
            !sandbox.join(".example.old").exists(),
            "the previous install must not be left behind as a backup"
        );
        assert!(
            !sandbox.join(".example.extracting").exists(),
            "no staging directory may survive a successful extract"
        );
    }

    #[test]
    fn a_cancelled_extraction_stops_and_leaves_nothing_behind() {
        let dir = temp_dir("cancel");
        let archive = write_archive(&dir, &vec![3u8; 512]);
        let sandbox = dir.join("sandbox");
        let error = extract_with_limit(
            archive.to_str().unwrap(),
            sandbox.to_str().unwrap(),
            "example",
            &["lib/x.so".to_string()],
            4096,
            &|| true,
        )
        .unwrap_err();
        assert!(
            error.to_string().contains("cancelled"),
            "the refusal should name the cancellation: {error}"
        );
        assert!(
            !sandbox.join("example").exists(),
            "a cancelled package must leave no extracted mod behind"
        );
        assert!(
            !sandbox.join(".example.extracting").exists(),
            "a cancelled package must leave no staging directory behind"
        );
    }
}
