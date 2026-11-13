//! Deterministic noise for terrain generation.
//!
//! Terrain is generated on a background thread and, in multiplayer, must match between devices, so
//! the noise is a pure function of `(seed, coordinates)`: no global state, no time, no RNG that
//! advances. Two clients running the same `.singularity` pack and the same seed therefore generate
//! the same terrain, which is what makes seed sync possible at all.
//!
//! The implementations are value noise (a hashed lattice with smooth interpolation) and its
//! fractal sum (fBm). Value noise is used rather than Perlin gradient noise because the hashed
//! lattice is branch-free and vectorises cleanly on arm64, and the visual difference is negligible
//! once several octaves are summed.

/// A 64-bit integer hash of a lattice point, the basis of the noise.
///
/// SplitMix64's finaliser: strong avalanche, no state, identical on every platform.
#[inline]
pub fn hash3(seed: u64, x: i64, y: i64, z: i64) -> u64 {
    let mut h = seed;
    h = h.wrapping_add((x as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15));
    h = h.wrapping_add((y as u64).wrapping_mul(0xC2B2_AE3D_27D4_EB4F));
    h = h.wrapping_add((z as u64).wrapping_mul(0x1656_67B1_9E37_79F9));
    h ^= h >> 30;
    h = h.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    h ^= h >> 27;
    h = h.wrapping_mul(0x94D0_49BB_1331_11EB);
    h ^= h >> 31;
    h
}

/// A lattice value in `[0, 1)`.
#[inline]
fn lattice(seed: u64, x: i64, y: i64, z: i64) -> f32 {
    (hash3(seed, x, y, z) >> 40) as f32 / (1u32 << 24) as f32
}

/// Smoothstep interpolation, the cubic the noise uses between lattice points.
#[inline]
fn smooth(t: f32) -> f32 {
    t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
}

#[inline]
fn lerp(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// 3D value noise in `[0, 1]`.
pub fn value3(seed: u64, x: f32, y: f32, z: f32) -> f32 {
    let xi = x.floor();
    let yi = y.floor();
    let zi = z.floor();
    let tx = smooth(x - xi);
    let ty = smooth(y - yi);
    let tz = smooth(z - zi);
    let (x0, y0, z0) = (xi as i64, yi as i64, zi as i64);
    let (x1, y1, z1) = (x0 + 1, y0 + 1, z0 + 1);

    let c000 = lattice(seed, x0, y0, z0);
    let c100 = lattice(seed, x1, y0, z0);
    let c010 = lattice(seed, x0, y1, z0);
    let c110 = lattice(seed, x1, y1, z0);
    let c001 = lattice(seed, x0, y0, z1);
    let c101 = lattice(seed, x1, y0, z1);
    let c011 = lattice(seed, x0, y1, z1);
    let c111 = lattice(seed, x1, y1, z1);

    let x00 = lerp(c000, c100, tx);
    let x10 = lerp(c010, c110, tx);
    let x01 = lerp(c001, c101, tx);
    let x11 = lerp(c011, c111, tx);
    let y0v = lerp(x00, x10, ty);
    let y1v = lerp(x01, x11, ty);
    lerp(y0v, y1v, tz)
}

/// 2D value noise in `[0, 1]` (a slice of the 3D lattice at `z = 0`).
pub fn value2(seed: u64, x: f32, z: f32) -> f32 {
    value3(seed, x, 0.0, z)
}

/// A fractal Brownian motion over value noise, in `[0, 1]`.
///
/// `octaves` is capped at [`MAX_OCTAVES`]: a hostile pack could otherwise ask for a million octaves
/// and stall the generation thread.
pub fn fbm3(seed: u64, x: f32, y: f32, z: f32, octaves: u32, lacunarity: f32, gain: f32) -> f32 {
    let octaves = octaves.clamp(1, MAX_OCTAVES);
    let mut amplitude = 1.0f32;
    let mut frequency = 1.0f32;
    let mut sum = 0.0f32;
    let mut norm = 0.0f32;
    for octave in 0..octaves {
        // Each octave uses a different hash seed, so octaves do not correlate.
        let octave_seed = seed ^ ((octave as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15));
        sum += amplitude * value3(octave_seed, x * frequency, y * frequency, z * frequency);
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    if norm > 0.0 {
        sum / norm
    } else {
        0.0
    }
}

/// A fractal Brownian motion over 2D value noise.
pub fn fbm2(seed: u64, x: f32, z: f32, octaves: u32, lacunarity: f32, gain: f32) -> f32 {
    fbm3(seed, x, 0.0, z, octaves, lacunarity, gain)
}

/// Cellular (Worley) noise: the distance to the nearest hashed lattice point, in `[0, 1]`.
///
/// Used for biome cell placement, where the "nearest site" structure reads better than smooth fBm.
pub fn cellular2(seed: u64, x: f32, z: f32) -> f32 {
    let xi = x.floor();
    let zi = z.floor();
    let (x0, z0) = (xi as i64, zi as i64);
    let mut nearest = f32::MAX;
    for dz in -1..=1i64 {
        for dx in -1..=1i64 {
            let (cx, cz) = (x0 + dx, z0 + dz);
            // Jitter the site within its cell from the hash.
            let h = hash3(seed, cx, 0, cz);
            let jx = ((h >> 40) & 0xff) as f32 / 255.0;
            let jz = ((h >> 32) & 0xff) as f32 / 255.0;
            let px = cx as f32 + jx;
            let pz = cz as f32 + jz;
            let d = ((px - x) * (px - x) + (pz - z) * (pz - z)).sqrt();
            if d < nearest {
                nearest = d;
            }
        }
    }
    nearest.clamp(0.0, 1.0)
}

/// The most octaves any fBm call may use.
pub const MAX_OCTAVES: u32 = 16;

/// Domain warping: offsets the sample point by another noise field, which is what turns smooth
/// fBm hills into overhangs, ridges and floating islands.
pub fn warped3(seed: u64, x: f32, y: f32, z: f32, strength: f32) -> f32 {
    let wx = value3(seed ^ 0x1111, x * 0.5, y * 0.5, z * 0.5) - 0.5;
    let wy = value3(seed ^ 0x2222, x * 0.5, y * 0.5, z * 0.5) - 0.5;
    let wz = value3(seed ^ 0x3333, x * 0.5, y * 0.5, z * 0.5) - 0.5;
    fbm3(seed, x + wx * strength, y + wy * strength, z + wz * strength, 4, 2.0, 0.5)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn noise_is_deterministic() {
        for _ in 0..100 {
            assert_eq!(value3(42, 1.5, 2.5, 3.5), value3(42, 1.5, 2.5, 3.5));
        }
        assert_eq!(fbm2(7, 10.0, 20.0, 4, 2.0, 0.5), fbm2(7, 10.0, 20.0, 4, 2.0, 0.5));
    }

    #[test]
    fn noise_stays_in_range() {
        for i in 0..2000 {
            let x = i as f32 * 0.37;
            let z = i as f32 * 0.11;
            let v = fbm2(1, x, z, 6, 2.0, 0.5);
            assert!((0.0..=1.0).contains(&v), "fbm out of range: {v}");
            let c = cellular2(1, x, z);
            assert!((0.0..=1.0).contains(&c), "cellular out of range: {c}");
        }
    }

    #[test]
    fn different_seeds_differ() {
        let a = value3(1, 5.0, 5.0, 5.0);
        let b = value3(2, 5.0, 5.0, 5.0);
        assert_ne!(a, b);
    }

    #[test]
    fn octaves_are_capped() {
        // A hostile octave count must not stall: the result is the capped one.
        let capped = fbm2(3, 1.0, 1.0, MAX_OCTAVES, 2.0, 0.5);
        let huge = fbm2(3, 1.0, 1.0, u32::MAX, 2.0, 0.5);
        assert_eq!(capped, huge);
    }

    #[test]
    fn interpolation_is_continuous_across_lattice_cells() {
        // Two samples either side of a lattice boundary must be close.
        let a = value3(9, 3.9999, 0.0, 0.0);
        let b = value3(9, 4.0001, 0.0, 0.0);
        assert!((a - b).abs() < 0.01, "noise jumps at a lattice boundary: {a} vs {b}");
    }

    #[test]
    fn warped_noise_stays_in_range() {
        for i in 0..500 {
            let v = warped3(5, i as f32, 0.0, 0.0, 8.0);
            assert!((0.0..=1.0).contains(&v), "warped out of range: {v}");
        }
    }
}
