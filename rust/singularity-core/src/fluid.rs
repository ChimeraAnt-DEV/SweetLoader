//! Fluid dynamics: a cellular automaton over the fluid blocks in a chunk.
//!
//! Bedrock's fluids spread by a fixed distance from a source. This engine spreads by *pressure*: a
//! cell's level is set by its neighbours' levels, so a pool finds its own level and a source refills
//! what drains. Viscosity scales the spread, so honey crawls where water rushes.
//!
//! The simulation is a pure step function over a flat cell array, which makes it testable and lets
//! the caller run it on a background thread and swap the result in.

/// The fluid ids the engine understands. `None` means no fluid.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FluidKind {
    Water,
    Lava,
    /// A custom fluid whose behaviour comes from its [`FluidProperties`].
    Custom(u32),
}

/// A fluid's physical properties.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FluidProperties {
    /// Higher spreads slower. Water is 1; honey is high.
    pub viscosity: f32,
    /// The blocks a fluid may spread from a source before it stops (vanilla water is 7).
    pub max_spread: i32,
    /// True when a cell of this fluid is a permanent source (an ocean, a spring).
    pub infinite_source: bool,
}

impl FluidProperties {
    pub fn water() -> FluidProperties {
        FluidProperties {
            viscosity: 1.0,
            max_spread: 7,
            infinite_source: false,
        }
    }

    pub fn lava() -> FluidProperties {
        FluidProperties {
            viscosity: 4.0,
            max_spread: 3,
            infinite_source: false,
        }
    }
}

/// A grid of fluid cells: the fluid id and its level (0 empty, higher is fuller).
#[derive(Debug, Clone, PartialEq)]
pub struct FluidGrid {
    pub width: i32,
    pub height: i32,
    pub cells: Vec<Option<(FluidKind, i32)>>,
}

impl FluidGrid {
    pub fn new(width: i32, height: i32) -> FluidGrid {
        FluidGrid {
            width,
            height,
            cells: vec![None; (width * height) as usize],
        }
    }

    fn index(&self, x: i32, y: i32) -> Option<usize> {
        if x < 0 || x >= self.width || y < 0 || y >= self.height {
            return None;
        }
        Some((x + self.width * y) as usize)
    }

    pub fn get(&self, x: i32, y: i32) -> Option<(FluidKind, i32)> {
        self.index(x, y).and_then(|i| self.cells[i])
    }

    pub fn set(&mut self, x: i32, y: i32, value: Option<(FluidKind, i32)>) {
        if let Some(index) = self.index(x, y) {
            self.cells[index] = value;
        }
    }

    /// The highest level a cell may hold.
    pub const MAX_LEVEL: i32 = 8;
}

/// Advances the fluid one step.
///
/// Each non-source cell moves toward the average level of its same-fluid neighbours, scaled down by
/// viscosity; a cell with no same-fluid neighbour loses one level (it drains, because nothing feeds
/// it). An empty cell is filled from above when gravity pulls fluid down, or laterally from a
/// neighbour holding more than one level. The step reads the previous grid and writes a new one, so
/// the update order does not matter (no in-place scan artifacts).
pub fn step(
    grid: &FluidGrid,
    properties: &dyn Fn(FluidKind) -> FluidProperties,
    gravity: f32,
) -> FluidGrid {
    let mut next = grid.clone();
    for y in 0..grid.height {
        for x in 0..grid.width {
            let Some((kind, level)) = grid.get(x, y) else {
                // An empty cell may be filled from above (a falling stream) or sideways.
                let mut inflow = 0i32;
                if gravity > 0.0 {
                    if let Some((above_kind, above_level)) = grid.get(x, y - 1) {
                        let props = properties(above_kind);
                        let flow =
                            ((above_level as f32) * gravity / props.viscosity.max(0.01)).round() as i32;
                        if flow >= 1 {
                            inflow = inflow.max(flow);
                        }
                    }
                }
                for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
                    if let Some((neighbour_kind, neighbour_level)) = grid.get(x + dx, y + dy) {
                        if neighbour_level > 1 {
                            let props = properties(neighbour_kind);
                            let flow = (neighbour_level as f32 / props.viscosity.max(0.01)).round() as i32;
                            if flow >= 1 {
                                inflow = inflow.max(flow);
                            }
                        }
                    }
                }
                if inflow > 0 {
                    // The incoming fluid keeps its kind; take it from the strongest contributor.
                    let kind = strongest_neighbour_kind(grid, properties, x, y).unwrap_or(FluidKind::Water);
                    next.set(x, y, Some((kind, inflow.min(FluidGrid::MAX_LEVEL))));
                }
                continue;
            };
            let props = properties(kind);
            if props.infinite_source {
                next.set(x, y, Some((kind, FluidGrid::MAX_LEVEL)));
                continue;
            }
            // Look at the four neighbours of the same fluid.
            let mut pressure = 0;
            let mut count = 0;
            for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
                if let Some((neighbour_kind, neighbour_level)) = grid.get(x + dx, y + dy) {
                    if neighbour_kind == kind {
                        pressure += neighbour_level;
                        count += 1;
                    }
                }
            }
            let best = if count > 0 {
                let average = pressure / count;
                // Move toward the average, scaled down by viscosity.
                let delta = ((average - level) as f32 / props.viscosity.max(0.01)).round() as i32;
                (level + delta).clamp(0, FluidGrid::MAX_LEVEL)
            } else {
                // Nothing feeds this cell: it loses a level and eventually empties.
                level - 1
            };
            if best < 1 {
                next.set(x, y, None);
            } else {
                next.set(x, y, Some((kind, best)));
            }
        }
    }
    next
}

/// The kind of the neighbour contributing the most fluid to `(x, y)`, used when filling an empty
/// cell so the incoming fluid keeps a kind rather than defaulting blindly.
fn strongest_neighbour_kind(
    grid: &FluidGrid,
    properties: &dyn Fn(FluidKind) -> FluidProperties,
    x: i32,
    y: i32,
) -> Option<FluidKind> {
    let mut best: Option<(FluidKind, i32)> = None;
    for (dx, dy) in [(0, -1), (1, 0), (-1, 0), (0, 1)] {
        if let Some((kind, level)) = grid.get(x + dx, y + dy) {
            let effective = (level as f32 / properties(kind).viscosity.max(0.01)).round() as i32;
            if effective >= 1 {
                match best {
                    Some((_, best_level)) if best_level >= effective => {}
                    _ => best = Some((kind, effective)),
                }
            }
        }
    }
    best.map(|(kind, _)| kind)
}

/// True when a source of `kind` sits within `max_spread` blocks of `(x, y)`.
pub fn within_source_range(
    grid: &FluidGrid,
    properties: &dyn Fn(FluidKind) -> FluidProperties,
    kind: FluidKind,
    x: i32,
    y: i32,
) -> bool {
    let props = properties(kind);
    for dy in -props.max_spread..=props.max_spread {
        for dx in -props.max_spread..=props.max_spread {
            if dx.abs() + dy.abs() > props.max_spread {
                continue;
            }
            if let Some((neighbour_kind, _)) = grid.get(x + dx, y + dy) {
                if neighbour_kind == kind {
                    let neighbour_props = properties(neighbour_kind);
                    if neighbour_props.infinite_source {
                        return true;
                    }
                }
            }
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn props(kind: FluidKind) -> FluidProperties {
        match kind {
            FluidKind::Water => FluidProperties::water(),
            FluidKind::Lava => FluidProperties::lava(),
            FluidKind::Custom(_) => FluidProperties {
                viscosity: 6.0,
                max_spread: 4,
                infinite_source: false,
            },
        }
    }

    #[test]
    fn a_source_stays_full() {
        let mut grid = FluidGrid::new(8, 8);
        grid.set(0, 0, Some((FluidKind::Water, 3)));
        let mut source = props(FluidKind::Water);
        source.infinite_source = true;
        let stepped = step(&grid, &|_| source, 1.0);
        assert_eq!(stepped.get(0, 0), Some((FluidKind::Water, FluidGrid::MAX_LEVEL)));
    }

    #[test]
    fn water_spreads_to_a_neighbour() {
        let mut grid = FluidGrid::new(8, 8);
        grid.set(0, 0, Some((FluidKind::Water, 8)));
        let stepped = step(&grid, &props, 1.0);
        // The neighbour at (1, 0) should have gained some water.
        assert!(stepped.get(1, 0).is_some());
    }

    #[test]
    fn a_lone_cell_with_no_neighbours_and_no_source_drains() {
        let mut grid = FluidGrid::new(4, 4);
        grid.set(1, 1, Some((FluidKind::Water, 1)));
        let stepped = step(&grid, &props, 0.0);
        // Level 1 with no neighbours and no gravity has nothing to hold it up.
        assert_eq!(stepped.get(1, 1), None);
    }

    #[test]
    fn gravity_fills_the_cell_below() {
        let mut grid = FluidGrid::new(4, 8);
        grid.set(1, 0, Some((FluidKind::Water, 8)));
        let stepped = step(&grid, &props, 1.0);
        assert!(stepped.get(1, 1).is_some(), "water should fall into the cell below");
    }

    #[test]
    fn viscosity_slows_the_spread() {
        // The same starting level spreads less with a high-viscosity fluid.
        let mut thin = FluidGrid::new(8, 8);
        thin.set(0, 0, Some((FluidKind::Water, 8)));
        let mut thick = FluidGrid::new(8, 8);
        thick.set(0, 0, Some((FluidKind::Custom(1), 8)));
        let thin_step = step(&thin, &props, 1.0);
        let thick_step = step(&thick, &props, 1.0);
        let thin_level = thin_step.get(1, 0).map(|(_, l)| l).unwrap_or(0);
        let thick_level = thick_step.get(1, 0).map(|(_, l)| l).unwrap_or(0);
        assert!(thin_level >= thick_level, "viscosity should slow spreading");
    }

    #[test]
    fn source_range_is_respected() {
        // `within_source_range` asks the properties closure whether a same-fluid neighbour is a
        // permanent source, so the closure here marks water as one.
        let source_props = |kind: FluidKind| {
            let mut props = props(kind);
            props.infinite_source = true;
            props
        };
        let mut grid = FluidGrid::new(32, 32);
        grid.set(0, 0, Some((FluidKind::Water, 8)));
        assert!(within_source_range(&grid, &source_props, FluidKind::Water, 3, 0));
        assert!(!within_source_range(&grid, &source_props, FluidKind::Water, 20, 0));
    }
}
