//! The mod load plan: which mods to load, in what order, and which to refuse.
//!
//! Loading is the one place where three separate rules meet, so the decision is made in one pure
//! function rather than spread across the loader's C ABI:
//!
//! 1. **Order** — a mod loads after every dependency it declares (`loadorder`).
//! 2. **Cycles** — a mod in a dependency cycle is refused, along with the rest of the cycle, rather
//!    than partially loaded.
//! 3. **Faults** — a mod that exhausted its fault budget is refused before anything is opened, so a
//!    broken mod costs one attempt per launch, not one attempt per launch forever.
//!
//! A plan is data: the loader opens exactly the libraries the plan lists, in the plan's order, and
//! nothing else. That is what makes "refuse to load it" verifiable instead of a promise.

use crate::fault::FaultTracker;
use crate::loadorder::{self, ModNode};

/// One installed mod the loader knows about.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModSpec {
    pub id: String,
    /// The extracted native library to `dlopen`, or empty for a manifest-only mod.
    pub lib_path: String,
    /// Other mod ids this mod must load after.
    pub dependencies: Vec<String>,
}

/// The outcome of planning a load: what to open, and what was refused and why.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct LoadPlan {
    /// The mod ids to load, in load order.
    pub load: Vec<String>,
    /// Mods refused because they exhausted their fault budget.
    pub disabled: Vec<String>,
    /// Mods refused because they are part of a dependency cycle.
    pub cycle: Vec<String>,
}

impl LoadPlan {
    /// True when nothing is to be loaded.
    pub fn is_empty(&self) -> bool {
        self.load.is_empty()
    }
}

/// Plans a load for `specs`, given the session's fault state.
///
/// `specs` is in the launcher's own order (whatever order the user's mod list is in); the returned
/// plan is the order the loader must use. A mod that is disabled or cyclic is never in `load`.
pub fn plan(specs: &[ModSpec], tracker: &FaultTracker) -> LoadPlan {
    let nodes: Vec<ModNode> = specs
        .iter()
        .map(|spec| ModNode {
            id: spec.id.clone(),
            dependencies: spec.dependencies.clone(),
        })
        .collect();
    let resolved = loadorder::resolve(&nodes);

    let mut plan = LoadPlan::default();
    for id in &resolved.order {
        if resolved.cycle.contains(id) {
            continue;
        }
        if tracker.is_disabled(id) {
            plan.disabled.push(id.clone());
            continue;
        }
        plan.load.push(id.clone());
    }
    // The cycle list is reported whole, even when a cycle member would also be disabled: the user
    // needs to see the dependency mistake, which is theirs to fix.
    plan.cycle = resolved.cycle;
    plan
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec(id: &str, deps: &[&str]) -> ModSpec {
        ModSpec {
            id: id.to_string(),
            lib_path: format!("lib/lib{id}.so"),
            dependencies: deps.iter().map(|d| d.to_string()).collect(),
        }
    }

    #[test]
    fn orders_dependencies_before_dependents() {
        let specs = vec![spec("app", &["lib"]), spec("lib", &[])];
        let plan = plan(&specs, &FaultTracker::new());
        assert_eq!(plan.load, vec!["lib", "app"]);
        assert!(plan.cycle.is_empty());
        assert!(plan.disabled.is_empty());
    }

    #[test]
    fn refuses_a_cycle_rather_than_loading_it_partially() {
        let specs = vec![spec("a", &["b"]), spec("b", &["a"])];
        let plan = plan(&specs, &FaultTracker::new());
        assert!(plan.load.is_empty(), "a cycle loads nothing: {plan:?}");
        assert_eq!(plan.cycle.len(), 2);
    }

    #[test]
    fn refuses_a_disabled_mod_and_reports_it() {
        let mut tracker = FaultTracker::new();
        for _ in 0..crate::fault::MAX_FAULTS {
            tracker.record_fault("bad");
        }
        let specs = vec![spec("good", &[]), spec("bad", &[])];
        let plan = plan(&specs, &tracker);
        assert_eq!(plan.load, vec!["good"]);
        assert_eq!(plan.disabled, vec!["bad"]);
    }

    #[test]
    fn an_absent_dependency_is_not_an_error() {
        // The user simply has not installed it; the mod still loads.
        let specs = vec![spec("app", &["missing"])];
        let plan = plan(&specs, &FaultTracker::new());
        assert_eq!(plan.load, vec!["app"]);
    }

    #[test]
    fn an_empty_set_plans_nothing() {
        let plan = plan(&[], &FaultTracker::new());
        assert!(plan.is_empty());
    }
}
