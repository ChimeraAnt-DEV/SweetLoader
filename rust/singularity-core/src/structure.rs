//! Deterministic structure placement.
//!
//! A structure (a castle, a dungeon, a ruined portal) is placed by a rule rather than by a global
//! RNG, so the same world seed places the same structures at the same coordinates on every device.
//! The rule is: divide the world into cells of `spacing` chunks; each cell may hold at most one
//! structure, at a jittered position derived from the seed and the cell coordinates; the structure
//! is accepted when a seeded roll beats its rarity.
//!
//! The overlap check is a pure AABB test over the placements accepted so far, so a caller can
//! refuse a structure that would clip a neighbour without needing the world.

/// A structure's placement rule.
#[derive(Debug, Clone, PartialEq)]
pub struct StructureRule {
    pub id: String,
    /// Cell size in chunks; one structure candidate per cell.
    pub spacing: i32,
    /// The chance in `[0, 1]` that a candidate cell actually places.
    pub rarity: f32,
    /// The structure's footprint in chunks.
    pub width: i32,
    pub depth: i32,
    /// The minimum distance in chunks from a placement to another placement of any rule.
    pub separation: i32,
}

/// A placed structure.
#[derive(Debug, Clone, PartialEq)]
pub struct Placement {
    pub id: String,
    pub chunk_x: i32,
    pub chunk_z: i32,
    pub width: i32,
    pub depth: i32,
}

impl Placement {
    /// True when this placement's footprint overlaps `other` expanded by `margin`.
    pub fn overlaps(&self, other: &Placement, margin: i32) -> bool {
        let a_left = self.chunk_x - margin;
        let a_right = self.chunk_x + self.width + margin;
        let b_left = other.chunk_x - margin;
        let b_right = other.chunk_x + other.width + margin;
        if a_right <= b_left || b_right <= a_left {
            return false;
        }
        let a_top = self.chunk_z - margin;
        let a_bottom = self.chunk_z + self.depth + margin;
        let b_top = other.chunk_z - margin;
        let b_bottom = other.chunk_z + other.depth + margin;
        !(a_bottom <= b_top || b_bottom <= a_top)
    }
}

/// The most placements a single structure rule may produce in one query, so a hostile rule cannot
/// make the generator loop forever.
pub const MAX_PLACEMENTS: usize = 4096;

/// The placements a rule produces in the chunk rectangle `[min_chunk_x, max_chunk_x] x
/// [min_chunk_z, max_chunk_z]`.
///
/// `existing` holds placements already accepted (from every rule), which this rule avoids.
pub fn place(
    rule: &StructureRule,
    seed: u64,
    min_chunk_x: i32,
    max_chunk_x: i32,
    min_chunk_z: i32,
    max_chunk_z: i32,
    existing: &[Placement],
) -> Vec<Placement> {
    let mut out = Vec::new();
    if rule.spacing <= 0 {
        return out;
    }
    let cell_min_x = min_chunk_x.div_euclid(rule.spacing) - 1;
    let cell_max_x = max_chunk_x.div_euclid(rule.spacing) + 1;
    let cell_min_z = min_chunk_z.div_euclid(rule.spacing) - 1;
    let cell_max_z = max_chunk_z.div_euclid(rule.spacing) + 1;

    for cell_z in cell_min_z..=cell_max_z {
        for cell_x in cell_min_x..=cell_max_x {
            if out.len() >= MAX_PLACEMENTS {
                return out;
            }
            let h = crate::noise::hash3(seed, cell_x as i64, 0, cell_z as i64);
            // Rarity roll: the top 24 bits as a fraction.
            let roll = (h >> 40) as f32 / (1u32 << 24) as f32;
            if roll >= rule.rarity {
                continue;
            }
            // Jitter within the cell from the lower bits.
            let jx = ((h >> 16) & 0xff) as i32 % rule.spacing.max(1);
            let jz = ((h >> 8) & 0xff) as i32 % rule.spacing.max(1);
            let chunk_x = cell_x * rule.spacing + jx;
            let chunk_z = cell_z * rule.spacing + jz;
            let candidate = Placement {
                id: rule.id.clone(),
                chunk_x,
                chunk_z,
                width: rule.width,
                depth: rule.depth,
            };
            // In-range and non-overlapping, or it is dropped.
            if chunk_x < min_chunk_x || chunk_x > max_chunk_x {
                continue;
            }
            if chunk_z < min_chunk_z || chunk_z > max_chunk_z {
                continue;
            }
            if existing
                .iter()
                .chain(out.iter())
                .any(|other| candidate.overlaps(other, rule.separation))
            {
                continue;
            }
            out.push(candidate);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rule(id: &str, spacing: i32, rarity: f32) -> StructureRule {
        StructureRule {
            id: id.to_string(),
            spacing,
            rarity,
            width: 2,
            depth: 2,
            separation: 1,
        }
    }

    #[test]
    fn placement_is_deterministic() {
        let rule = rule("castle", 16, 1.0);
        let a = place(&rule, 42, 0, 64, 0, 64, &[]);
        let b = place(&rule, 42, 0, 64, 0, 64, &[]);
        assert_eq!(a, b);
        assert!(!a.is_empty());
    }

    #[test]
    fn a_rarity_of_zero_places_nothing() {
        let rule = rule("rare", 8, 0.0);
        assert!(place(&rule, 1, 0, 256, 0, 256, &[]).is_empty());
    }

    #[test]
    fn different_seeds_place_differently() {
        let rule = rule("castle", 16, 1.0);
        let a = place(&rule, 1, 0, 128, 0, 128, &[]);
        let b = place(&rule, 2, 0, 128, 0, 128, &[]);
        assert_ne!(a, b);
    }

    #[test]
    fn placements_are_one_per_cell_and_do_not_overlap() {
        let rule = rule("castle", 16, 1.0);
        let placements = place(&rule, 7, 0, 200, 0, 200, &[]);
        for (i, a) in placements.iter().enumerate() {
            for b in placements.iter().skip(i + 1) {
                assert!(!a.overlaps(b, 0), "placements overlap: {a:?} {b:?}");
            }
        }
    }

    #[test]
    fn existing_placements_are_avoided() {
        let rule = rule("castle", 16, 1.0);
        let first = place(&rule, 7, 0, 128, 0, 128, &[]);
        let again = place(&rule, 7, 0, 128, 0, 128, &first);
        // Every candidate that would overlap an existing one is dropped.
        for candidate in &again {
            for existing in &first {
                assert!(
                    !candidate.overlaps(existing, rule.separation),
                    "a placement overlaps an existing one"
                );
            }
        }
    }

    #[test]
    fn a_zero_spacing_rule_places_nothing() {
        let rule = rule("bad", 0, 1.0);
        assert!(place(&rule, 1, 0, 64, 0, 64, &[]).is_empty());
    }

    #[test]
    fn overlap_is_symmetric_and_margin_sensitive() {
        let a = Placement {
            id: "a".into(),
            chunk_x: 0,
            chunk_z: 0,
            width: 2,
            depth: 2,
        };
        let b = Placement {
            id: "b".into(),
            chunk_x: 3,
            chunk_z: 0,
            width: 2,
            depth: 2,
        };
        assert!(!a.overlaps(&b, 0));
        assert!(a.overlaps(&b, 2));
        assert_eq!(a.overlaps(&b, 1), b.overlaps(&a, 1));
    }
}
