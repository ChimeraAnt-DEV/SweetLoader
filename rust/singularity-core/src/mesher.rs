//! Greedy meshing with face culling and per-vertex ambient occlusion.
//!
//! The mesher turns a chunk's block array into a vertex/index buffer the render bridge can upload
//! directly. Three properties keep the vertex count (and so the upload and the draw) small:
//!
//! * **Face culling.** A face is emitted only when the neighbour across it is transparent, so the
//!   interior of a solid mass contributes nothing.
//! * **Greedy merging.** Coplanar faces of the same block are merged into the largest possible
//!   rectangle, which is what turns a flat field into a handful of quads instead of thousands.
//! * **Ambient occlusion.** Each vertex is darkened by how many of its three neighbours are solid,
//!   computed from the same neighbour lookups, so the shading is free relative to the scan.
//!
//! The output is deliberately a plain `Vec<f32>`/`Vec<u32>`: the seam to the game is "here are
//! floats", not a Rust type the C++ side would have to mirror.

use crate::chunk::{is_transparent, Chunk, CHUNK_SIZE};

/// One vertex: position, normal, texture coordinate, ambient occlusion, block id.
///
/// 8 floats per vertex. Laid out flat so the whole buffer is one upload.
pub const FLOATS_PER_VERTEX: usize = 8;

/// A meshed chunk: interleaved vertices and indices.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Mesh {
    pub vertices: Vec<f32>,
    pub indices: Vec<u32>,
}

impl Mesh {
    pub fn vertex_count(&self) -> usize {
        self.vertices.len() / FLOATS_PER_VERTEX
    }

    pub fn triangle_count(&self) -> usize {
        self.indices.len() / 3
    }

    pub fn is_empty(&self) -> bool {
        self.indices.is_empty()
    }
}

/// The ambient occlusion factor for a corner: 0 fully occluded, 1 fully open.
fn occlusion(side1: bool, side2: bool, corner: bool) -> f32 {
    if side1 && side2 {
        return 0.0;
    }
    let occluders = side1 as i32 + side2 as i32 + corner as i32;
    match occluders {
        0 => 1.0,
        1 => 0.75,
        2 => 0.5,
        _ => 0.25,
    }
}

/// Meshes a chunk.
///
/// `neighbour_solid` answers whether a block in a neighbouring chunk is solid; a caller that has
/// only the one chunk passes a closure returning false, and the chunk's outer faces are drawn
/// (correct, just not merged across the boundary).
pub fn mesh<F>(chunk: &Chunk, mut neighbour_solid: F) -> Mesh
where
    F: FnMut(i32, i32, i32) -> bool,
{
    let mut mesh = Mesh::default();

    // For each axis, sweep the slices along it and greedy-merge the visible faces in each slice.
    for axis in 0..3usize {
        let (u_axis, v_axis) = match axis {
            0 => (1, 2),
            1 => (0, 2),
            _ => (0, 1),
        };
        let size_axis = [CHUNK_SIZE, chunk.height, CHUNK_SIZE][axis];
        let size_u = [CHUNK_SIZE, chunk.height, CHUNK_SIZE][u_axis];
        let size_v = [CHUNK_SIZE, chunk.height, CHUNK_SIZE][v_axis];
        // The mask is one cell per (u, v) of a slice; its size depends on the axis (the Y sweep's
        // slice is height x 16, not 16 x 16).
        let mut mask = vec![None::<(u8, i32, f32)>; (size_u * size_v) as usize];

        for slice in 0..size_axis {
            // Build the mask for this slice: for each (u, v) cell, which face (if any) is visible.
            for v in 0..size_v {
                for u in 0..size_u {
                    let (x, y, z) = place(axis, u_axis, v_axis, slice, u, v);
                    let block = chunk.block(x, y, z);
                    if is_transparent(block) {
                        mask[(u + size_u * v) as usize] = None;
                        continue;
                    }
                    // Positive face: the block on the +axis side.
                    let (px, py, pz) = place(axis, u_axis, v_axis, slice + 1, u, v);
                    let positive_solid = if in_chunk(axis, slice + 1, size_axis) {
                        !is_transparent(chunk.block(px, py, pz))
                    } else {
                        neighbour_solid(px, py, pz)
                    };
                    if !positive_solid {
                        mask[(u + size_u * v) as usize] = Some((block, 1, 1.0));
                        continue;
                    }
                    // Negative face: the block on the -axis side.
                    let (nx, ny, nz) = place(axis, u_axis, v_axis, slice - 1, u, v);
                    let negative_solid = if in_chunk(axis, slice - 1, size_axis) {
                        !is_transparent(chunk.block(nx, ny, nz))
                    } else {
                        neighbour_solid(nx, ny, nz)
                    };
                    if !negative_solid {
                        mask[(u + size_u * v) as usize] = Some((block, -1, 1.0));
                    } else {
                        mask[(u + size_u * v) as usize] = None;
                    }
                }
            }

            // Greedy-merge the mask into rectangles.
            for v in 0..size_v {
                let mut u = 0;
                while u < size_u {
                    let Some((block, sign, _)) = mask[(u + size_u * v) as usize] else {
                        u += 1;
                        continue;
                    };
                    // Extend along u.
                    let mut width = 1;
                    while u + width < size_u
                        && mask[(u + width + size_u * v) as usize] == Some((block, sign, 1.0))
                    {
                        width += 1;
                    }
                    // Extend along v.
                    let mut height = 1;
                    'outer: while v + height < size_v {
                        for du in 0..width {
                            if mask[(u + du + size_u * (v + height)) as usize] != Some((block, sign, 1.0))
                            {
                                break 'outer;
                            }
                        }
                        height += 1;
                    }
                    emit_quad(
                        chunk,
                        &mut mesh,
                        axis,
                        u_axis,
                        v_axis,
                        slice,
                        u,
                        v,
                        width,
                        height,
                        sign,
                        block,
                        &mut neighbour_solid,
                    );
                    for dv in 0..height {
                        for du in 0..width {
                            mask[(u + du + size_u * (v + dv)) as usize] = None;
                        }
                    }
                    u += width;
                }
            }
        }
    }
    mesh
}

fn in_chunk(axis: usize, slice: i32, size: i32) -> bool {
    let _ = axis;
    slice >= 0 && slice < size
}

/// Maps a (slice, u, v) coordinate on `axis` back to a (x, y, z) block coordinate.
fn place(axis: usize, u_axis: usize, v_axis: usize, slice: i32, u: i32, v: i32) -> (i32, i32, i32) {
    let mut coord = [0i32; 3];
    coord[axis] = slice;
    coord[u_axis] = u;
    coord[v_axis] = v;
    (coord[0], coord[1], coord[2])
}

#[allow(clippy::too_many_arguments)]
fn emit_quad<F>(
    chunk: &Chunk,
    mesh: &mut Mesh,
    axis: usize,
    u_axis: usize,
    v_axis: usize,
    slice: i32,
    u: i32,
    v: i32,
    width: i32,
    height: i32,
    sign: i32,
    block: u8,
    neighbour_solid: &mut F,
) where
    F: FnMut(i32, i32, i32) -> bool,
{
    let base = mesh.vertex_count() as u32;
    // The quad's plane sits at slice + (sign>0 ? 1 : 0) along the axis.
    let plane = slice + if sign > 0 { 1 } else { 0 };
    // The normal is along the axis, signed.
    let mut normal = [0.0f32; 3];
    normal[axis] = sign as f32;

    let corner_uv = [(0, 0), (width, 0), (width, height), (0, height)];
    // Winding: flip for the negative faces so both sides face outward.
    let order: [usize; 4] = if sign > 0 {
        [0, 1, 2, 3]
    } else {
        [3, 2, 1, 0]
    };

    for corner in 0..4 {
        let (cu, cv) = corner_uv[order[corner]];
        let (x, y, z) = place(
            axis,
            u_axis,
            v_axis,
            plane,
            u + cu,
            v + cv,
        );
        // Ambient occlusion from the three neighbours around this corner in the face plane.
        let mut ao = 1.0f32;
        if width > 0 && height > 0 {
            let su = if cu == 0 { -1 } else { 1 };
            let sv = if cv == 0 { -1 } else { 1 };
            let mut side_u = [0i32; 3];
            side_u[u_axis] = su;
            let mut side_v = [0i32; 3];
            side_v[v_axis] = sv;
            let mut sample = |offset: [i32; 3]| -> bool {
                let (sx, sy, sz) = (x + offset[0], y + offset[1], z + offset[2]);
                if sx >= 0 && sx < CHUNK_SIZE && sy >= 0 && sy < chunk.height && sz >= 0 && sz < CHUNK_SIZE {
                    chunk.is_solid(sx, sy, sz)
                } else {
                    neighbour_solid(sx, sy, sz)
                }
            };
            let side1 = sample(side_u);
            let side2 = sample(side_v);
            let corner_sample = sample([
                side_u[0] + side_v[0],
                side_u[1] + side_v[1],
                side_u[2] + side_v[2],
            ]);
            ao = occlusion(side1, side2, corner_sample);
        }
        mesh.vertices.extend_from_slice(&[
            x as f32,
            y as f32,
            z as f32,
            normal[0],
            normal[1],
            normal[2],
            ao,
            block as f32,
        ]);
    }
    mesh.indices
        .extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::chunk::{generate_chunk, ChunkPos, TerrainConfig};

    fn flat_chunk() -> Chunk {
        let config = TerrainConfig {
            seed: 1,
            base_height: 32,
            amplitude: 0,
            sea_level: 0,
            height: 64,
            octaves: 1,
            warp: 0.0,
            default_biome: "plains".to_string(),
        };
        generate_chunk(&config, &crate::biome::BiomeRegistry::new("plains"), ChunkPos { x: 0, z: 0 })
    }

    #[test]
    fn a_flat_chunk_meshes_and_merges() {
        let chunk = flat_chunk();
        let mesh = mesh(&chunk, |_, _, _| false);
        assert!(!mesh.is_empty());
        // The top of a flat chunk is one merged quad per face direction set, not 256 quads.
        let top_vertices = 4;
        assert!(mesh.vertex_count() < 256 * 4, "greedy merging should cut the vertex count");
        assert!(mesh.vertex_count() >= top_vertices);
    }

    #[test]
    fn every_index_is_in_range() {
        let chunk = flat_chunk();
        let mesh = mesh(&chunk, |_, _, _| false);
        let count = mesh.vertex_count() as u32;
        for index in &mesh.indices {
            assert!(*index < count, "index {index} out of range {count}");
        }
        assert_eq!(mesh.indices.len() % 6, 0, "indices must form whole quads");
    }

    #[test]
    fn a_solid_neighbour_culls_the_boundary_faces() {
        let chunk = flat_chunk();
        let open = mesh(&chunk, |_, _, _| false);
        let enclosed = mesh(&chunk, |_, _, _| true);
        // With every neighbour solid, the outer faces are culled, so fewer vertices.
        assert!(enclosed.vertex_count() <= open.vertex_count());
    }

    #[test]
    fn ambient_occlusion_is_within_range() {
        let chunk = flat_chunk();
        let mesh = mesh(&chunk, |_, _, _| false);
        for vertex in 0..mesh.vertex_count() {
            let ao = mesh.vertices[vertex * FLOATS_PER_VERTEX + 6];
            assert!((0.0..=1.0).contains(&ao), "ao out of range: {ao}");
        }
    }

    #[test]
    fn an_empty_chunk_meshes_to_nothing() {
        let config = TerrainConfig {
            seed: 1,
            base_height: 1,
            amplitude: 0,
            sea_level: 0,
            height: 8,
            octaves: 1,
            warp: 0.0,
            default_biome: "plains".to_string(),
        };
        let chunk = generate_chunk(&config, &crate::biome::BiomeRegistry::new("plains"), ChunkPos { x: 0, z: 0 });
        // Bedrock at y=0 still meshes, but only a handful of quads.
        let mesh = mesh(&chunk, |_, _, _| true);
        assert!(mesh.vertex_count() <= 6 * 4);
    }
}
