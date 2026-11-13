#pragma once

/**
 * @file UniverseApi.h
 * @brief The C ABI `libcaramel_universe.so` exports, and the C++ bridge that resolves it.
 *
 * <p>The `.universe` loader is a Rust cdylib. The JVM loads it with `System.loadLibrary`, and this
 * bridge talks to the *same* already-loaded library through `dlsym(RTLD_DEFAULT, ...)` — it never
 * `dlopen`s a second copy, so there is exactly one loader instance in the process and one place
 * that owns the cancellation flag and the BerryAPI pointer.
 *
 * <p><b>Fail-closed.</b> Every function below reports failure when the library is absent, when a
 * symbol is missing, or when a version this build does not understand is reported. A null message
 * pointer means success; a non-null pointer is a heap string the caller frees with
 * {@link pl::engine::FreeMessage}. Nothing here dereferences a guessed address.
 */

#include <string>

#include "pl/Export.hpp"

namespace pl::engine {

/**
 * @brief Resolves the `.universe` loader's C ABI, once.
 *
 * <p>Idempotent and thread-safe. Returns false when the Rust library is not loaded (the launcher
 * has not called `System.loadLibrary("caramel_universe")` yet) or when a required symbol is
 * missing, in which case every other function here reports failure rather than calling through a
 * null pointer.
 */
PL_EXPORT bool EnsureUniverseLoaded();

/** True when the `.universe` C ABI is resolved and usable. */
PL_EXPORT bool UniverseAvailable();

/** The loader's own client version string, or empty when unavailable. */
PL_EXPORT std::string UniverseClientVersion();

/**
 * @brief Hands the loader the BerryAPI API table.
 *
 * <p>Called once at session start with the pointer the launcher received from BerryAPI. Passing
 * null is allowed and leaves the memory bridge off (fail-closed).
 */
PL_EXPORT bool UniverseInit(void *berryApiPointer);

/** Frees a message returned by any function in this file. Null is a no-op. */
PL_EXPORT void FreeMessage(char *message);

/**
 * @brief Validates a `.universe` archive without extracting it.
 *
 * @param archivePath  Path to the `.universe` file.
 * @param outError     Receives the failure message (empty on success).
 * @return true when the archive's manifest, ABI and min-client-version all check out.
 */
PL_EXPORT bool UniverseValidate(const std::string &archivePath, std::string *outError);

/**
 * @brief Extracts a `.universe` archive into a sandbox, refusing a path that escapes it.
 *
 * @param archivePath  Path to the `.universe` file.
 * @param sandboxRoot  The directory the package is extracted under.
 * @param allowedRoot  The only directory the sandbox may live in (an extra guard).
 */
PL_EXPORT bool UniverseExtract(const std::string &archivePath,
                               const std::string &sandboxRoot,
                               const std::string &allowedRoot,
                               std::string *outError);

/** True when the loader has been asked to stop (unload, safe mode, shutdown). */
PL_EXPORT bool UniverseIsCancelled();

/** Sets the loader's cancellation flag. */
PL_EXPORT void UniverseSetCancelled(bool cancelled);

/**
 * @brief Verifies a package's detached Ed25519 signature against the trusted key set.
 *
 * @param archivePath     Path to the `.universe` file.
 * @param signatureHex    The detached signature (lower/upper-case hex).
 * @param trustedKeysHex  A comma-separated list of trusted public keys (hex).
 * @param outError        Receives the failure message (empty on success).
 * @return true when the signature verifies against one trusted key. Fail-closed: an empty trusted
 *         set, a malformed key or a bad signature all return false rather than accepting the package.
 */
PL_EXPORT bool UniverseVerifySignature(const std::string &archivePath,
                                       const std::string &signatureHex,
                                       const std::string &trustedKeysHex,
                                       std::string *outError);

/**
 * @brief Checks that a mod's declared permissions cover the capabilities it requests.
 *
 * @param declaredCsv   The capabilities the manifest declares (comma-separated names).
 * @param requestedCsv  The capabilities the call site needs (comma-separated names).
 * @param trusted       Whether the package carried a trusted signature.
 * @param outError      Receives the first denied capability (empty when all are granted).
 * @return true when every requested capability is granted.
 */
PL_EXPORT bool UniverseCheckPermissions(const std::string &declaredCsv,
                                        const std::string &requestedCsv,
                                        bool trusted,
                                        std::string *outError);

/**
 * @brief Resolves the deterministic load order for a set of mods.
 *
 * @param nodesJson  A JSON array of `{"id": "...", "dependencies": ["..."]}` objects.
 * @return The loader's JSON report `{"order": [...], "cycle": [...]}` (or `{"error": "..."}` when
 *         the input is malformed), or an empty string when the loader is unavailable.
 */
PL_EXPORT std::string UniverseResolveLoadOrder(const std::string &nodesJson);

} // namespace pl::engine
