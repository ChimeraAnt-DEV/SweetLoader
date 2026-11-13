//! Persistent per-mod configuration, as JSON.
//!
//! A mod's settings live in `<mod dir>/config.json`. The rules here exist because config is written
//! from the UI and read at load time inside the game process:
//!
//! * **Atomic write.** The file is written to a temporary sibling and renamed over the target, so a
//!   crash mid-write cannot leave a truncated file. A truncated config is worse than a missing one,
//!   because the next load reads garbage.
//! * **Rotating backups.** The previous [`MAX_BACKUPS`] good configs are kept as `config.json.bak.N`,
//!   so a bad edit is recoverable without a device-level file manager.
//! * **Fail-safe load.** A missing or unparseable config loads as empty rather than faulting; the
//!   mod sees its defaults, not a crash.
//!
//! The store is a JSON object; typed getters coerce within reason (an integer reads as a float, a
//! number reads as a string) so a UI that stores everything as a string still round-trips.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

use crate::{Error, Result};

/// How many previous configs to keep.
pub const MAX_BACKUPS: usize = 3;

/// A mod's configuration store.
#[derive(Debug, Clone, Default)]
pub struct Config {
    values: serde_json::Map<String, Value>,
}

impl Config {
    /// An empty config.
    pub fn new() -> Config {
        Config::default()
    }

    /// Parses a config from JSON text, refusing non-object JSON.
    pub fn from_json(text: &str) -> Result<Config> {
        let value: Value = serde_json::from_str(text)
            .map_err(|e| Error::new(format!("config is not valid JSON: {e}")))?;
        match value {
            Value::Object(map) => Ok(Config { values: map }),
            _ => Err(Error::new("config must be a JSON object")),
        }
    }

    /// The config as pretty JSON text.
    pub fn to_json(&self) -> String {
        Value::Object(self.values.clone()).to_string()
    }

    /// Loads a config from `path`.
    ///
    /// A missing file is an empty config, not an error: a mod that has never been configured must
    /// start with its defaults. A *present but unparseable* file is reported as an error so the
    /// caller can log it, while still returning an empty config to run with.
    pub fn load(path: &Path) -> Config {
        match fs::read_to_string(path) {
            Ok(text) => Config::from_json(&text).unwrap_or_default(),
            Err(_) => Config::new(),
        }
    }

    /// Saves the config to `path` atomically, rotating the previous copies into backups.
    pub fn save(&self, path: &Path) -> Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| Error::new(format!("could not create the config directory: {e}")))?;
        }
        let temporary = sibling(path, "tmp");
        fs::write(&temporary, self.to_json())
            .map_err(|e| Error::new(format!("could not write the config: {e}")))?;

        if path.exists() {
            rotate_backups(path);
        }
        fs::rename(&temporary, path)
            .map_err(|e| Error::new(format!("could not replace the config: {e}")))?;
        Ok(())
    }

    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    pub fn contains_key(&self, key: &str) -> bool {
        self.values.contains_key(key)
    }

    pub fn keys(&self) -> impl Iterator<Item = &String> {
        self.values.keys()
    }

    /// Sets a value, overwriting any existing one.
    pub fn set(&mut self, key: &str, value: Value) {
        self.values.insert(key.to_string(), value);
    }

    /// Sets an integer value.
    pub fn set_int(&mut self, key: &str, value: i64) {
        self.set(key, Value::from(value));
    }

    /// Sets a floating-point value.
    pub fn set_float(&mut self, key: &str, value: f64) {
        self.set(key, Value::from(value));
    }

    /// Sets a boolean value.
    pub fn set_bool(&mut self, key: &str, value: bool) {
        self.set(key, Value::from(value));
    }

    /// Sets a string value.
    pub fn set_string(&mut self, key: &str, value: &str) {
        self.set(key, Value::from(value));
    }

    /// Removes a key, returning whether it was present.
    pub fn remove(&mut self, key: &str) -> bool {
        self.values.remove(key).is_some()
    }

    /// Reads an integer, or `default` when the key is absent or not coercible.
    pub fn get_int(&self, key: &str, default: i64) -> i64 {
        match self.values.get(key) {
            Some(Value::Number(number)) => number.as_i64().unwrap_or(default),
            Some(Value::String(text)) => text.trim().parse().unwrap_or(default),
            Some(Value::Bool(flag)) => i64::from(*flag),
            _ => default,
        }
    }

    /// Reads a float, or `default` when the key is absent or not coercible.
    pub fn get_float(&self, key: &str, default: f64) -> f64 {
        match self.values.get(key) {
            Some(Value::Number(number)) => number.as_f64().unwrap_or(default),
            Some(Value::String(text)) => text.trim().parse().unwrap_or(default),
            _ => default,
        }
    }

    /// Reads a boolean, or `default` when the key is absent or not coercible.
    pub fn get_bool(&self, key: &str, default: bool) -> bool {
        match self.values.get(key) {
            Some(Value::Bool(flag)) => *flag,
            Some(Value::String(text)) => match text.trim().to_ascii_lowercase().as_str() {
                "true" | "1" | "yes" | "on" => true,
                "false" | "0" | "no" | "off" => false,
                _ => default,
            },
            Some(Value::Number(number)) => number.as_i64().map(|v| v != 0).unwrap_or(default),
            _ => default,
        }
    }

    /// Reads a string, or `default` when the key is absent or not coercible.
    pub fn get_string(&self, key: &str, default: &str) -> String {
        match self.values.get(key) {
            Some(Value::String(text)) => text.clone(),
            Some(Value::Number(number)) => number.to_string(),
            Some(Value::Bool(flag)) => flag.to_string(),
            _ => default.to_string(),
        }
    }
}

/// Builds a sibling path by appending `suffix` to the file name (`config.json` -> `config.json.tmp`).
fn sibling(path: &Path, suffix: &str) -> PathBuf {
    let mut name = path.file_name().map(|n| n.to_os_string()).unwrap_or_default();
    name.push(".");
    name.push(suffix);
    path.with_file_name(name)
}

/// Shifts `config.json.bak.1..N` down one and moves the current config into `.bak.1`.
fn rotate_backups(path: &Path) {
    for index in (1..MAX_BACKUPS).rev() {
        let from = sibling(path, &format!("bak.{index}"));
        let to = sibling(path, &format!("bak.{}", index + 1));
        if from.exists() {
            let _ = fs::rename(&from, &to);
        }
    }
    let _ = fs::rename(path, sibling(path, "bak.1"));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!("universe-config-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn round_trips_through_json() {
        let mut config = Config::new();
        config.set_int("render_distance", 12);
        config.set_bool("glow", true);
        config.set_string("theme", "caramel");
        let text = config.to_json();
        let parsed = Config::from_json(&text).unwrap();
        assert_eq!(parsed.get_int("render_distance", 0), 12);
        assert!(parsed.get_bool("glow", false));
        assert_eq!(parsed.get_string("theme", ""), "caramel");
    }

    #[test]
    fn typed_getters_are_fail_safe() {
        let config = Config::from_json(r#"{"n": "7", "f": 2.5, "b": "yes", "s": 3}"#).unwrap();
        assert_eq!(config.get_int("n", 0), 7);
        assert_eq!(config.get_float("f", 0.0), 2.5);
        assert!(config.get_bool("b", false));
        assert_eq!(config.get_string("s", ""), "3");
        // Absent and mistyped keys read as the default, never a panic.
        assert_eq!(config.get_int("absent", 42), 42);
        assert_eq!(config.get_int("f", -1), -1);
    }

    #[test]
    fn refuses_a_non_object_config() {
        assert!(Config::from_json("[1,2,3]").is_err());
        assert!(Config::from_json("not json").is_err());
    }

    #[test]
    fn a_missing_file_loads_empty() {
        let dir = temp_dir("missing");
        let config = Config::load(&dir.join("config.json"));
        assert!(config.is_empty());
    }

    #[test]
    fn saves_and_reloads() {
        let dir = temp_dir("save");
        let path = dir.join("config.json");
        let mut config = Config::new();
        config.set_int("a", 1);
        config.save(&path).unwrap();
        let reloaded = Config::load(&path);
        assert_eq!(reloaded.get_int("a", 0), 1);
    }

    #[test]
    fn keeps_three_rotating_backups() {
        let dir = temp_dir("backups");
        let path = dir.join("config.json");
        for round in 0..6 {
            let mut config = Config::new();
            config.set_int("round", round);
            config.save(&path).unwrap();
        }
        // The live file holds the last round, and the three most recent prior rounds survive.
        assert_eq!(Config::load(&path).get_int("round", -1), 5);
        assert_eq!(Config::load(&sibling(&path, "bak.1")).get_int("round", -1), 4);
        assert_eq!(Config::load(&sibling(&path, "bak.2")).get_int("round", -1), 3);
        assert_eq!(Config::load(&sibling(&path, "bak.3")).get_int("round", -1), 2);
        // A fourth backup is never created.
        assert!(!sibling(&path, "bak.4").exists());
    }

    #[test]
    fn an_unparseable_file_loads_empty_without_faulting() {
        let dir = temp_dir("garbage");
        let path = dir.join("config.json");
        fs::write(&path, "{ not json").unwrap();
        assert!(Config::load(&path).is_empty());
    }
}
