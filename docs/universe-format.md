# The `.universe` mod format

A `.universe` package is the CaramelClient native mod format. It replaces the removed `.AntEgg`
format: same job (ship native code and assets into the game process), but with a checked manifest, a
signature, a permission gate and a deterministic load order.

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

- `id` is not a safe identifier (letters, digits, `.`, `_`, `-`; must not begin with `.`, which is
  reserved for the extractor's staging directories; length-capped);
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

A mod only gets the capabilities it declared. The loader builds a host table with the undeclared
function pointers left null, and a mod must null-check every pointer before calling.

Two honest caveats, because the words "sandbox" and "gate" overstate what this is:

- **This is a gate on a function table, not a sandbox.** A `.universe` mod is a native `.so` in the
  game process; it can call `mprotect` and the syscalls directly, so a permission the loader refuses
  is a permission the mod does not get *through the host table*, not one it cannot reach. An
  unsigned mod still runs arbitrary native code. Treat a permission grant as a statement of intent
  the UI can show, not as containment.
- **The host table does not exist yet.** `universe_host_t` is defined in `pl/universe/universe_api.h`
  and `check_permissions` is implemented and tested, but no code builds the table or hands it to
  `universe_init`, so the permission model is not enforced at runtime today. The launcher labels an
  engine package "Preview only" for this reason; see the client's
  `docs/agents/engine-packages-universe-singularity-import-discovery-honest-availability.md`.

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

Every call into mod code is wrapped in a Rust `catch_unwind` guard, and the entry points are typed
`extern "C-unwind"` so a Rust panic can actually reach that guard (a panic unwinding out of a plain
`extern "C"` function is undefined behaviour — in practice an abort — and the guard would never run).
A caught panic is logged, the mod is unloaded immediately, and one mod's failure never stops
another's tick. A mod that faults three times in a row is disabled for the session.

The guard catches a Rust panic. A C++ exception thrown out of a mod's own C++ and left to escape its
`universe_init`/`tick`/`shutdown` is *not* reliably caught by `catch_unwind` — the C-unwind ABI lets
it cross the boundary, but a foreign exception unwinding through Rust frames is not the panic path
the guard handles. A mod written in C++ must catch its own exceptions at its entry points.

A panic is not a segmentation fault. `catch_unwind` catches a Rust panic, but a hard fault (a null
dereference inside a mod's C/C++ code, a `SIGSEGV`/`SIGABRT`) is delivered as a signal to the whole
process and is not recoverable by `catch_unwind`. The loader's defence against that class is
fail-closed loading — a wrong ABI, a missing entry point or a failed `universe_init` is refused
before the mod ever runs — plus the per-mod fault budget that stops a mod which faults on every
launch. Do not read this section as "a crashing native mod is contained"; it is not, and no
in-process loader can contain it.

A running native call also cannot be interrupted from another thread, so a mod that *blocks* inside
`universe_tick` cannot be stopped mid-call. The loader therefore measures each mod's tick against a
per-frame budget (`universe_core::budget::TICK_BUDGET_US`, 2 ms) and counts an overrun as a fault,
so a mod that overruns on every frame is disabled after the same three consecutive faults as a
panicking one. The budget is an upper bound on ordinary per-frame work, not a hard preemption: the
first overrun still costs that frame. `universe_init` is not budgeted — it runs once, at load, under
the launcher's own bounded enable (see the client's `ModManager::enableLoadedMods`).

The loader's C ABI (`caramel_universe_load_mods`, `_tick_mods`, `_shutdown_mods`, `_live_mods`) is
the surface the C++ runtime drives, with matching wrappers in `pl/engine/UniverseApi.h`. Loading
applies the dependency order, refuses a dependency cycle rather than loading it partially, and
refuses a disabled mod before opening anything.

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
