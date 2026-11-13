//! `caramel-cli` — packages and signs CaramelClient mod packages.
//!
//! Subcommands:
//!
//! * `pack` — build a `.universe` (or `.singularity`) ZIP from a directory that holds a
//!   `manifest.json` and the files it names.
//! * `sign` — write the detached Ed25519 sidecar (`<package>.sig`) over a package's bytes.
//! * `keygen` — generate an Ed25519 key pair, writing the private key and printing the public key
//!   in the hex form the client's trusted-key set uses.
//! * `verify` — check a package against a public key.
//! * `inspect` — print a package's manifest.
//!
//! The loader verifies exactly the bytes this tool writes, so `pack` + `sign` produce a package the
//! client accepts and an unsigned or tampered one it refuses.

use std::process::ExitCode;

use caramel_cli::{run, Command};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let command = match Command::parse(&args) {
        Ok(command) => command,
        Err(message) => {
            eprintln!("error: {message}");
            print_usage();
            return ExitCode::from(2);
        }
    };
    match run(&command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}

fn print_usage() {
    eprintln!(
        "caramel-cli {}\n\
         \n\
         usage:\n\
         \x20 caramel-cli pack <dir> <out.universe|out.singularity>\n\
         \x20 caramel-cli sign <package> <private-key-hex>\n\
         \x20 caramel-cli verify <package> <public-key-hex>\n\
         \x20 caramel-cli keygen [private-key-out]\n\
         \x20 caramel-cli inspect <package>\n",
        env!("CARGO_PKG_VERSION")
    );
}

