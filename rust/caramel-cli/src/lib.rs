//! The `caramel-cli` command implementations, kept separate from `main` so they are unit-tested.
//!
//! `pack` walks the source directory, adds every regular file under a normalised relative name,
//! refuses a symlink (which could pull in a file from outside the package), writes the ZIP
//! deterministically (entries sorted by name) and prints the SHA-256 the author signs. `sign` reads
//! the private key from hex, signs the package bytes and writes the sidecar. `verify` re-derives the
//! hash and checks the signature, which is exactly what the loader does.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use universe_core::signature::{self, PublicKey, RawSignature, TrustedKeys};
use universe_core::{sha256_hex, Error, Result};

/// A parsed command line.
#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    Pack {
        source: PathBuf,
        output: PathBuf,
    },
    Sign {
        package: PathBuf,
        private_key_hex: String,
    },
    Verify {
        package: PathBuf,
        public_key_hex: String,
    },
    Keygen {
        output: Option<PathBuf>,
    },
    Inspect {
        package: PathBuf,
    },
}

impl Command {
    /// Parses the argument vector (without the program name).
    pub fn parse(args: &[String]) -> Result<Command> {
        let (name, rest) = args
            .split_first()
            .ok_or_else(|| Error::new("no subcommand given"))?;
        match name.as_str() {
            "pack" => {
                if rest.len() != 2 {
                    return Err(Error::new("pack takes <dir> <output>"));
                }
                Ok(Command::Pack {
                    source: PathBuf::from(&rest[0]),
                    output: PathBuf::from(&rest[1]),
                })
            }
            "sign" => {
                if rest.len() != 2 {
                    return Err(Error::new("sign takes <package> <private-key-hex>"));
                }
                Ok(Command::Sign {
                    package: PathBuf::from(&rest[0]),
                    private_key_hex: rest[1].clone(),
                })
            }
            "verify" => {
                if rest.len() != 2 {
                    return Err(Error::new("verify takes <package> <public-key-hex>"));
                }
                Ok(Command::Verify {
                    package: PathBuf::from(&rest[0]),
                    public_key_hex: rest[1].clone(),
                })
            }
            "keygen" => {
                if rest.len() > 1 {
                    return Err(Error::new("keygen takes at most one output path"));
                }
                Ok(Command::Keygen {
                    output: rest.first().map(PathBuf::from),
                })
            }
            "inspect" => {
                if rest.len() != 1 {
                    return Err(Error::new("inspect takes <package>"));
                }
                Ok(Command::Inspect {
                    package: PathBuf::from(&rest[0]),
                })
            }
            other => Err(Error::new(format!("unknown subcommand '{other}'"))),
        }
    }
}

/// Runs a command.
pub fn run(command: &Command) -> Result<()> {
    match command {
        Command::Pack { source, output } => pack(source, output),
        Command::Sign {
            package,
            private_key_hex,
        } => sign(package, private_key_hex),
        Command::Verify {
            package,
            public_key_hex,
        } => verify(package, public_key_hex),
        Command::Keygen { output } => keygen(output.as_deref()),
        Command::Inspect { package } => inspect(package),
    }
}

/// The most bytes a packed package may hold, matching the loader's limit.
const MAX_PACKAGE_BYTES: u64 = 512 * 1024 * 1024;

/// Builds a ZIP from a directory, deterministically.
pub fn pack(source: &Path, output: &Path) -> Result<()> {
    if !source.is_dir() {
        return Err(Error::new(format!(
            "{} is not a directory",
            source.display()
        )));
    }
    // The manifest must exist and parse, or the package would be refused at load time.
    let manifest_path = source.join("manifest.json");
    if !manifest_path.is_file() {
        return Err(Error::new("the source directory has no manifest.json"));
    }

    let mut files = Vec::new();
    collect_files(source, source, &mut files)?;
    // Deterministic order: sorted by the relative name, so the same input packs to the same bytes.
    files.sort_by(|a, b| a.0.cmp(&b.0));

    let file = fs::File::create(output)
        .map_err(|e| Error::new(format!("could not create {}: {e}", output.display())))?;
    let mut zip = zip::ZipWriter::new(file);
    let options: zip::write::FileOptions<()> =
        zip::write::FileOptions::default().compression_method(zip::CompressionMethod::Deflated);

    let mut total: u64 = 0;
    for (name, path) in &files {
        let data = fs::read(path)
            .map_err(|e| Error::new(format!("could not read {}: {e}", path.display())))?;
        total += data.len() as u64;
        if total > MAX_PACKAGE_BYTES {
            return Err(Error::new("the package exceeds the size limit"));
        }
        zip.start_file(name, options)
            .map_err(|e| Error::new(format!("could not add {name}: {e}")))?;
        zip.write_all(&data)
            .map_err(|e| Error::new(format!("could not write {name}: {e}")))?;
    }
    zip.finish()
        .map_err(|e| Error::new(format!("could not finish the archive: {e}")))?;

    let bytes = fs::read(output)
        .map_err(|e| Error::new(format!("could not read back the package: {e}")))?;
    println!("packed {} files into {}", files.len(), output.display());
    println!("sha256 {}", sha256_hex(&bytes));
    Ok(())
}

/// Walks `dir` collecting `(relative name, path)` for every regular file, refusing symlinks.
fn collect_files(root: &Path, dir: &Path, out: &mut Vec<(String, PathBuf)>) -> Result<()> {
    let entries = fs::read_dir(dir)
        .map_err(|e| Error::new(format!("could not read {}: {e}", dir.display())))?;
    for entry in entries {
        let entry =
            entry.map_err(|e| Error::new(format!("could not read a directory entry: {e}")))?;
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path)
            .map_err(|e| Error::new(format!("could not stat {}: {e}", path.display())))?;
        if metadata.file_type().is_symlink() {
            return Err(Error::new(format!(
                "{} is a symlink; packages must contain regular files only",
                path.display()
            )));
        }
        if metadata.is_dir() {
            collect_files(root, &path, out)?;
        } else if metadata.is_file() {
            let relative = path
                .strip_prefix(root)
                .map_err(|_| Error::new("a file escaped the source directory"))?
                .to_string_lossy()
                .replace('\\', "/");
            out.push((relative, path));
        }
    }
    Ok(())
}

/// Signs a package, writing `<package>.sig`.
pub fn sign(package: &Path, private_key_hex: &str) -> Result<()> {
    let seed = universe_core::from_hex(private_key_hex.trim())?;
    let signing_key = signature::signing_key_from_bytes(&seed)?;
    let body = fs::read(package)
        .map_err(|e| Error::new(format!("could not read {}: {e}", package.display())))?;
    let raw = signature::sign(&body, &signing_key);
    let public = signature::public_key_of(&signing_key);

    let mut sidecar = package.as_os_str().to_os_string();
    sidecar.push(".sig");
    let sidecar = PathBuf::from(sidecar);
    fs::write(&sidecar, raw.to_hex())
        .map_err(|e| Error::new(format!("could not write {}: {e}", sidecar.display())))?;
    println!("signed {}", package.display());
    println!("public key {}", public.to_hex());
    Ok(())
}

/// Verifies a package against a public key.
pub fn verify(package: &Path, public_key_hex: &str) -> Result<()> {
    let key = PublicKey::from_hex(public_key_hex)?;
    let body = fs::read(package)
        .map_err(|e| Error::new(format!("could not read {}: {e}", package.display())))?;
    let mut sidecar = package.as_os_str().to_os_string();
    sidecar.push(".sig");
    let sidecar = PathBuf::from(sidecar);
    let signature_text = fs::read_to_string(&sidecar)
        .map_err(|e| Error::new(format!("could not read {}: {e}", sidecar.display())))?;
    let signature = RawSignature::from_hex(&signature_text)?;
    if signature::verify(&body, &signature, &key) {
        println!("{} verifies against {}", package.display(), public_key_hex);
        Ok(())
    } else {
        Err(Error::new("the signature does not verify"))
    }
}

/// Generates an Ed25519 key pair.
///
/// The seed comes from the OS entropy source through `getrandom`. The private key is written to
/// `output` (or printed), and the public key is printed in the hex form the trusted-key set uses.
pub fn keygen(output: Option<&Path>) -> Result<()> {
    let mut seed = [0u8; 32];
    getrandom::getrandom(&mut seed)
        .map_err(|e| Error::new(format!("no entropy available: {e}")))?;
    let signing_key = signature::signing_key_from_bytes(&seed)?;
    let public = signature::public_key_of(&signing_key);
    let private_hex = universe_core::to_hex(&seed);
    match output {
        Some(path) => {
            fs::write(path, &private_hex)
                .map_err(|e| Error::new(format!("could not write {}: {e}", path.display())))?;
            println!("wrote the private key to {}", path.display());
        }
        None => println!("private key {private_hex}"),
    }
    println!("public key {}", public.to_hex());
    Ok(())
}

/// Prints a package's manifest.
///
/// A `.singularity` manifest is a superset of a `.universe` one, so the singularity schema is tried
/// first; when it does not apply, the raw text is printed.
pub fn inspect(package: &Path) -> Result<()> {
    let path = package
        .to_str()
        .ok_or_else(|| Error::new("the package path is not valid UTF-8"))?;
    let text = universe_core::archive::read_manifest_text(path)?;
    match singularity_core::manifest::parse(&text) {
        Ok(manifest) => {
            println!("kind: singularity");
            println!("{}", singularity_core::manifest::to_json(&manifest));
            Ok(())
        }
        Err(_) => {
            println!("kind: universe");
            println!("{text}");
            Ok(())
        }
    }
}

/// A trusted key set built from the public keys given on the command line, for tests and the
/// `verify` path.
pub fn trusted_from(public_keys_hex: &[String]) -> Result<TrustedKeys> {
    let mut trusted = TrustedKeys::new();
    for (index, hex) in public_keys_hex.iter().enumerate() {
        trusted.add(format!("key-{index}"), PublicKey::from_hex(hex)?);
    }
    Ok(trusted)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!("caramel-cli-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write_minimal_universe(dir: &Path) {
        fs::create_dir_all(dir.join("lib")).unwrap();
        fs::write(
            dir.join("manifest.json"),
            r#"{
                "id": "example",
                "version": "1.0.0",
                "min_client_version": "1.0.0",
                "native_libs": ["lib/libexample.so"],
                "berry_api_dependencies": [],
                "hooks": []
            }"#,
        )
        .unwrap();
        fs::write(dir.join("lib/libexample.so"), b"\x7fELF").unwrap();
    }

    #[test]
    fn parse_rejects_unknown_commands() {
        assert!(Command::parse(&["frobnicate".to_string()]).is_err());
        assert!(Command::parse(&[]).is_err());
        assert!(Command::parse(&["pack".to_string()]).is_err());
    }

    #[test]
    fn pack_produces_a_loadable_archive() {
        let dir = temp_dir("pack");
        let source = dir.join("source");
        write_minimal_universe(&source);
        let output = dir.join("example.universe");
        pack(&source, &output).unwrap();

        // The archive the loader reads back parses to the same manifest.
        let manifest = universe_core::archive::read_manifest(output.to_str().unwrap()).unwrap();
        assert_eq!(manifest.id, "example");
    }

    #[test]
    fn pack_is_deterministic() {
        let dir = temp_dir("deterministic");
        let source = dir.join("source");
        write_minimal_universe(&source);
        let a = dir.join("a.universe");
        let b = dir.join("b.universe");
        pack(&source, &a).unwrap();
        pack(&source, &b).unwrap();
        assert_eq!(fs::read(&a).unwrap(), fs::read(&b).unwrap());
    }

    #[test]
    fn pack_refuses_a_directory_without_a_manifest() {
        let dir = temp_dir("nomanifest");
        let source = dir.join("source");
        fs::create_dir_all(&source).unwrap();
        assert!(pack(&source, &dir.join("x.universe")).is_err());
    }

    #[test]
    fn sign_then_verify_round_trips() {
        let dir = temp_dir("signverify");
        let source = dir.join("source");
        write_minimal_universe(&source);
        let package = dir.join("example.universe");
        pack(&source, &package).unwrap();

        let mut seed = [0u8; 32];
        for (index, byte) in seed.iter_mut().enumerate() {
            *byte = index as u8;
        }
        let signing = signature::signing_key_from_bytes(&seed).unwrap();
        let public = signature::public_key_of(&signing).to_hex();
        sign(&package, &universe_core::to_hex(&seed)).unwrap();
        verify(&package, &public).unwrap();
    }

    #[test]
    fn verify_fails_for_a_tampered_package() {
        let dir = temp_dir("tamper");
        let source = dir.join("source");
        write_minimal_universe(&source);
        let package = dir.join("example.universe");
        pack(&source, &package).unwrap();
        let seed = [7u8; 32];
        let signing = signature::signing_key_from_bytes(&seed).unwrap();
        let public = signature::public_key_of(&signing).to_hex();
        sign(&package, &universe_core::to_hex(&seed)).unwrap();

        // Append a byte: the hash changes and the signature no longer verifies.
        let mut bytes = fs::read(&package).unwrap();
        bytes.push(0);
        fs::write(&package, &bytes).unwrap();
        assert!(verify(&package, &public).is_err());
    }
}
