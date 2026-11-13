# The `.singularity` world-gen format

A `.singularity` package overrides Minecraft Bedrock's world generation, fluids and physics. It is
the sibling of `.universe`: same archive, same signature and permission rules, but its manifest
describes a *world* rather than a set of hooks, and the engine that acts on it is the Rust
`singularity-core` crate.

## Package layout

```
manifest.json          required
biomes/*.json          optional, custom biome definitions
structures/*.json      optional, structure placement rules
lib/                   optional, a native library for a custom shaper
```

## Manifest

```json
{
  "id": "sky-islands",
  "name": "Sky Islands",
  "version": "1.0.0",
  "min_client_version": "1.0.0",
  "abi": "arm64-v8a",
  "seed_algorithm": "sha256",
  "custom_dimensions": [
    { "id": "aether", "sky_color": "#88ccff", "fog_color": "#aaddff" }
  ],
  "berry_api_dependencies": ["world"],
  "native_libs": [],
  "biomes": ["cherry_grove"],
  "structures": ["castle"],
  "terrain": {
    "base_height": 80,
    "amplitude": 48,
    "height": 256,
    "octaves": 6,
    "sea_level": 62,
    "warp": 4.0,
    "default_biome": "plains"
  },
  "physics": {
    "gravity": [0.0, -3.0, 0.0],
    "wind": [0.2, 0.0, 0.0],
    "wind_altitude_gain": 0.01,
    "wind_base_altitude": 64.0
  },
  "permissions": ["world"]
}
```

Required: `id`, `version`, `min_client_version`, `seed_algorithm`, `custom_dimensions`,
`berry_api_dependencies`. Optional: everything else.

Validation refuses a pack whose:

- `seed_algorithm` is not `sha256` or `fnv1a` (only deterministic algorithms are accepted, so two
  clients on the same seed generate the same terrain);
- a `custom_dimensions[].id` is not a safe identifier;
- `terrain.height` is outside `16..=2048`, `base_height`/`sea_level` are outside the chunk, or
  `octaves` is outside `1..=16`;
- `physics.gravity`/`wind` contains a non-finite value.

## Determinism

This is the load-bearing rule of the format. Generation is a pure function of `(seed, chunk
coordinates, manifest)`: no clock, no global RNG, no allocation-order dependence. The seed string is
folded through the manifest's `seed_algorithm` into a 64-bit seed, and every client that types the
same seed produces the same world. A pack that breaks this desyncs multiplayer, so the engine only
offers deterministic primitives (`seed_hash`, `noise2`, `noise3`) to a native shaper.

## The pipeline

1. **Climate.** Three noise fields (temperature, humidity, continentalness) sampled per column.
2. **Biome.** The climate picks a biome from the registry; near a boundary the choice is blended
   over a band rather than snapping.
3. **Height.** fBm (optionally domain-warped) shapes the surface; the surface block comes from the
   biome, with sand at a shoreline.
4. **Blocks.** Stone below, dirt above, the biome's surface block on top; water fills from the sea
   level up to the surface when the surface is below it.
5. **Structures.** Each rule places at most one candidate per cell of `spacing` chunks, at a jittered
   position from the seed, accepted when a seeded roll beats its `rarity` and its footprint does not
   overlap an existing placement.
6. **Mesh.** The greedy mesher merges coplanar faces of the same block into the largest rectangle,
   culls faces against solid neighbours, and shades each vertex with ambient occlusion.
7. **Fluids.** A pressure automaton spreads fluid toward its neighbours' level, scaled by viscosity.
8. **Physics.** Gravity, wind (strengthening with altitude) and per-material friction/restitution
   integrate entity motion; collision stays the game's job, so the client does not desync.

## Custom fluids and materials

A custom fluid is described by its viscosity (higher spreads slower), `max_spread` and whether it is
an infinite source. A block's physics material is `friction` (the fraction of horizontal speed lost
on landing; 0 is ice) and `restitution` (the bounce factor; 1 is a trampoline).

## Writing a native shaper

Most packs need no native code — the manifest's terrain and biome fields cover the common cases. A
pack that needs a bespoke shaper includes `pl/singularity/singularity_api.h` and exports the same
three entry points a `.universe` mod does. See `examples/sky_islands/`.

## Packaging

```sh
caramel-cli pack examples/sky_islands sky_islands.singularity
caramel-cli sign sky_islands.singularity "$(cat private.key.hex)"
```
