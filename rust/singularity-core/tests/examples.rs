//! Validates the shipped example packs against the real manifest/definition parsers.
//!
//! The examples are the documentation's worked samples, so a stale field in one is a doc bug that
//! only a test catches: this loads `examples/sky_islands/manifest.json`, its `biomes/*.json` and its
//! `structures/*.json` through the same code the engine uses on a device, and fails the build when
//! any of them stops parsing. It also asserts the manifest's declared biome/structure lists match
//! the files actually present, so a pack cannot ship a definition the manifest never names.

use std::fs;
use std::path::PathBuf;

fn example_root() -> PathBuf {
    // CARGO_MANIFEST_DIR is rust/singularity-core; the examples live two levels up.
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("examples")
}

#[test]
fn the_sky_islands_pack_parses() {
    let root = example_root().join("sky_islands");
    let manifest_text = fs::read_to_string(root.join("manifest.json")).expect("manifest.json");
    let manifest = singularity_core::manifest::parse(&manifest_text).expect("a valid manifest");
    assert_eq!(manifest.id, "sky_islands");
    assert_eq!(manifest.seed_algorithm, singularity_core::manifest::SeedAlgorithm::Sha256);

    let mut biome_ids = Vec::new();
    for entry in fs::read_dir(root.join("biomes")).expect("biomes/") {
        let path = entry.expect("a directory entry").path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let biome = singularity_core::biome::parse_definition(&fs::read_to_string(&path).unwrap())
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        biome_ids.push(biome.id);
    }
    biome_ids.sort();
    let mut declared = manifest.biomes.clone();
    declared.sort();
    assert_eq!(biome_ids, declared, "biomes/ and the manifest must agree");

    let mut structure_ids = Vec::new();
    for entry in fs::read_dir(root.join("structures")).expect("structures/") {
        let path = entry.expect("a directory entry").path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let rule = singularity_core::structure::parse_definition(&fs::read_to_string(&path).unwrap())
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        structure_ids.push(rule.id);
    }
    structure_ids.sort();
    let mut declared = manifest.structures.clone();
    declared.sort();
    assert_eq!(structure_ids, declared, "structures/ and the manifest must agree");
}
