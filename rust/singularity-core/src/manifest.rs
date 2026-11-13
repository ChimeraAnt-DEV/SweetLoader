//! The `.singularity` manifest: parsing and validation.
//!
//! A `.singularity` pack is a ZIP whose root holds `manifest.json`, `biomes/`, `structures/` and
//! `lib/`. It shares the archive, signature, version and permission rules with `.universe` (they
//! live in `universe-core`), and adds the world-generation fields: `seed_algorithm`,
//! `custom_dimensions`, the biome and structure lists and the physics environment.
//!
//! Every field the engine acts on is required and validated here, so the engine refuses a pack it
//! cannot fully understand rather than generating half a world from it.

use serde::{Deserialize, Serialize};

use crate::noise::MAX_OCTAVES;
use universe_core::manifest::is_safe_segment;
use universe_core::{version, Error, Result, SUPPORTED_ABI};

/// The manifest file name inside a `.singularity` archive.
pub const MANIFEST_NAME: &str = "manifest.json";

/// The `.singularity` file extension, lower-case.
pub const EXTENSION: &str = ".singularity";

/// The default terrain height when a pack does not override it.
pub const DEFAULT_HEIGHT: i32 = 128;

/// A seed algorithm a pack may name. Only the deterministic ones are accepted; the engine refuses a
/// pack whose seed handling it cannot reproduce on every client.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SeedAlgorithm {
    /// SHA-256 of the world seed string, folded to 64 bits. Deterministic across devices.
    Sha256,
    /// A 64-bit FNV-1a of the seed string. Deterministic, cheaper, weaker.
    Fnv1a,
}

/// A custom dimension the pack defines.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Dimension {
    pub id: String,
    #[serde(default)]
    pub sky_color: Option<String>,
    #[serde(default)]
    pub fog_color: Option<String>,
    #[serde(default)]
    pub ambient_sound: Option<String>,
}

/// The physics environment the pack overrides.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PhysicsConfig {
    #[serde(default = "default_gravity")]
    pub gravity: [f32; 3],
    #[serde(default)]
    pub wind: [f32; 3],
    #[serde(default)]
    pub wind_altitude_gain: f32,
    #[serde(default = "default_wind_base")]
    pub wind_base_altitude: f32,
}

fn default_gravity() -> [f32; 3] {
    [0.0, -9.8, 0.0]
}

fn default_wind_base() -> f32 {
    64.0
}

impl Default for PhysicsConfig {
    fn default() -> Self {
        PhysicsConfig {
            gravity: default_gravity(),
            wind: [0.0, 0.0, 0.0],
            wind_altitude_gain: 0.0,
            wind_base_altitude: default_wind_base(),
        }
    }
}

/// The terrain generation settings.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TerrainConfig {
    #[serde(default = "default_base_height")]
    pub base_height: i32,
    #[serde(default = "default_amplitude")]
    pub amplitude: i32,
    #[serde(default = "default_sea_level")]
    pub sea_level: i32,
    #[serde(default = "default_height")]
    pub height: i32,
    #[serde(default = "default_octaves")]
    pub octaves: u32,
    #[serde(default)]
    pub warp: f32,
    #[serde(default = "default_biome")]
    pub default_biome: String,
}

fn default_base_height() -> i32 {
    64
}
fn default_amplitude() -> i32 {
    32
}
fn default_sea_level() -> i32 {
    62
}
fn default_height() -> i32 {
    DEFAULT_HEIGHT
}
fn default_octaves() -> u32 {
    5
}
fn default_biome() -> String {
    "plains".to_string()
}

impl Default for TerrainConfig {
    fn default() -> Self {
        TerrainConfig {
            base_height: default_base_height(),
            amplitude: default_amplitude(),
            sea_level: default_sea_level(),
            height: default_height(),
            octaves: default_octaves(),
            warp: 0.0,
            default_biome: default_biome(),
        }
    }
}

/// A parsed and validated `.singularity` manifest.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SingularityManifest {
    pub id: String,
    #[serde(default)]
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub author: String,
    pub min_client_version: String,
    #[serde(default = "default_abi")]
    pub abi: String,
    pub seed_algorithm: SeedAlgorithm,
    pub custom_dimensions: Vec<Dimension>,
    pub berry_api_dependencies: Vec<String>,
    #[serde(default)]
    pub native_libs: Vec<String>,
    #[serde(default)]
    pub biomes: Vec<String>,
    #[serde(default)]
    pub structures: Vec<String>,
    #[serde(default)]
    pub terrain: TerrainConfig,
    #[serde(default)]
    pub physics: PhysicsConfig,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub permissions: Vec<String>,
}

fn default_abi() -> String {
    SUPPORTED_ABI.to_string()
}

impl SingularityManifest {
    pub fn display_name(&self) -> &str {
        if self.name.is_empty() {
            &self.id
        } else {
            &self.name
        }
    }

    pub fn permissions(&self) -> Result<Vec<universe_core::permission::Permission>> {
        self.permissions
            .iter()
            .map(|name| universe_core::permission::Permission::parse(name))
            .collect()
    }
}

/// Parses and validates manifest JSON.
pub fn parse(json: &str) -> Result<SingularityManifest> {
    let manifest: SingularityManifest = serde_json::from_str(json).map_err(|e| {
        Error::new(format!("{MANIFEST_NAME} is not a valid singularity manifest: {e}"))
    })?;
    validate(&manifest)?;
    Ok(manifest)
}

/// Serializes a validated manifest back to JSON.
pub fn to_json(manifest: &SingularityManifest) -> String {
    serde_json::to_string(manifest).unwrap_or_else(|_| "{}".to_string())
}

/// Validates every field the engine depends on, naming the first failure.
pub fn validate(manifest: &SingularityManifest) -> Result<()> {
    universe_core::manifest::validate_id(&manifest.id)?;
    version::Version::parse(&manifest.version).map_err(|e| Error::new(format!("version: {e}")))?;
    version::Version::parse(&manifest.min_client_version)
        .map_err(|e| Error::new(format!("min_client_version: {e}")))?;
    if manifest.abi != SUPPORTED_ABI {
        return Err(Error::new(format!(
            "abi '{}' is not {SUPPORTED_ABI}",
            manifest.abi
        )));
    }
    for dimension in &manifest.custom_dimensions {
        if !is_safe_segment(&dimension.id) {
            return Err(Error::new(format!(
                "dimension id '{}' is not a safe identifier",
                dimension.id
            )));
        }
    }
    for dependency in &manifest.dependencies {
        universe_core::manifest::validate_id(dependency)
            .map_err(|e| Error::new(format!("dependency: {e}")))?;
    }
    if manifest.terrain.height < 16 || manifest.terrain.height > 2048 {
        return Err(Error::new(
            "terrain.height must be between 16 and 2048",
        ));
    }
    if manifest.terrain.amplitude < 0 {
        return Err(Error::new("terrain.amplitude must not be negative"));
    }
    if manifest.terrain.base_height < 0 || manifest.terrain.base_height >= manifest.terrain.height {
        return Err(Error::new(
            "terrain.base_height must be inside the chunk height",
        ));
    }
    if manifest.terrain.sea_level < 0 || manifest.terrain.sea_level > manifest.terrain.height {
        return Err(Error::new("terrain.sea_level must be inside the chunk height"));
    }
    if !(1..=MAX_OCTAVES).contains(&manifest.terrain.octaves) {
        return Err(Error::new("terrain.octaves is out of range"));
    }
    if !manifest.terrain.warp.is_finite() || manifest.terrain.warp < 0.0 {
        return Err(Error::new("terrain.warp must be a non-negative finite number"));
    }
    if manifest.physics.gravity.iter().any(|v| !v.is_finite()) {
        return Err(Error::new("physics.gravity must be finite"));
    }
    if manifest.physics.wind.iter().any(|v| !v.is_finite()) {
        return Err(Error::new("physics.wind must be finite"));
    }
    manifest.permissions()?;
    Ok(())
}

/// True when the file name ends with the `.singularity` extension (case-insensitive).
pub fn looks_like_singularity(file_name: &str) -> bool {
    let lower = file_name.to_ascii_lowercase();
    lower.len() > EXTENSION.len() && lower.ends_with(EXTENSION)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_json(extra: &str) -> String {
        let base = r##"{
                "id": "sky-islands",
                "name": "Sky Islands",
                "version": "1.0.0",
                "min_client_version": "1.0.0",
                "seed_algorithm": "sha256",
                "custom_dimensions": [{"id": "aether", "sky_color": "#88ccff"}],
                "berry_api_dependencies": ["world"],
                "biomes": ["cherry_grove"],
                "structures": ["castle"],
                "terrain": {"base_height": 80, "amplitude": 48, "height": 256, "octaves": 6, "warp": 4.0},
                "physics": {"gravity": [0.0, -3.0, 0.0]}
            }"##;
        let mut value: serde_json::Value = serde_json::from_str(base).unwrap();
        if !extra.is_empty() {
            let extra_value: serde_json::Value = serde_json::from_str(&format!("{{{extra}}}")).unwrap();
            merge(value.as_object_mut().unwrap(), extra_value.as_object().unwrap());
        }
        value.to_string()
    }

    /// Shallow-merges `overrides` into `base`, replacing whole keys (so a test can override
    /// `terrain` without duplicating the key, which serde_json would reject).
    fn merge(
        base: &mut serde_json::Map<String, serde_json::Value>,
        overrides: &serde_json::Map<String, serde_json::Value>,
    ) {
        for (key, value) in overrides {
            base.insert(key.clone(), value.clone());
        }
    }

    #[test]
    fn parses_a_valid_manifest() {
        let manifest = parse(&valid_json("")).unwrap();
        assert_eq!(manifest.id, "sky-islands");
        assert_eq!(manifest.seed_algorithm, SeedAlgorithm::Sha256);
        assert_eq!(manifest.custom_dimensions.len(), 1);
        assert_eq!(manifest.custom_dimensions[0].id, "aether");
        assert_eq!(manifest.terrain.height, 256);
        assert_eq!(manifest.physics.gravity, [0.0, -3.0, 0.0]);
    }

    #[test]
    fn refuses_a_missing_required_field() {
        for field in ["id", "version", "min_client_version", "seed_algorithm", "custom_dimensions", "berry_api_dependencies"] {
            let json = valid_json("");
            let mut value: serde_json::Value = serde_json::from_str(&json).unwrap();
            value.as_object_mut().unwrap().remove(field);
            assert!(parse(&value.to_string()).is_err(), "missing {field} should be refused");
        }
    }

    #[test]
    fn refuses_a_bad_seed_algorithm() {
        assert!(parse(&valid_json(r#""seed_algorithm": "random""#)).is_err());
    }

    #[test]
    fn refuses_an_out_of_range_height() {
        assert!(parse(&valid_json(r#""terrain": {"height": 4}"#)).is_err());
        assert!(parse(&valid_json(r#""terrain": {"height": 100000}"#)).is_err());
        assert!(parse(&valid_json(r#""terrain": {"height": 256}"#)).is_ok());
    }

    #[test]
    fn refuses_a_base_height_outside_the_chunk() {
        assert!(parse(&valid_json(r#""terrain": {"base_height": 500, "height": 128}"#)).is_err());
    }

    #[test]
    fn refuses_a_non_finite_gravity() {
        // A f32 that overflows to infinity is the reachable case: a pack can carry it as a huge
        // JSON number, and the finiteness check must refuse it.
        let mut manifest = parse(&valid_json("")).unwrap();
        manifest.physics.gravity = [0.0, f32::INFINITY, 0.0];
        assert!(validate(&manifest).is_err());
        manifest.physics.gravity = [0.0, -3.0, 0.0];
        assert!(validate(&manifest).is_ok());
    }

    #[test]
    fn refuses_a_bad_dimension_id() {
        assert!(parse(&valid_json(r#""custom_dimensions": [{"id": "../x"}]"#)).is_err());
    }

    #[test]
    fn refuses_a_wrong_abi() {
        assert!(parse(&valid_json(r#""abi": "x86_64""#)).is_err());
    }

    #[test]
    fn detects_the_extension_case_insensitively() {
        assert!(looks_like_singularity("pack.singularity"));
        assert!(looks_like_singularity("PACK.SINGULARITY"));
        assert!(!looks_like_singularity("pack.universe"));
        assert!(!looks_like_singularity("singularity"));
    }
}
