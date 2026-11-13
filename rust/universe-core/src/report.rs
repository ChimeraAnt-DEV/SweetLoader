//! JSON shaping for the loader's C ABI.
//!
//! The C ABI hands structured results back as a JSON string, because a `char *` is the only thing a
//! C caller can free and parse without an agreed struct layout. Building that JSON is a pure
//! function, so it lives here (tested on the host) rather than in the `cdylib`, which stays a thin
//! marshalling shell.
//!
//! The one rule that matters: a mod id is attacker-controlled, so it is escaped before it is put in
//! a JSON string. An id containing a quote would otherwise let a package forge the report the
//! launcher reads.

use crate::loadorder::{self, LoadOrder, ModNode};

/// Escapes `text` for inclusion in a JSON string literal.
pub fn escape_json(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for character in text.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Renders a slice of strings as a JSON array, escaping each element.
pub fn json_string_array(items: &[String]) -> String {
    let escaped: Vec<String> = items
        .iter()
        .map(|item| format!("\"{}\"", escape_json(item)))
        .collect();
    format!("[{}]", escaped.join(","))
}

/// Renders a [`LoadOrder`] as `{"order": [...], "cycle": [...]}`.
pub fn load_order_json(order: &LoadOrder) -> String {
    format!(
        "{{\"order\":{},\"cycle\":{}}}",
        json_string_array(&order.order),
        json_string_array(&order.cycle)
    )
}

/// Renders a [`LoadPlan`] plus the live-mod list as the loader's load report.
pub fn load_plan_json(plan: &crate::runtime::LoadPlan, live: &[String]) -> String {
    format!(
        "{{\"load\":{},\"disabled\":{},\"cycle\":{},\"live\":{}}}",
        json_string_array(&plan.load),
        json_string_array(&plan.disabled),
        json_string_array(&plan.cycle),
        json_string_array(live)
    )
}

/// Parses a JSON array of `{"id": ..., "lib_path": ..., "dependencies": [...]}` objects.
pub fn parse_mod_specs(json: &str) -> crate::Result<Vec<crate::runtime::ModSpec>> {
    #[derive(serde::Deserialize)]
    struct Spec {
        id: String,
        #[serde(default)]
        lib_path: String,
        #[serde(default)]
        dependencies: Vec<String>,
    }

    let specs: Vec<Spec> = serde_json::from_str(json)
        .map_err(|error| crate::Error::new(format!("the mod list is not valid JSON: {error}")))?;
    Ok(specs
        .into_iter()
        .map(|spec| crate::runtime::ModSpec {
            id: spec.id,
            lib_path: spec.lib_path,
            dependencies: spec.dependencies,
        })
        .collect())
}

/// Parses a JSON array of `{"id": ..., "dependencies": [...]}` objects into [`ModNode`]s.
pub fn parse_mod_nodes(json: &str) -> crate::Result<Vec<ModNode>> {
    #[derive(serde::Deserialize)]
    struct Node {
        id: String,
        #[serde(default)]
        dependencies: Vec<String>,
    }

    let nodes: Vec<Node> = serde_json::from_str(json)
        .map_err(|error| crate::Error::new(format!("the node list is not valid JSON: {error}")))?;
    Ok(nodes
        .into_iter()
        .map(|node| ModNode {
            id: node.id,
            dependencies: node.dependencies,
        })
        .collect())
}

/// Parses a node list and resolves its load order, returning one JSON object.
///
/// Always succeeds with a JSON object: a parse failure comes back as `{"error": "..."}`, so the C
/// caller parses one shape and never has to distinguish "no result" from "empty result".
pub fn resolve_load_order_json(nodes_json: &str) -> String {
    match parse_mod_nodes(nodes_json) {
        Ok(mods) => load_order_json(&loadorder::resolve(&mods)),
        Err(error) => format!("{{\"error\":\"{}\"}}", escape_json(&error.to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn escapes_quotes_backslashes_and_controls() {
        assert_eq!(escape_json("a\"b"), "a\\\"b");
        assert_eq!(escape_json("a\\b"), "a\\\\b");
        assert_eq!(escape_json("a\nb"), "a\\nb");
        assert_eq!(escape_json("a\u{1}b"), "a\\u0001b");
    }

    #[test]
    fn renders_an_array_with_escaped_elements() {
        let items = vec!["a".to_string(), "b\"c".to_string()];
        assert_eq!(json_string_array(&items), r#"["a","b\"c"]"#);
    }

    #[test]
    fn resolves_and_renders_an_order() {
        let json = r#"[{"id":"app","dependencies":["lib"]},{"id":"lib"}]"#;
        let report = resolve_load_order_json(json);
        assert!(report.starts_with("{\"order\":"), "{report}");
        assert!(report.contains("\"cycle\":[]"), "{report}");
        assert!(report.find("lib").unwrap() < report.find("app").unwrap());
    }

    #[test]
    fn reports_a_cycle() {
        let json = r#"[{"id":"a","dependencies":["b"]},{"id":"b","dependencies":["a"]}]"#;
        let report = resolve_load_order_json(json);
        assert!(report.contains("\"cycle\":[\"a\",\"b\"]") || report.contains("\"cycle\":[\"b\",\"a\"]"), "{report}");
    }

    #[test]
    fn bad_json_becomes_an_error_object() {
        let report = resolve_load_order_json("not json");
        assert!(report.starts_with("{\"error\":\""), "{report}");
    }

    #[test]
    fn a_hostile_id_is_escaped_in_the_report() {
        let json = r#"[{"id":"a\",\"order\":[\"evil"}]"#;
        let report = resolve_load_order_json(json);
        // The forged fragment must be escaped, not emitted as structure.
        assert!(report.contains("\\\""), "{report}");
        assert!(!report.contains("\"order\":[\"evil\"]"), "{report}");
    }

    #[test]
    fn parses_mod_specs() {
        let specs = parse_mod_specs(r#"[{"id":"a","lib_path":"lib/liba.so","dependencies":["b"]}]"#)
            .unwrap();
        assert_eq!(specs.len(), 1);
        assert_eq!(specs[0].lib_path, "lib/liba.so");
        assert_eq!(specs[0].dependencies, vec!["b"]);
        // lib_path and dependencies are optional.
        let specs = parse_mod_specs(r#"[{"id":"a"}]"#).unwrap();
        assert!(specs[0].lib_path.is_empty());
        assert!(specs[0].dependencies.is_empty());
    }

    #[test]
    fn renders_a_load_plan_report() {
        let plan = crate::runtime::LoadPlan {
            load: vec!["a".to_string()],
            disabled: vec!["b".to_string()],
            cycle: vec![],
        };
        let report = load_plan_json(&plan, &["a".to_string()]);
        assert!(report.contains("\"load\":[\"a\"]"), "{report}");
        assert!(report.contains("\"disabled\":[\"b\"]"), "{report}");
        assert!(report.contains("\"live\":[\"a\"]"), "{report}");
    }
}
