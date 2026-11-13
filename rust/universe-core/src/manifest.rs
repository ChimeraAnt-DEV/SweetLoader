//! The `.universe` manifest: parsing and validation.
//!
//! A `.universe` package is a ZIP whose root holds `manifest.json`. The manifest is the whole
//! contract between a mod author and the loader, so every field the loader acts on is required and
//! validated here: the loader refuses a package it cannot fully understand rather than loading a
//! half-checked mod into the game process.
//!
//! The required keys are `id`, `version`, `min_client_version`, `native_libs`,
//! `berry_api_dependencies` and `hooks`. `dependencies` (load ordering) and `permissions` (the
//! sandbox grant) are optional.

use serde::{Deserialize, Serialize};

use crate::permission::Permission;
use crate::version;
use crate::{Error, Result, SUPPORTED_ABI};

/// The manifest file name inside a `.universe` archive.
pub const MANIFEST_NAME: &str = "manifest.json";

/// The `.universe` file extension, lower-case.
pub const EXTENSION: &str = ".universe";

/// The game module a hook targets when a hook declaration does not name one.
pub const DEFAULT_MODULE: &str = "libminecraftpe.so";

const MAX_ID_LEN: usize = 128;
const MAX_PATH_LEN: usize = 1024;
const MAX_NATIVE_LIBS: usize = 64;
const MAX_HOOKS: usize = 256;

/// One hook a mod asks the loader to install.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HookDeclaration {
    /// The byte signature to resolve, using the rule-file syntax (`..` wildcards).
    pub signature: String,
    /// The module to scan; a basename, never a path.
    #[serde(default = "default_module")]
    pub module: String,
    /// A human-readable name for the hook, used in logs.
    #[serde(default)]
    pub name: String,
}

fn default_module() -> String {
    DEFAULT_MODULE.to_string()
}

/// A parsed and validated `.universe` manifest.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UniverseManifest {
    pub id: String,
    #[serde(default)]
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub author: String,
    pub min_client_version: String,
    #[serde(default = "default_abi")]
    pub abi: String,
    pub native_libs: Vec<String>,
    pub berry_api_dependencies: Vec<String>,
    pub hooks: Vec<HookDeclaration>,
    /// Other mod ids this mod must load after. Used to order the load; an absent dependency is not
    /// an error.
    #[serde(default)]
    pub dependencies: Vec<String>,
    /// The capabilities the mod declares. Validated against the known permission set.
    #[serde(default)]
    pub permissions: Vec<String>,
}

fn default_abi() -> String {
    SUPPORTED_ABI.to_string()
}

impl UniverseManifest {
    /// The display name, falling back to the id when the author gave none.
    pub fn display_name(&self) -> &str {
        if self.name.is_empty() {
            &self.id
        } else {
            &self.name
        }
    }

    /// The parsed permissions, or an error naming the first unknown one.
    pub fn permissions(&self) -> Result<Vec<Permission>> {
        self.permissions
            .iter()
            .map(|name| Permission::parse(name))
            .collect()
    }
}

/// Parses and validates manifest JSON.
pub fn parse(json: &str) -> Result<UniverseManifest> {
    let manifest: UniverseManifest = serde_json::from_str(json)
        .map_err(|e| Error::new(format!("{MANIFEST_NAME} is not a valid universe manifest: {e}")))?;
    validate(&manifest)?;
    Ok(manifest)
}

/// Serializes a validated manifest back to JSON, for the launcher's mod list.
pub fn to_json(manifest: &UniverseManifest) -> String {
    serde_json::to_string(manifest).unwrap_or_else(|_| "{}".to_string())
}

/// Validates every field the loader depends on, naming the first failure.
pub fn validate(manifest: &UniverseManifest) -> Result<()> {
    validate_id(&manifest.id)?;
    if !manifest.name.is_empty() && manifest.name.len() > MAX_ID_LEN {
        return Err(Error::new("name is too long"));
    }
    version::Version::parse(&manifest.version)
        .map_err(|e| Error::new(format!("version: {e}")))?;
    version::Version::parse(&manifest.min_client_version)
        .map_err(|e| Error::new(format!("min_client_version: {e}")))?;
    if manifest.abi != SUPPORTED_ABI {
        return Err(Error::new(format!(
            "abi '{}' is not {SUPPORTED_ABI}; only {SUPPORTED_ABI} packages load",
            manifest.abi
        )));
    }
    if manifest.native_libs.is_empty() {
        return Err(Error::new("native_libs must list at least one .so file"));
    }
    if manifest.native_libs.len() > MAX_NATIVE_LIBS {
        return Err(Error::new("native_libs lists too many libraries"));
    }
    for lib in &manifest.native_libs {
        validate_native_lib(lib)?;
    }
    for dependency in &manifest.dependencies {
        validate_id(dependency)
            .map_err(|e| Error::new(format!("dependency: {e}")))?;
    }
    if manifest.hooks.len() > MAX_HOOKS {
        return Err(Error::new("hooks lists too many declarations"));
    }
    for hook in &manifest.hooks {
        if hook.signature.trim().is_empty() {
            return Err(Error::new("a hook declaration has an empty signature"));
        }
        if !is_safe_segment(&hook.module) {
            return Err(Error::new(format!(
                "hook module '{}' is not a plain module name",
                hook.module
            )));
        }
    }
    // The permission set is validated eagerly, so an unknown capability is a load-time refusal
    // rather than a silent grant the first time the mod calls into the API.
    manifest.permissions()?;
    Ok(())
}

/// True when the file name ends with the `.universe` extension (case-insensitive).
pub fn looks_like_universe(file_name: &str) -> bool {
    let lower = file_name.to_ascii_lowercase();
    lower.len() > EXTENSION.len() && lower.ends_with(EXTENSION)
}

/// A mod id: a single safe path segment, so it can name a sandbox directory directly.
pub fn validate_id(id: &str) -> Result<()> {
    if id.is_empty() {
        return Err(Error::new("id is empty"));
    }
    if id.len() > MAX_ID_LEN {
        return Err(Error::new("id is too long"));
    }
    if !is_safe_segment(id) {
        return Err(Error::new(format!(
            "id '{id}' must be a single path segment of letters, digits, '.', '_' or '-'"
        )));
    }
    Ok(())
}

/// True when `text` is a safe single path segment: non-empty, not `.`/`..`, no separator, no NUL.
pub fn is_safe_segment(text: &str) -> bool {
    if text.is_empty() || text == "." || text == ".." {
        return false;
    }
    text.chars().all(|c| {
        c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-') || c == ' '
    }) && !text.contains(['/', '\\', '\0'])
}

/// Validates a native library path: a safe relative `.so` path inside the package.
fn validate_native_lib(path: &str) -> Result<()> {
    if path.is_empty() {
        return Err(Error::new("native_libs contains an empty path"));
    }
    if path.len() > MAX_PATH_LEN {
        return Err(Error::new(format!("native lib path '{path}' is too long")));
    }
    if path.contains('\0') || path.contains('\\') {
        return Err(Error::new(format!("native lib path '{path}' has an invalid character")));
    }
    if path.starts_with('/') {
        return Err(Error::new(format!("native lib path '{path}' must be relative")));
    }
    if path.split('/').any(|segment| segment == "..") {
        return Err(Error::new(format!("native lib path '{path}' escapes the package")));
    }
    if !path.to_ascii_lowercase().ends_with(".so") {
        return Err(Error::new(format!("native lib '{path}' is not a .so file")));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_json(extra: &str) -> String {
        let mut json = String::from(
            r#"{
                "id": "example-mod",
                "name": "Example Mod",
                "version": "1.2.3",
                "author": "Someone",
                "min_client_version": "1.0.0",
                "native_libs": ["lib/libexample.so"],
                "berry_api_dependencies": ["cosmetic"],
                "hooks": [{"signature": "1f 20 03 d5", "module": "libminecraftpe.so", "name": "tick"}]
            }"#,
        );
        if !extra.is_empty() {
            let insert = json.rfind('}').unwrap();
            json.insert_str(insert, &format!(",{extra}"));
        }
        json
    }

    #[test]
    fn parses_a_valid_manifest() {
        let manifest = parse(&valid_json("")).unwrap();
        assert_eq!(manifest.id, "example-mod");
        assert_eq!(manifest.display_name(), "Example Mod");
        assert_eq!(manifest.abi, SUPPORTED_ABI);
        assert_eq!(manifest.native_libs, vec!["lib/libexample.so"]);
        assert_eq!(manifest.hooks.len(), 1);
        assert_eq!(manifest.hooks[0].module, "libminecraftpe.so");
    }

    #[test]
    fn display_name_falls_back_to_id() {
        let manifest = parse(&valid_json("")).unwrap();
        let mut manifest = manifest;
        manifest.name = String::new();
        assert_eq!(manifest.display_name(), "example-mod");
    }

    #[test]
    fn refuses_a_missing_required_field() {
        for field in [
            "\"id\"",
            "\"version\"",
            "\"min_client_version\"",
            "\"native_libs\"",
            "\"berry_api_dependencies\"",
            "\"hooks\"",
        ] {
            let json = valid_json("");
            let mut value: serde_json::Value = serde_json::from_str(&json).unwrap();
            value.as_object_mut().unwrap().remove(&field[1..field.len() - 1]);
            let text = value.to_string();
            assert!(parse(&text).is_err(), "missing {field} should be refused");
        }
    }

    #[test]
    fn refuses_a_wrong_abi() {
        assert!(parse(&valid_json(r#""abi": "armeabi-v7a""#)).is_err());
        assert!(parse(&valid_json(r#""abi": "arm64-v8a""#)).is_ok());
    }

    #[test]
    fn refuses_an_escaping_native_lib() {
        assert!(parse(&valid_json(r#""native_libs": ["../lib/x.so"]"#)).is_err());
        assert!(parse(&valid_json(r#""native_libs": ["/lib/x.so"]"#)).is_err());
        assert!(parse(&valid_json(r#""native_libs": ["lib/x.txt"]"#)).is_err());
        assert!(parse(&valid_json(r#""native_libs": []"#)).is_err());
    }

    #[test]
    fn refuses_a_traversing_id() {
        assert!(parse(&valid_json("")).is_ok());
        assert!(parse(&valid_json(r#""id": "../etc""#)).is_err());
        assert!(parse(&valid_json(r#""id": "a/b""#)).is_err());
        assert!(parse(&valid_json(r#""id": "..""#)).is_err());
    }

    #[test]
    fn refuses_an_unknown_permission() {
        assert!(parse(&valid_json(r#""permissions": ["render_hook"]"#)).is_ok());
        assert!(parse(&valid_json(r#""permissions": ["become_root"]"#)).is_err());
    }

    #[test]
    fn refuses_a_non_version_min_client_version() {
        assert!(parse(&valid_json(r#""min_client_version": "soon""#)).is_err());
    }

    #[test]
    fn detects_the_extension_case_insensitively() {
        assert!(looks_like_universe("mod.universe"));
        assert!(looks_like_universe("MOD.UNIVERSE"));
        assert!(!looks_like_universe("mod.zip"));
        assert!(!looks_like_universe("universe"));
    }
}
