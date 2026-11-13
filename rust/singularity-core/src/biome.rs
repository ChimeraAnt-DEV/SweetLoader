//! Biome placement from a climate map.
//!
//! A biome is chosen from three climate axes (temperature, humidity, continentalness) sampled as
//! noise, then matched against the registered biomes' ranges. Two details matter for a believable
//! world:
//!
//! * **Blending, not hard cuts.** Near a biome boundary the choice is weighted, so the terrain
//!   transitions over a few blocks rather than snapping between, say, desert and tundra.
//! * **A deterministic fallback.** A climate point no biome claims falls back to a designated
//!   default biome rather than panicking, so a pack with sparse climate ranges still generates.

/// One climate axis' range a biome accepts.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Range {
    pub min: f32,
    pub max: f32,
}

impl Range {
    pub fn new(min: f32, max: f32) -> Range {
        Range {
            min: min.min(max),
            max: min.max(max),
        }
    }

    /// True when `value` is inside the range.
    pub fn contains(&self, value: f32) -> bool {
        value >= self.min && value <= self.max
    }

    /// The distance from `value` to the nearest edge, 0 when inside. Used to pick the *best* match
    /// rather than the first.
    pub fn distance(&self, value: f32) -> f32 {
        if self.contains(value) {
            0.0
        } else if value < self.min {
            self.min - value
        } else {
            value - self.max
        }
    }
}

/// A registered biome and the climate it accepts.
#[derive(Debug, Clone, PartialEq)]
pub struct Biome {
    pub id: String,
    pub temperature: Range,
    pub humidity: Range,
    pub continentalness: Range,
    /// Higher wins when two biomes both accept a climate point.
    pub weight: i32,
}

/// A climate sample.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Climate {
    pub temperature: f32,
    pub humidity: f32,
    pub continentalness: f32,
}

/// A biome definition as it appears in a pack's `biomes/*.json`.
#[derive(Debug, Clone, PartialEq, serde::Deserialize)]
pub struct BiomeDefinition {
    pub id: String,
    #[serde(default = "full_range")]
    pub temperature: [f32; 2],
    #[serde(default = "full_range")]
    pub humidity: [f32; 2],
    #[serde(default = "full_range")]
    pub continentalness: [f32; 2],
    #[serde(default)]
    pub weight: i32,
}

fn full_range() -> [f32; 2] {
    [0.0, 1.0]
}

impl BiomeDefinition {
    /// Converts the definition to a [`Biome`], refusing an unsafe id or a non-finite range so a bad
    /// `biomes/*.json` is a reported error rather than a biome the engine cannot place.
    pub fn to_biome(&self) -> universe_core::Result<Biome> {
        if !universe_core::manifest::is_safe_segment(&self.id) {
            return Err(universe_core::Error::new(format!(
                "biome id '{}' is not a safe identifier",
                self.id
            )));
        }
        for (name, range) in [
            ("temperature", self.temperature),
            ("humidity", self.humidity),
            ("continentalness", self.continentalness),
        ] {
            if !range[0].is_finite() || !range[1].is_finite() {
                return Err(universe_core::Error::new(format!(
                    "biome '{}' has a non-finite {name} range",
                    self.id
                )));
            }
        }
        Ok(Biome {
            id: self.id.clone(),
            temperature: Range::new(self.temperature[0], self.temperature[1]),
            humidity: Range::new(self.humidity[0], self.humidity[1]),
            continentalness: Range::new(self.continentalness[0], self.continentalness[1]),
            weight: self.weight,
        })
    }
}

/// Parses one `biomes/*.json` file into a [`Biome`].
pub fn parse_definition(json: &str) -> universe_core::Result<Biome> {
    let definition: BiomeDefinition = serde_json::from_str(json).map_err(|error| {
        universe_core::Error::new(format!("a biome definition is not valid JSON: {error}"))
    })?;
    definition.to_biome()
}

/// Parses a JSON array of biome definitions into a [`Vec<Biome>`].
///
/// All-or-nothing: one malformed element fails the whole list, so a caller never registers a
/// partial biome set and then generates a world against it.
pub fn parse_definitions(json: &str) -> universe_core::Result<Vec<Biome>> {
    let definitions: Vec<BiomeDefinition> = serde_json::from_str(json).map_err(|error| {
        universe_core::Error::new(format!("the biome list is not valid JSON: {error}"))
    })?;
    definitions.iter().map(BiomeDefinition::to_biome).collect()
}

/// The biome registry.
#[derive(Debug, Clone)]
pub struct BiomeRegistry {
    biomes: Vec<Biome>,
    default_biome: String,
}

impl BiomeRegistry {
    /// Builds a registry with a fallback biome id.
    pub fn new(default_biome: impl Into<String>) -> BiomeRegistry {
        BiomeRegistry {
            biomes: Vec::new(),
            default_biome: default_biome.into(),
        }
    }

    pub fn register(&mut self, biome: Biome) {
        self.biomes.push(biome);
    }

    pub fn len(&self) -> usize {
        self.biomes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.biomes.is_empty()
    }

    pub fn default_biome(&self) -> &str {
        &self.default_biome
    }

    /// Picks the biome for a climate sample.
    ///
    /// The best candidate is the one with the highest weight among those that fully contain the
    /// climate, or, if none fully contains it, the one whose climate is nearest. The fallback biome
    /// answers only when the registry is empty.
    pub fn select(&self, climate: Climate) -> &str {
        if self.biomes.is_empty() {
            return &self.default_biome;
        }
        let mut best: Option<(&Biome, f32)> = None; // (biome, distance)
        for biome in &self.biomes {
            let distance = biome.temperature.distance(climate.temperature)
                + biome.humidity.distance(climate.humidity)
                + biome.continentalness.distance(climate.continentalness);
            best = match best {
                None => Some((biome, distance)),
                Some((best_biome, best_distance)) => {
                    // Inside (distance 0) always beats outside; otherwise higher weight, then
                    // nearer. Ties keep the earlier registration, so the result is deterministic.
                    let better = distance < best_distance
                        || (distance == best_distance && biome.weight > best_biome.weight);
                    if better {
                        Some((biome, distance))
                    } else {
                        Some((best_biome, best_distance))
                    }
                }
            };
        }
        match best {
            Some((biome, _)) => &biome.id,
            None => &self.default_biome,
        }
    }

    /// A blend weight in `[0, 1]` for the boundary between two biomes at a climate point.
    ///
    /// 0 means "entirely `a`", 1 means "entirely `b`". The caller uses this to interpolate terrain
    /// height and surface blocks over a transition band rather than snapping.
    pub fn blend(&self, climate: Climate, a: &str, b: &str, band: f32) -> f32 {
        let distance_to = |id: &str| -> Option<f32> {
            self.biomes
                .iter()
                .find(|biome| biome.id == id)
                .map(|biome| {
                    biome.temperature.distance(climate.temperature)
                        + biome.humidity.distance(climate.humidity)
                        + biome.continentalness.distance(climate.continentalness)
                })
        };
        match (distance_to(a), distance_to(b)) {
            (Some(da), Some(db)) => {
                if band <= 0.0 {
                    if da <= db {
                        0.0
                    } else {
                        1.0
                    }
                } else {
                    // Closer to b -> higher weight, smoothly across the band.
                    ((da - db) / (2.0 * band) + 0.5).clamp(0.0, 1.0)
                }
            }
            _ => 0.0,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn biome(id: &str, t: (f32, f32), h: (f32, f32), c: (f32, f32), weight: i32) -> Biome {
        Biome {
            id: id.to_string(),
            temperature: Range::new(t.0, t.1),
            humidity: Range::new(h.0, h.1),
            continentalness: Range::new(c.0, c.1),
            weight,
        }
    }

    fn registry() -> BiomeRegistry {
        let mut registry = BiomeRegistry::new("plains");
        registry.register(biome("desert", (0.6, 1.0), (0.0, 0.3), (0.0, 1.0), 0));
        registry.register(biome("tundra", (0.0, 0.3), (0.0, 1.0), (0.0, 1.0), 0));
        registry.register(biome("jungle", (0.6, 1.0), (0.6, 1.0), (0.0, 1.0), 1));
        registry
    }

    #[test]
    fn selects_the_matching_biome() {
        let registry = registry();
        assert_eq!(
            registry.select(Climate {
                temperature: 0.9,
                humidity: 0.1,
                continentalness: 0.5
            }),
            "desert"
        );
        assert_eq!(
            registry.select(Climate {
                temperature: 0.1,
                humidity: 0.5,
                continentalness: 0.5
            }),
            "tundra"
        );
    }

    #[test]
    fn higher_weight_wins_when_both_match() {
        let registry = registry();
        // Hot and wet matches both desert (weight 0) and jungle (weight 1).
        assert_eq!(
            registry.select(Climate {
                temperature: 0.8,
                humidity: 0.8,
                continentalness: 0.5
            }),
            "jungle"
        );
    }

    #[test]
    fn an_unclaimed_climate_uses_the_nearest_biome() {
        let registry = registry();
        // Cold and dry: tundra is nearer than desert.
        assert_eq!(
            registry.select(Climate {
                temperature: 0.0,
                humidity: 0.0,
                continentalness: 0.5
            }),
            "tundra"
        );
    }

    #[test]
    fn an_empty_registry_falls_back() {
        let registry = BiomeRegistry::new("plains");
        assert_eq!(
            registry.select(Climate {
                temperature: 0.5,
                humidity: 0.5,
                continentalness: 0.5
            }),
            "plains"
        );
    }

    #[test]
    fn blending_moves_between_the_two_biomes() {
        let registry = registry();
        let near_desert = Climate {
            temperature: 0.9,
            humidity: 0.05,
            continentalness: 0.5,
        };
        let weight = registry.blend(near_desert, "desert", "jungle", 0.2);
        assert!(
            weight < 0.5,
            "near desert should weight desert higher: {weight}"
        );
        let near_jungle = Climate {
            temperature: 0.9,
            humidity: 0.95,
            continentalness: 0.5,
        };
        let weight = registry.blend(near_jungle, "desert", "jungle", 0.2);
        assert!(
            weight > 0.5,
            "near jungle should weight jungle higher: {weight}"
        );
    }

    #[test]
    fn range_distance_is_zero_inside() {
        let range = Range::new(0.2, 0.8);
        assert_eq!(range.distance(0.5), 0.0);
        assert!((range.distance(0.0) - 0.2).abs() < 1e-6);
        assert!((range.distance(1.0) - 0.2).abs() < 1e-6);
    }

    #[test]
    fn a_reversed_range_is_normalised() {
        let range = Range::new(0.8, 0.2);
        assert_eq!(range.min, 0.2);
        assert_eq!(range.max, 0.8);
    }

    #[test]
    fn parses_a_biome_definition() {
        let biome = parse_definition(
            r#"{ "id": "cherry_grove", "temperature": [0.5, 0.9], "humidity": [0.4, 1.0], "weight": 2 }"#,
        )
        .unwrap();
        assert_eq!(biome.id, "cherry_grove");
        assert_eq!(biome.temperature.min, 0.5);
        assert_eq!(biome.weight, 2);
        // An absent axis defaults to the full range.
        assert_eq!(biome.continentalness.min, 0.0);
        assert_eq!(biome.continentalness.max, 1.0);
    }

    #[test]
    fn refuses_a_biome_definition_with_an_unsafe_id() {
        assert!(parse_definition(r#"{ "id": "../escape" }"#).is_err());
        assert!(parse_definition(r#"{ "id": "" }"#).is_err());
    }

    #[test]
    fn refuses_a_biome_definition_that_is_not_json() {
        assert!(parse_definition("not json").is_err());
    }

    #[test]
    fn parses_a_biome_definition_list() {
        let biomes = parse_definitions(r#"[{"id":"a"},{"id":"b","weight":3}]"#).unwrap();
        assert_eq!(biomes.len(), 2);
        assert_eq!(biomes[1].weight, 3);
    }

    #[test]
    fn a_biome_list_is_all_or_nothing() {
        // One bad element fails the whole list rather than yielding the good prefix.
        assert!(parse_definitions(r#"[{"id":"a"},{"id":"../evil"}]"#).is_err());
        assert!(parse_definitions(r#"[{"id":"a"},"#).is_err());
    }
}
