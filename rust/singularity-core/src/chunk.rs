//! Chunk data and deterministic terrain generation.
//!
//! A chunk is a `16 x height x 16` column of block ids. Generation is a pure function of
//! `(seed, chunk coordinates, config)`, so it can run on any thread and on any device and produce
//! the same result. The pipeline is: climate sample -> biome -> heightmap -> block column. The
//! block column is the *only* thing the renderer needs, which keeps the seam to the game small: the
//! loader hands over a flat block array and a height, and the mesher (or the game's own) turns it
//! into geometry.

use crate::biome::{BiomeRegistry, Climate};
use crate::noise;

/// The horizontal size of a chunk, in blocks. Bedrock's is 16.
pub const CHUNK_SIZE: i32 = 16;

/// The block id for air.
pub const AIR: u8 = 0;
/// The block id for stone.
pub const STONE: u8 = 1;
/// The block id for dirt.
pub const DIRT: u8 = 2;
/// The block id for grass.
pub const GRASS: u8 = 3;
/// The block id for water.
pub const WATER: u8 = 4;
/// The block id for sand.
pub const SAND: u8 = 5;
/// The block id for bedrock (the unbreakable floor).
pub const BEDROCK: u8 = 6;
/// The block id for snow.
pub const SNOW: u8 = 7;

/// True when a block id is transparent to the mesher (a face against it is drawn).
pub fn is_transparent(block: u8) -> bool {
    matches!(block, AIR | WATER)
}

/// The terrain generation parameters, from the `.singularity` manifest.
#[derive(Debug, Clone, PartialEq)]
pub struct TerrainConfig {
    pub seed: u64,
    /// The height the terrain oscillates around.
    pub base_height: i32,
    /// The peak-to-trough amplitude of the height noise.
    pub amplitude: i32,
    /// The water level; blocks below it and above the surface are water.
    pub sea_level: i32,
    /// The vertical extent of a chunk.
    pub height: i32,
    pub octaves: u32,
    /// How strongly domain warping distorts the terrain (0 disables it).
    pub warp: f32,
    /// The biome used when the registry claims no climate point.
    pub default_biome: String,
}

impl Default for TerrainConfig {
    fn default() -> Self {
        TerrainConfig {
            seed: 0,
            base_height: 64,
            amplitude: 32,
            sea_level: 62,
            height: 128,
            octaves: 5,
            warp: 0.0,
            default_biome: "plains".to_string(),
        }
    }
}

/// A chunk position in chunk coordinates.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ChunkPos {
    pub x: i32,
    pub z: i32,
}

/// A generated chunk: a flat block array plus its biome and heightmap.
#[derive(Debug, Clone)]
pub struct Chunk {
    pub pos: ChunkPos,
    pub height: i32,
    /// Blocks indexed `x + CHUNK_SIZE * (z + CHUNK_SIZE * y)`.
    pub blocks: Vec<u8>,
    /// The surface height at each column, indexed `x + CHUNK_SIZE * z`.
    pub heightmap: Vec<i32>,
    pub biome: String,
}

impl Chunk {
    /// The index of `(x, y, z)` in the flat array, or `None` when out of range.
    pub fn index(&self, x: i32, y: i32, z: i32) -> Option<usize> {
        if x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || y < 0 || y >= self.height {
            return None;
        }
        Some((x + CHUNK_SIZE * (z + CHUNK_SIZE * y)) as usize)
    }

    /// The block at `(x, y, z)`, or [`AIR`] when out of range.
    pub fn block(&self, x: i32, y: i32, z: i32) -> u8 {
        self.index(x, y, z).map(|i| self.blocks[i]).unwrap_or(AIR)
    }

    /// True when the block at `(x, y, z)` is solid (not air and not water).
    pub fn is_solid(&self, x: i32, y: i32, z: i32) -> bool {
        !is_transparent(self.block(x, y, z))
    }
}

/// The climate at a world column.
pub fn climate_at(seed: u64, world_x: i32, world_z: i32) -> Climate {
    let wx = world_x as f32;
    let wz = world_z as f32;
    Climate {
        temperature: noise::fbm2(seed ^ 0xA1, wx / 2048.0, wz / 2048.0, 3, 2.0, 0.5),
        humidity: noise::fbm2(seed ^ 0xB2, wx / 1536.0, wz / 1536.0, 3, 2.0, 0.5),
        continentalness: noise::fbm2(seed ^ 0xC3, wx / 3072.0, wz / 3072.0, 3, 2.0, 0.5),
    }
}

/// The surface height at a world column.
pub fn surface_height(config: &TerrainConfig, world_x: i32, world_z: i32) -> i32 {
    let wx = world_x as f32;
    let wz = world_z as f32;
    let base = if config.warp > 0.0 {
        noise::warped3(config.seed, wx / 256.0, 0.0, wz / 256.0, config.warp)
    } else {
        noise::fbm2(
            config.seed,
            wx / 256.0,
            wz / 256.0,
            config.octaves,
            2.0,
            0.5,
        )
    };
    let shaped = (base - 0.5) * 2.0 * config.amplitude as f32;
    let height = config.base_height as f32 + shaped;
    height.round().clamp(1.0, (config.height - 2) as f32) as i32
}

/// Generates the chunk at `pos`.
pub fn generate_chunk(config: &TerrainConfig, biomes: &BiomeRegistry, pos: ChunkPos) -> Chunk {
    let mut blocks = vec![AIR; (CHUNK_SIZE * CHUNK_SIZE * config.height) as usize];
    let mut heightmap = vec![0i32; (CHUNK_SIZE * CHUNK_SIZE) as usize];
    // The chunk's biome is the one at its centre column; a finer-grained per-column biome is
    // available through `climate_at` when a pack wants it.
    let centre_x = pos.x * CHUNK_SIZE + CHUNK_SIZE / 2;
    let centre_z = pos.z * CHUNK_SIZE + CHUNK_SIZE / 2;
    let biome = biomes
        .select(climate_at(config.seed, centre_x, centre_z))
        .to_string();

    for local_z in 0..CHUNK_SIZE {
        for local_x in 0..CHUNK_SIZE {
            let world_x = pos.x * CHUNK_SIZE + local_x;
            let world_z = pos.z * CHUNK_SIZE + local_z;
            let surface = surface_height(config, world_x, world_z);
            heightmap[(local_x + CHUNK_SIZE * local_z) as usize] = surface;

            // A thin unbreakable floor, so the world has a bottom.
            set_block(&mut blocks, config.height, local_x, 0, local_z, BEDROCK);
            for y in 1..=surface {
                let block = if y == surface {
                    surface_block(&biome, surface, config)
                } else if y > surface - 4 {
                    DIRT
                } else {
                    STONE
                };
                set_block(&mut blocks, config.height, local_x, y, local_z, block);
            }
            // Water fills from the sea level up to the surface when the surface is below it.
            if surface < config.sea_level {
                for y in (surface + 1)..=config.sea_level {
                    set_block(&mut blocks, config.height, local_x, y, local_z, WATER);
                }
            }
        }
    }

    Chunk {
        pos,
        height: config.height,
        blocks,
        heightmap,
        biome,
    }
}

/// The surface block for a biome, with the sea level forcing sand at a shoreline.
fn surface_block(biome: &str, surface: i32, config: &TerrainConfig) -> u8 {
    if surface <= config.sea_level + 1 {
        return SAND;
    }
    match biome {
        "desert" | "beach" => SAND,
        "tundra" | "snowy_plains" => SNOW,
        _ => GRASS,
    }
}

fn set_block(blocks: &mut [u8], height: i32, x: i32, y: i32, z: i32, block: u8) {
    if x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || y < 0 || y >= height {
        return;
    }
    let index = (x + CHUNK_SIZE * (z + CHUNK_SIZE * y)) as usize;
    if index < blocks.len() {
        blocks[index] = block;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> TerrainConfig {
        TerrainConfig {
            seed: 12345,
            ..TerrainConfig::default()
        }
    }

    fn registry() -> BiomeRegistry {
        BiomeRegistry::new("plains")
    }

    #[test]
    fn generation_is_deterministic() {
        let a = generate_chunk(&config(), &registry(), ChunkPos { x: 3, z: -2 });
        let b = generate_chunk(&config(), &registry(), ChunkPos { x: 3, z: -2 });
        assert_eq!(a.blocks, b.blocks);
        assert_eq!(a.heightmap, b.heightmap);
        assert_eq!(a.biome, b.biome);
    }

    #[test]
    fn different_seeds_generate_differently() {
        let a = generate_chunk(&config(), &registry(), ChunkPos { x: 0, z: 0 });
        let mut other = config();
        other.seed = 999;
        let b = generate_chunk(&other, &registry(), ChunkPos { x: 0, z: 0 });
        assert_ne!(a.heightmap, b.heightmap);
    }

    #[test]
    fn the_floor_is_bedrock_and_air_is_above_the_surface() {
        let chunk = generate_chunk(&config(), &registry(), ChunkPos { x: 0, z: 0 });
        for local_z in 0..CHUNK_SIZE {
            for local_x in 0..CHUNK_SIZE {
                assert_eq!(chunk.block(local_x, 0, local_z), BEDROCK);
                let surface = chunk.heightmap[(local_x + CHUNK_SIZE * local_z) as usize];
                if surface >= config().sea_level {
                    assert_eq!(chunk.block(local_x, surface + 1, local_z), AIR);
                }
            }
        }
    }

    #[test]
    fn below_the_sea_level_is_flooded() {
        let mut cfg = config();
        cfg.base_height = 40;
        cfg.amplitude = 4;
        cfg.sea_level = 62;
        let chunk = generate_chunk(&cfg, &registry(), ChunkPos { x: 0, z: 0 });
        // Every column's surface is well below the sea level, so water must sit above it.
        for local_z in 0..CHUNK_SIZE {
            for local_x in 0..CHUNK_SIZE {
                let surface = chunk.heightmap[(local_x + CHUNK_SIZE * local_z) as usize];
                assert!(surface < cfg.sea_level);
                assert_eq!(chunk.block(local_x, cfg.sea_level, local_z), WATER);
            }
        }
    }

    #[test]
    fn out_of_range_reads_are_air() {
        let chunk = generate_chunk(&config(), &registry(), ChunkPos { x: 0, z: 0 });
        assert_eq!(chunk.block(-1, 5, 0), AIR);
        assert_eq!(chunk.block(0, chunk.height, 0), AIR);
        assert_eq!(chunk.block(0, 0, CHUNK_SIZE), AIR);
        assert!(chunk.index(0, 0, 0).is_some());
        assert!(chunk.index(CHUNK_SIZE, 0, 0).is_none());
    }

    #[test]
    fn the_heightmap_matches_the_surface() {
        let chunk = generate_chunk(&config(), &registry(), ChunkPos { x: 0, z: 0 });
        for local_z in 0..CHUNK_SIZE {
            for local_x in 0..CHUNK_SIZE {
                let surface = chunk.heightmap[(local_x + CHUNK_SIZE * local_z) as usize];
                assert!(!chunk.is_solid(local_x, surface + 1, local_z));
                assert!(
                    chunk.is_solid(local_x, surface, local_z)
                        || chunk.block(local_x, surface, local_z) == WATER
                );
            }
        }
    }
}
