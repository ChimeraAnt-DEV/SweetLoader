# The `.universe` mod format

A `.universe` package is the CaramelClient native mod format. It replaces the removed `.AntEgg`
format: same job (ship native code and assets into the game process), but with a checked manifest, a
signature, a permission sandbox and a deterministic load order.

## Package layout

A `.universe` file is a ZIP. The root holds:

```
manifest.json          required, the whole contract (see below)
lib/                   the native libraries the manifest names
assets/                anything the mod reads at runtime
```

`manifest.json` is read straight out of the archive's central directory before anything is
extracted, so a package is refused *before* a single byte of it is written to disk.

## Manifest

```json
{
  "id": "example",
  "name": "Example Mod",
  "version": "1.0.0",
  "author": "you",
  "min_client_version": "1.0.0",
  "abi": "arm64-v8a",
  "native_libs": ["lib/libexample.so"],
  "berry_api_dependencies": ["world"],
  "hooks": [
    { "signature": "f0 4f 02 a9 .. .. 00 94", "module": "libminecraftpe.so", "name": "tick" }
  ],
  "dependencies": [],
  "permissions": ["read_memory", "entities"]
}
```

Required: `id`, `version`, `min_client_version`, `native_libs`, `berry_api_dependencies`, `hooks`.
Optional: `name`, `author`, `abi` (defaults to `arm64-v8a`), `dependencies`, `permissions`.

Every field is validated. The loader refuses a package whose:

- `id` is not a safe identifier (letters, digits, `-`, `_`, length-capped);
- `version` or `min_client_version` is not a `major.minor.patch` version;
- `min_client_version` is newer than the client (a package built for a future client);
- `abi` is not `arm64-v8a` (the only ABI the client ships);
- `native_libs` names a file the archive does not contain, or a path that is not a plain relative
  name (`../`, an absolute path, a backslash);
- `hooks` exceeds the hook limit, or a hook names an unsafe module;
- `permissions` names an unknown capability.

## Signature

A package may carry a detached Ed25519 signature in `<package>.sig`, over the SHA-256 of the
package's bytes. The client verifies it against its trusted-key set. A package whose signature does
not verify is refused, and a `write_memory` permission is only honoured for a trusted-signature
package — an unsigned mod cannot be granted memory writes, whatever it declares.

## Permissions

| Permission     | Grants                                                    | Trusted only |
| -------------- | --------------------------------------------------------- | ------------ |
| `read_memory`  | read process memory through the host's safe reader        | no           |
| `write_memory` | write process memory                                      | **yes**      |
| `render_hook`  | install render hooks                                      | no           |
| `network`      | open a socket                                             | no           |
| `file_system`  | read files under the mod's own sandbox directory          | no           |
| `ui`           | draw into the HUD / register screens                      | no           |
| `entities`     | read the entity feed                                      | no           |
| `world`        | read world / block state                                  | no           |
| `audio`        | play custom audio                                         | no           |

A mod only gets the capabilities it declared, and the loader passes a host table with the
undeclared function pointers left null. A mod must null-check every pointer before calling.

## Load order

`dependencies` names other mod ids this mod must load *after*. The loader runs a topological sort;
a dependency cycle refuses the whole cycle (each mod in it is reported and skipped) rather than
loading a partial order. An absent dependency is not an error: it is a mod the user has not
installed, and the loader loads what it can.

## Lifecycle

1. The manifest is parsed and validated.
2. The signature is verified (when present).
3. The package is extracted into `<sandbox>/<id>/`, staged under a temporary name and renamed into
   place only when every declared native library is present — a partial extraction leaves nothing
   loadable behind.
4. Native libraries are `dlopen`ed with `RTLD_NOW | RTLD_LOCAL`.
5. `universe_init(&host)` is called with the host table.
6. `universe_tick(frame, dt)` is called once per frame.
7. `universe_shutdown()` is called, then the libraries are `dlclose`d.

Every call into mod code is wrapped so a panic or a fault is caught, the mod is unloaded, and the
game thread keeps running. A mod that fails three times in a row is blacklisted for the session.

## Writing the native side

Include `pl/universe/universe_api.h` and export the three entry points. See
`examples/example_universe/` for a complete, buildable mod, and the header for the full host table.

## Packaging

```sh
caramel-cli keygen private.key.hex
caramel-cli pack examples/example_universe example.universe
caramel-cli sign example.universe "$(cat private.key.hex)"
caramel-cli verify example.universe <public-key-hex>
```
