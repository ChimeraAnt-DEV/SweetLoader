//! Deterministic load ordering for `.universe` mods from their declared dependencies.
//!
//! Two properties matter, and both are pure so they are unit-tested here:
//!
//! * **Determinism.** The same set of mods loads in the same order every launch, so a failure is
//!   reproducible. Mods with no ordering constraint keep their declaration order rather than being
//!   re-sorted by name.
//! * **Cycle detection.** A dependency cycle has no valid order, so it is reported (with its
//!   members named) rather than silently broken at an arbitrary point.
//!
//! A dependency on a mod that is not installed is *not* an error: it is a missing optional mod, and
//! the dependent still loads. Refusing would turn one absent mod into a broken install.

use std::collections::HashMap;

/// One mod's identity and the mods it declares it needs loaded first.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModNode {
    pub id: String,
    pub dependencies: Vec<String>,
}

/// The result of ordering a set of mods.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LoadOrder {
    /// The mod ids in the order they should be loaded.
    pub order: Vec<String>,
    /// The ids that form a cycle, when one was found.
    pub cycle: Vec<String>,
}

impl LoadOrder {
    pub fn has_cycle(&self) -> bool {
        !self.cycle.is_empty()
    }
}

/// Orders `mods` so a mod is emitted after every dependency it declares that is present.
pub fn resolve(mods: &[ModNode]) -> LoadOrder {
    // Build the id set, keeping the first declaration of a duplicate id (a mod cannot load twice)
    // and preserving declaration order for the tie-break.
    let mut by_id: HashMap<&str, &ModNode> = HashMap::new();
    let mut input_order: Vec<&str> = Vec::new();
    for node in mods {
        if node.id.is_empty() {
            continue;
        }
        if by_id.contains_key(node.id.as_str()) {
            continue;
        }
        by_id.insert(node.id.as_str(), node);
        input_order.push(node.id.as_str());
    }

    #[derive(Clone, Copy, PartialEq)]
    enum State {
        Unvisited,
        OnStack,
        Done,
    }

    let mut state: HashMap<&str, State> = HashMap::new();
    let mut order: Vec<String> = Vec::new();
    let mut cycle: Vec<String> = Vec::new();

    // An explicit frame stack rather than recursion: a hostile package could declare a deep
    // dependency chain, and a recursive walk would overflow the game thread's stack.
    struct Frame<'a> {
        id: &'a str,
        next_dependency: usize,
    }

    for root in &input_order {
        if state.get(root).copied().unwrap_or(State::Unvisited) != State::Unvisited {
            continue;
        }
        let mut stack: Vec<Frame> = vec![Frame {
            id: root,
            next_dependency: 0,
        }];
        state.insert(root, State::OnStack);

        while !stack.is_empty() {
            let top = stack.len() - 1;
            let id = stack[top].id;
            let next_dependency = stack[top].next_dependency;
            let dependencies: &[String] = by_id
                .get(id)
                .map(|node| node.dependencies.as_slice())
                .unwrap_or(&[]);
            if next_dependency < dependencies.len() {
                let dependency = dependencies[next_dependency].as_str();
                stack[top].next_dependency += 1;
                if !by_id.contains_key(dependency) {
                    // An absent dependency is a missing optional mod, not an error.
                    continue;
                }
                match state.get(dependency).copied().unwrap_or(State::Unvisited) {
                    State::Done => {}
                    State::Unvisited => {
                        state.insert(dependency, State::OnStack);
                        stack.push(Frame {
                            id: dependency,
                            next_dependency: 0,
                        });
                    }
                    State::OnStack => {
                        if cycle.is_empty() {
                            if let Some(start) = stack.iter().position(|f| f.id == dependency) {
                                cycle = stack[start..].iter().map(|f| f.id.to_string()).collect();
                            } else {
                                cycle.push(dependency.to_string());
                            }
                        }
                    }
                }
                continue;
            }
            state.insert(id, State::Done);
            order.push(id.to_string());
            stack.pop();
        }
    }

    LoadOrder { order, cycle }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn node(id: &str, dependencies: &[&str]) -> ModNode {
        ModNode {
            id: id.to_string(),
            dependencies: dependencies.iter().map(|s| s.to_string()).collect(),
        }
    }

    #[test]
    fn independent_mods_keep_their_declaration_order() {
        let mods = vec![node("a", &[]), node("b", &[]), node("c", &[])];
        assert_eq!(resolve(&mods).order, vec!["a", "b", "c"]);
    }

    #[test]
    fn a_dependency_loads_before_its_dependent() {
        let mods = vec![node("app", &["lib"]), node("lib", &[])];
        let order = resolve(&mods).order;
        let lib = order.iter().position(|id| id == "lib").unwrap();
        let app = order.iter().position(|id| id == "app").unwrap();
        assert!(lib < app, "lib must load before app: {order:?}");
    }

    #[test]
    fn an_absent_dependency_is_not_an_error() {
        let mods = vec![node("app", &["missing"])];
        let order = resolve(&mods);
        assert_eq!(order.order, vec!["app"]);
        assert!(!order.has_cycle());
    }

    #[test]
    fn a_cycle_is_reported_with_its_members() {
        let mods = vec![node("a", &["b"]), node("b", &["a"])];
        let order = resolve(&mods);
        assert!(order.has_cycle());
        assert!(order.cycle.contains(&"a".to_string()));
        assert!(order.cycle.contains(&"b".to_string()));
    }

    #[test]
    fn a_self_dependency_is_a_cycle() {
        let order = resolve(&[node("a", &["a"])]);
        assert!(order.has_cycle());
    }

    #[test]
    fn a_deep_chain_does_not_overflow_the_stack() {
        // A hostile package can declare a very deep chain; the iterative walk must survive it.
        let mut mods = Vec::new();
        let depth = 20_000;
        for index in 0..depth {
            let dependency = if index + 1 < depth {
                vec![format!("m{}", index + 1)]
            } else {
                Vec::new()
            };
            mods.push(ModNode {
                id: format!("m{index}"),
                dependencies: dependency,
            });
        }
        let order = resolve(&mods);
        assert_eq!(order.order.len(), depth);
        assert!(!order.has_cycle());
    }

    #[test]
    fn duplicate_ids_load_once() {
        let mods = vec![node("a", &[]), node("a", &[])];
        assert_eq!(resolve(&mods).order, vec!["a"]);
    }
}
