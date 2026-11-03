// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::VecDeque;
use std::io::ErrorKind;
use std::sync::OnceLock;

use dep_graph_delta::DepGraphDelta;
use dep_graph_delta::HashSetDelta;
use depgraph_reader::BaseDepgraphTrait;
use depgraph_reader::Dep;
use depgraph_reader::DepGraph;
use hash::HashSet;
use itertools::Either;
use parking_lot::MappedRwLockReadGuard;
use parking_lot::MappedRwLockWriteGuard;
use parking_lot::RwLock;
use parking_lot::RwLockReadGuard;
use parking_lot::RwLockWriteGuard;
use rpds::HashTrieSet;
use typing_deps_hash::DepType;

use crate::DepGraphLoadError;
use crate::DepSet;
use crate::RawTypingDepsMode;
use crate::TypingDepsMode;

enum BaseDepGraph<B> {
    Uninitialized,
    Loaded(Option<B>),
}

pub struct DepGraphWithDelta<B: BaseDepgraphTrait> {
    /// A structure wrapping the memory-mapped dependency graph.
    /// Each worker will itself lazily (or eagerly upon request)
    /// open a memory-mapping to the dependency graph.
    ///
    /// `Loaded(None)` means that custom mode is enabled without an existing
    /// saved state, while `Uninitialized` means no load has been attempted.
    base: BaseDepGraph<B>,
    /// The dependency graph delta.
    ///
    /// Even though this is only used in a single-threaded context (from OCaml)
    /// we wrap it in a `Mutex` to ensure safety.
    delta: DepGraphDelta,
}

impl<B: BaseDepgraphTrait> Default for DepGraphWithDelta<B> {
    fn default() -> Self {
        Self {
            base: BaseDepGraph::Uninitialized,
            delta: DepGraphDelta::default(),
        }
    }
}

impl<B: BaseDepgraphTrait> DepGraphWithDelta<B> {
    pub fn new(base: Option<B>, delta: DepGraphDelta) -> Self {
        Self {
            base: BaseDepGraph::Loaded(base),
            delta,
        }
    }

    pub fn delta(&self) -> &DepGraphDelta {
        &self.delta
    }

    pub fn delta_mut(&mut self) -> &mut DepGraphDelta {
        &mut self.delta
    }

    pub fn base(&self) -> Option<&B> {
        match &self.base {
            BaseDepGraph::Uninitialized => None,
            BaseDepGraph::Loaded(base) => base.as_ref(),
        }
    }

    fn is_base_loaded(&self) -> bool {
        matches!(self.base, BaseDepGraph::Loaded(_))
    }

    /// Iterates over all dependents of a dependency, with possibly duplicate dependents.
    pub fn iter_dependents_with_duplicates<F, R>(&self, dep: Dep, mut f: F) -> R
    where
        F: FnMut(&mut dyn Iterator<Item = Dep>) -> R,
    {
        let Self { delta, .. } = self;
        let base = self.base();
        let HashSetDelta { added, removed } = delta.get(dep);
        let mut added_iter = added
            .map(|s| s.iter().copied())
            .map_or(Either::Right(std::iter::empty::<Dep>()), Either::Left);
        let is_removed = |d: &Dep| removed.is_some_and(|removed| removed.contains(d));
        match base {
            None => f(&mut added_iter),
            Some(base) => {
                let base_iter = base.iter_dependents(dep);
                f(&mut added_iter.chain(base_iter.filter(|d| !is_removed(d))))
            }
        }
    }

    pub fn has_edge(&self, dependent: Dep, dependency: Dep) -> bool {
        let Self { delta, .. } = self;
        let base = self.base();
        let HashSetDelta { added, removed } = delta.get(dependency);
        added.is_some_and(|added| added.contains(&dependent))
            || (base
                .as_ref()
                .is_some_and(|g| g.dependent_dependency_edge_exists(dependent, dependency))
                && !removed.is_some_and(|removed| removed.contains(&dependent)))
    }

    /// Returns the recursive 'extends' dependents of a dep.
    /// Does not include the dep itself.
    pub fn get_extend_deps(
        &self,
        visited: &mut HashSet<Dep>,
        source_class: Dep,
        acc: &mut HashTrieSet<Dep>,
    ) {
        let mut queue = VecDeque::new();
        self.get_extend_deps_visit(visited, &mut queue, source_class, acc);
        while let Some(source_class) = queue.pop_front() {
            self.get_extend_deps_visit(visited, &mut queue, source_class, acc);
        }
    }

    /// Helper function to recursively get extend deps
    fn get_extend_deps_visit(
        &self,
        visited: &mut HashSet<Dep>,
        queue: &mut VecDeque<Dep>,
        source_class: Dep,
        acc: &mut HashTrieSet<Dep>,
    ) {
        if !visited.insert(source_class) {
            return;
        }
        if source_class.is_class() {
            let extends_hash = source_class.class_to_extends().unwrap();
            self.iter_dependents_with_duplicates(extends_hash, |iter| {
                iter.for_each(|dep: Dep| {
                    if dep.is_class() {
                        if !acc.contains(&dep) {
                            acc.insert_mut(dep);
                            queue.push_back(dep);
                        }
                    }
                })
            });
        }
    }

    /// Returns the union of the provided dep set and their recursive 'extends' dependents.
    pub fn add_extend_deps(&self, acc: &mut HashTrieSet<Dep>) {
        let mut visited = HashSet::default();
        let mut queue = VecDeque::new();
        for source_class in acc.iter() {
            queue.push_back(*source_class);
        }
        while let Some(source_class) = queue.pop_front() {
            self.get_extend_deps_visit(&mut visited, &mut queue, source_class, acc);
        }
    }

    /// The direct dependents of `dep`, stopping early once there are more than
    /// `max`.
    ///
    /// The result is the complete set, or some partial subset of it that is big
    /// enough to show the limit was exceeded.
    pub fn get_dependents_bounded(&self, dep: Dep, max: usize) -> HashTrieSet<Dep> {
        self.iter_dependents_with_duplicates(dep, |iter| {
            let mut dependents = HashTrieSet::new();
            insert_until_over(&mut dependents, iter, max);
            dependents
        })
    }

    /// Adds to `acc` the direct dependents of every dep in `query`, stopping
    /// early once `acc` holds more than `max`.
    ///
    /// With `acc` starting as a copy of `query`, the result is the complete
    /// union, or some partial subset of it that is big enough to show the limit
    /// was exceeded.
    pub fn add_dependents_bounded(
        &self,
        query: &HashTrieSet<Dep>,
        acc: &mut HashTrieSet<Dep>,
        max: usize,
    ) {
        for dependency in query.iter() {
            if acc.size() > max {
                break;
            }
            self.iter_dependents_with_duplicates(*dependency, |iter| {
                insert_until_over(acc, iter, max)
            });
        }
    }

    /// The fanout of a member `m` in type `A` contains:
    /// - the members `m` in descendants of `A` down to the first members `m` which are declared.
    /// - the dependents of those members `m` in descendants,
    ///   but excluding dependents of declared members.
    ///
    /// We also include `A::m` itself in the result.
    /// The computed fanout is added to the provided fanout accumulator.
    pub fn get_member_fanout(
        &self,
        class_dep: Dep,
        member_type: DepType,
        member_name: &str,
        fanout_acc: &mut HashTrieSet<Dep>,
    ) {
        let mut queue = VecDeque::new();
        let mut visited = HashSet::default();

        self.visit_class_dep_for_member_fanout(
            class_dep,
            member_type,
            member_name,
            &mut visited,
            &mut queue,
            fanout_acc,
            false,
        );

        while let Some(class_dep) = queue.pop_front() {
            self.visit_class_dep_for_member_fanout(
                class_dep,
                member_type,
                member_name,
                &mut visited,
                &mut queue,
                fanout_acc,
                true,
            );
        }
    }

    fn visit_class_dep_for_member_fanout(
        &self,
        class_dep: Dep,
        member_type: DepType,
        member_name: &str,
        visited: &mut HashSet<Dep>,
        queue: &mut VecDeque<Dep>,
        fanout_acc: &mut HashTrieSet<Dep>,
        stop_if_declared: bool,
    ) {
        if !visited.insert(class_dep) {
            return;
        }
        fanout_acc.insert_mut(class_dep);
        let member_dep = class_dep.member(member_type, member_name);
        let mut member_is_declared = false;
        let mut member_deps = HashSet::default();
        self.iter_dependents_with_duplicates(member_dep, |deps| {
            for dep in deps {
                if dep.is_declares() {
                    member_is_declared = true;
                    if stop_if_declared {
                        break;
                    }
                } else {
                    member_deps.insert(dep);
                }
            }
        });
        if !(stop_if_declared && member_is_declared) {
            member_deps
                .into_iter()
                .for_each(|dep| fanout_acc.insert_mut(dep));
            let extends_dep_for_class = class_dep.class_to_extends().unwrap();
            self.iter_dependents_with_duplicates(extends_dep_for_class, |iter| {
                iter.for_each(|dep| queue.push_back(dep));
            });
        }
    }

    /// Follow any NotSubtype edge from provided deps and add these edges'
    /// ends to the fanout.
    pub fn get_not_subtype_fanout(&self, deps: &DepSet, fanout_acc: &mut HashTrieSet<Dep>) {
        for class_dep in deps.iter() {
            let not_subtype_dep = class_dep.class_to_not_subtype();
            self.iter_dependents_with_duplicates(not_subtype_dep, |deps| {
                for dep in deps {
                    fanout_acc.insert_mut(dep);
                }
            });
        }
    }

    pub fn remove(&mut self, dependent: Dep, dependency: Dep) {
        self.delta.remove(dependent, dependency)
    }
}

impl DepGraphWithDelta<DepGraph> {
    /// Load the graph using the given mode.
    ///
    /// The mode is only used on the first call, to establish some global state, and
    /// then ignored for future calls.
    fn load_base(&mut self, mode: TypingDepsMode) -> Result<(), DepGraphLoadError> {
        if !self.is_base_loaded() {
            self.base = BaseDepGraph::Loaded(Self::base_dep_graph_from_mode(mode)?);
        }

        Ok(())
    }

    /// Override the loaded dep graph.
    pub fn replace_dep_graph(&mut self, mode: TypingDepsMode) -> Result<(), DepGraphLoadError> {
        self.base = BaseDepGraph::Loaded(Self::base_dep_graph_from_mode(mode)?);
        Ok(())
    }

    fn base_dep_graph_from_mode(
        mode: TypingDepsMode,
    ) -> Result<Option<DepGraph>, DepGraphLoadError> {
        match mode {
            TypingDepsMode::InMemoryMode(None)
            | TypingDepsMode::SaveToDiskMode {
                graph: None,
                new_edges_dir: _,
                human_readable_dep_map_dir: _,
            } => {
                // Enabled, but we don't have a saved-state, so we can't open it
                Ok(None)
            }
            TypingDepsMode::InMemoryMode(Some(depgraph_fn))
            | TypingDepsMode::SaveToDiskMode {
                graph: Some(depgraph_fn),
                new_edges_dir: _,
                human_readable_dep_map_dir: _,
            } => {
                // We are opening and intializing the dep graph while holding onto the mutex...
                // Which typically isn't great, but since ocaml is single threaded, it's ok.
                let depgraph = DepGraph::from_path(&depgraph_fn).map_err(|error| {
                    if error.kind() == ErrorKind::NotFound {
                        DepGraphLoadError::DepgraphNotFound(depgraph_fn.clone())
                    } else {
                        DepGraphLoadError::DepgraphOpenError(format!(
                            "could not open dep graph file {depgraph_fn:?}: {error}"
                        ))
                    }
                })?;
                Ok(Some(depgraph))
            }
        }
    }
}

pub struct LockedDepgraphWithDelta(RwLock<OnceLock<DepGraphWithDelta<DepGraph>>>);

impl LockedDepgraphWithDelta {
    pub const fn new() -> Self {
        Self(RwLock::new(OnceLock::new()))
    }

    fn read_init(&self) -> MappedRwLockReadGuard<'_, DepGraphWithDelta<DepGraph>> {
        RwLockReadGuard::map(self.0.read(), |c| c.get_or_init(DepGraphWithDelta::default))
    }

    fn write_init(&self) -> MappedRwLockWriteGuard<'_, DepGraphWithDelta<DepGraph>> {
        RwLockWriteGuard::map(self.0.write(), |c| {
            c.get_or_init(DepGraphWithDelta::default);
            c.get_mut().unwrap()
        })
    }

    fn mode_from_raw(mode: RawTypingDepsMode) -> Result<TypingDepsMode, DepGraphLoadError> {
        // SAFETY: Callers invoke this while the OCaml runtime is interrupted by
        // the FFI call which supplied `mode`, so the pointed-to value cannot
        // move or be modified during conversion.
        unsafe { mode.to_rust() }
            .map_err(|error| DepGraphLoadError::DepgraphInvalidMode(format!("{error:?}")))
    }

    /// Locks the depgraph delta for reading, which also maintains a lock on the depgraph
    ///
    /// # Panics
    ///
    /// When another reference to delta is still active, but that
    /// isn't likely, given that we only have one thread, and the
    /// `with`/`with_mut` auxiliary functions disallow the reference
    /// to escape.
    pub fn read_delta(&self) -> MappedRwLockReadGuard<'_, DepGraphDelta> {
        MappedRwLockReadGuard::map(self.read_init(), |g| g.delta())
    }

    /// Locks the depgraph delta for writing, which also maintains a lock on the depgraph
    pub fn write_delta(&self) -> MappedRwLockWriteGuard<'_, DepGraphDelta> {
        MappedRwLockWriteGuard::map(self.write_init(), |g| g.delta_mut())
    }

    /// Locks the depgraph for reading.
    ///
    /// The mode is only used on the first call, to establish some global state, and
    /// then ignored for future calls.
    ///
    /// # Safety
    ///
    /// The pointer to the dependency graph mode should still be pointing
    /// to a valid OCaml object.
    pub fn read(
        &self,
        mode: RawTypingDepsMode,
    ) -> Result<MappedRwLockReadGuard<'_, DepGraphWithDelta<DepGraph>>, DepGraphLoadError> {
        self.load(mode)?;
        Ok(self.read_init())
    }

    /// Locks the depgraph for writing.
    ///
    /// The mode is only used on the first call, to establish some global state, and
    /// then ignored for future calls.
    ///
    /// # Safety
    ///
    /// The pointer to the dependency graph mode should still be pointing
    /// to a valid OCaml object.
    pub fn write(
        &self,
        mode: RawTypingDepsMode,
    ) -> Result<MappedRwLockWriteGuard<'_, DepGraphWithDelta<DepGraph>>, DepGraphLoadError> {
        self.load(mode)?;
        Ok(self.write_init())
    }

    /// Return the already-loaded graph without attempting any I/O.
    pub fn read_loaded(&self) -> Option<MappedRwLockReadGuard<'_, DepGraphWithDelta<DepGraph>>> {
        RwLockReadGuard::try_map(self.0.read(), |cell| {
            cell.get().filter(|graph| graph.is_base_loaded())
        })
        .ok()
    }

    /// Load the graph using the given mode.
    ///
    /// The mode is only used on the first call, to establish some global state, and
    /// then ignored for future calls.
    ///
    /// # Safety
    ///
    /// The pointer to the dependency graph mode should still be pointing
    /// to a valid OCaml object.
    pub fn load(&self, mode: RawTypingDepsMode) -> Result<(), DepGraphLoadError> {
        self.load_mode(Self::mode_from_raw(mode)?)
    }

    /// Replace the loaded graph using the given mode without first attempting
    /// to load the previous graph.
    pub fn replace(&self, mode: RawTypingDepsMode) -> Result<(), DepGraphLoadError> {
        self.replace_mode(Self::mode_from_raw(mode)?)
    }

    fn load_mode(&self, mode: TypingDepsMode) -> Result<(), DepGraphLoadError> {
        self.write_init().load_base(mode)
    }

    fn replace_mode(&self, mode: TypingDepsMode) -> Result<(), DepGraphLoadError> {
        self.write_init().replace_dep_graph(mode)
    }
}

/// Inserts `dependents` into `acc`, stopping once `acc` holds more than `max`
/// distinct deps.
fn insert_until_over(
    acc: &mut HashTrieSet<Dep>,
    dependents: &mut dyn Iterator<Item = Dep>,
    max: usize,
) {
    for dependent in dependents {
        acc.insert_mut(dependent);
        if acc.size() > max {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::collections::HashSet;
    use std::ops::RangeInclusive;
    use std::path::Path;
    use std::path::PathBuf;
    use std::time::SystemTime;
    use std::time::UNIX_EPOCH;

    use depgraph_reader::compress::UncompressedHeader;
    use itertools::Itertools;

    use super::*;

    fn fresh_depgraph_path() -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("current time should be after the Unix epoch")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "hack_missing_lazy_depgraph_{}_{}",
            std::process::id(),
            unique
        ))
    }

    fn write_empty_depgraph(path: &Path) {
        let mut bytes = Vec::with_capacity(std::mem::size_of::<UncompressedHeader>());
        bytes.extend_from_slice(&UncompressedHeader::MAGIC);
        bytes.extend_from_slice(&UncompressedHeader::LATEST_VERSION.to_ne_bytes());
        bytes.extend_from_slice(&0_u64.to_ne_bytes());
        bytes.push(0);
        bytes.extend_from_slice(&[0; 7]);
        assert_eq!(bytes.len(), std::mem::size_of::<UncompressedHeader>());
        std::fs::write(path, bytes).expect("should write an empty dependency graph");
    }

    #[test]
    fn test_missing_lazy_depgraph_is_recoverable() {
        let path = fresh_depgraph_path();
        let path_string = path.to_string_lossy().into_owned();
        write_empty_depgraph(&path);
        std::fs::remove_file(&path).expect("should delete the dependency graph before loading");

        let graph = LockedDepgraphWithDelta::new();
        let error = graph
            .load_mode(TypingDepsMode::InMemoryMode(Some(path_string.clone())))
            .expect_err("loading a deleted dependency graph should fail");
        assert_eq!(
            error,
            DepGraphLoadError::DepgraphNotFound(path_string.clone())
        );
        assert!(graph.read_loaded().is_none());

        write_empty_depgraph(&path);
        graph
            .load_mode(TypingDepsMode::InMemoryMode(Some(path_string)))
            .expect("retrying after recreating the dependency graph should succeed");
        std::fs::remove_file(&path).expect("should delete the dependency graph after loading");

        let loaded_graph = graph
            .read_loaded()
            .expect("the memory-mapped dependency graph should remain loaded after unlink");
        assert!(!loaded_graph.has_edge(Dep::new(1), Dep::new(2)));
    }

    pub struct SimpleDepGraph {
        graph: HashMap<Dep, HashSet<Dep>>,
    }

    impl<const N: usize> From<[(Dep, HashSet<Dep>); N]> for SimpleDepGraph {
        fn from(arr: [(Dep, HashSet<Dep>); N]) -> Self {
            Self {
                graph: arr.into_iter().collect(),
            }
        }
    }

    impl BaseDepgraphTrait for SimpleDepGraph {
        fn iter_dependents(&self, dep: Dep) -> Box<dyn Iterator<Item = Dep> + '_> {
            Box::new(
                self.graph
                    .get(&dep)
                    .map_or(Either::Left(std::iter::empty()), |set| {
                        Either::Right(set.iter().copied())
                    }),
            )
        }

        fn dependent_dependency_edge_exists(&self, dependent: Dep, dependency: Dep) -> bool {
            self.graph
                .get(&dependency)
                .is_some_and(|set| set.contains(&dependent))
        }
    }

    #[test]
    fn test_some_base() {
        let base = SimpleDepGraph::from([
            (Dep::new(0), HashSet::from([Dep::new(1), Dep::new(2)])),
            (Dep::new(3), HashSet::from([Dep::new(2)])),
        ]);
        let mut delta = DepGraphDelta::default();
        delta.insert(Dep::new(4), Dep::new(3));
        delta.insert(Dep::new(3), Dep::new(0));
        delta.insert(Dep::new(4), Dep::new(0));
        delta.remove(Dep::new(2), Dep::new(0));
        delta.remove(Dep::new(4), Dep::new(0));
        let dg = DepGraphWithDelta::new(Some(base), delta);

        assert_eq!(
            dg.iter_dependents_with_duplicates(Dep::new(0), |iter| iter.collect::<HashSet<_>>()),
            HashSet::from([Dep::new(1), Dep::new(3)])
        );
        assert_eq!(
            dg.iter_dependents_with_duplicates(Dep::new(3), |iter| iter.collect::<HashSet<_>>()),
            HashSet::from([Dep::new(2), Dep::new(4)])
        );
        assert!(dg.has_edge(Dep::new(1), Dep::new(0)));
        assert!(!dg.has_edge(Dep::new(2), Dep::new(0)));
        assert!(dg.has_edge(Dep::new(2), Dep::new(3)));
        assert!(dg.has_edge(Dep::new(4), Dep::new(3)));
        assert!(dg.has_edge(Dep::new(3), Dep::new(0)));
        assert!(!dg.has_edge(Dep::new(4), Dep::new(0)));
        assert!(!dg.has_edge(Dep::new(10), Dep::new(11)));
    }

    #[test]
    fn test_no_base() {
        let mut delta = DepGraphDelta::default();
        delta.insert(Dep::new(4), Dep::new(3));
        delta.insert(Dep::new(3), Dep::new(0));
        delta.remove(Dep::new(2), Dep::new(0));
        let dg = DepGraphWithDelta::<SimpleDepGraph>::new(None, delta);

        assert_eq!(
            dg.iter_dependents_with_duplicates(Dep::new(0), |iter| iter.collect::<HashSet<_>>()),
            HashSet::from([Dep::new(3)])
        );
        assert_eq!(
            dg.iter_dependents_with_duplicates(Dep::new(3), |iter| iter.collect::<HashSet<_>>()),
            HashSet::from([Dep::new(4)])
        );
        assert!(dg.has_edge(Dep::new(4), Dep::new(3)));
        assert!(dg.has_edge(Dep::new(3), Dep::new(0)));
        assert!(!dg.has_edge(Dep::new(2), Dep::new(0)));
        assert!(!dg.has_edge(Dep::new(2), Dep::new(3)));
    }

    // The graph for the bounded queries. `WIDE` is the dep with the fan-in the
    // tests bound. The delta overlaps and trims its base, so that every way of
    // counting its dependents gives a different number: items yielded, distinct
    // dependents, base entries, and base entries left after the removal.
    // `NARROW` is a second dep for the whole-set query to move on to.
    const WIDE: u64 = 0;
    const NARROW: u64 = 1;
    /// A dep nothing depends on.
    const UNREFERENCED: u64 = 77;
    const WIDE_BASE: RangeInclusive<u64> = 103..=109;
    /// Overlaps the low end of `WIDE_BASE`, so the dependents in both are
    /// yielded twice, and early in the base's turn rather than last.
    const WIDE_ADDED: RangeInclusive<u64> = 100..=104;
    /// A dependent in `WIDE_BASE` that the delta removes.
    const WIDE_REMOVED: u64 = 109;
    const NARROW_BASE: RangeInclusive<u64> = 200..=203;

    fn graph_with_fan_in() -> DepGraphWithDelta<SimpleDepGraph> {
        let base = SimpleDepGraph::from([
            (Dep::new(WIDE), WIDE_BASE.map(Dep::new).collect()),
            (Dep::new(NARROW), NARROW_BASE.map(Dep::new).collect()),
        ]);
        let mut delta = DepGraphDelta::default();
        WIDE_ADDED.for_each(|dependent| delta.insert(Dep::new(dependent), Dep::new(WIDE)));
        delta.remove(Dep::new(WIDE_REMOVED), Dep::new(WIDE));
        DepGraphWithDelta::new(Some(base), delta)
    }

    /// What `WIDE` should have as dependents, worked out from the constants
    /// and not by asking the graph.
    fn expected_wide_dependents() -> HashSet<Dep> {
        WIDE_BASE
            .chain(WIDE_ADDED)
            .filter(|dependent| *dependent != WIDE_REMOVED)
            .map(Dep::new)
            .collect()
    }

    fn dependents_of(dg: &DepGraphWithDelta<SimpleDepGraph>, dep: Dep) -> HashSet<Dep> {
        dg.iter_dependents_with_duplicates(dep, |iter| iter.collect::<HashSet<_>>())
    }

    #[test]
    fn test_graph_with_fan_in_has_duplicates_and_a_removed_edge() {
        let dg = graph_with_fan_in();
        let yielded = dg.iter_dependents_with_duplicates(Dep::new(WIDE), |iter| iter.count());
        let distinct = dependents_of(&dg, Dep::new(WIDE));

        assert_eq!(
            distinct,
            expected_wide_dependents(),
            "the graph should hold exactly what the constants describe"
        );
        assert!(
            yielded > distinct.len(),
            "the other tests only mean something if some dependents are yielded twice, got {yielded} yielded and {} distinct",
            distinct.len()
        );
        assert!(
            !distinct.contains(&Dep::new(WIDE_REMOVED)),
            "the removed edge must not be a dependent"
        );
    }

    fn to_set(set: &HashTrieSet<Dep>) -> HashSet<Dep> {
        set.iter().copied().collect()
    }

    #[test]
    fn test_get_dependents_bounded_is_complete_within_bound() {
        let dg = graph_with_fan_in();
        let complete = dependents_of(&dg, Dep::new(WIDE));
        let size = complete.len();

        // Counting the duplicates as they are yielded would stop one short
        // when `max` equals `size`.
        (size..=size + 3).for_each(|max| {
            assert_eq!(
                to_set(&dg.get_dependents_bounded(Dep::new(WIDE), max)),
                complete,
                "max {max} is not below the {size} distinct dependents, so the answer must be complete"
            );
        });
        assert!(
            dg.get_dependents_bounded(Dep::new(UNREFERENCED), 0)
                .is_empty(),
            "a dep nothing depends on has no dependents, whatever the bound"
        );
    }

    #[test]
    fn test_get_dependents_bounded_exceeds_bound_when_truncated() {
        let dg = graph_with_fan_in();
        let complete = dependents_of(&dg, Dep::new(WIDE));

        (0..complete.len()).for_each(|max| {
            let bounded = to_set(&dg.get_dependents_bounded(Dep::new(WIDE), max));
            assert!(
                bounded.is_subset(&complete),
                "max {max}: a truncated answer only holds real dependents, never the removed edge"
            );
            assert!(
                bounded.len() > max,
                "max {max}: a truncated answer must be strictly larger than the bound, got {}",
                bounded.len()
            );
        });
    }

    #[test]
    fn test_add_dependents_bounded() {
        let dg = graph_with_fan_in();
        let query: HashTrieSet<Dep> = [WIDE, NARROW].into_iter().map(Dep::new).collect();
        let query_set = to_set(&query);
        let complete: HashSet<Dep> = query_set
            .iter()
            .flat_map(|dep| dependents_of(&dg, *dep))
            .chain(query_set.iter().copied())
            .collect();
        assert_eq!(
            complete.len(),
            query_set.len() + expected_wide_dependents().len() + NARROW_BASE.count(),
            "the query, the dependents of `WIDE` and the dependents of `NARROW`"
        );

        (0..=complete.len() + 2).for_each(|max| {
            let mut acc = query.clone();
            dg.add_dependents_bounded(&query, &mut acc, max);
            let got = to_set(&acc);

            assert!(
                query_set.is_subset(&got),
                "max {max}: the query is part of the answer"
            );
            assert!(
                got.is_subset(&complete),
                "max {max}: only real dependents were added"
            );
            if max >= complete.len() {
                assert_eq!(got, complete, "max {max}: within the bound, so complete");
            } else {
                assert!(
                    got.len() > max,
                    "max {max}: truncated, so strictly larger than the bound, got {}",
                    got.len()
                );
            }
            if max < query_set.len() {
                assert_eq!(
                    got, query_set,
                    "max {max}: the query alone already passes the bound, so no dependent is read"
                );
            }
        });
    }

    #[test]
    fn test_extends_deps() {
        fn class_dep(n: u64) -> Dep {
            Dep::new((n << 1) + 1)
        }
        let class_a = class_dep(0);
        let class_b = class_dep(1);
        let class_c = class_dep(2);
        let class_d = class_dep(3);
        let class_e = class_dep(4);
        let class_f = class_dep(5);
        let class_k = class_dep(10);
        let class_l = class_dep(11);
        let class_m = class_dep(12);

        let dg = DepGraphWithDelta::new(
            Some(SimpleDepGraph::from([
                // 'Extends' edges
                (
                    class_a.class_to_extends().unwrap(),
                    HashSet::from([class_b, class_c]),
                ),
                (
                    class_b.class_to_extends().unwrap(),
                    HashSet::from([class_d]),
                ),
                (
                    class_c.class_to_extends().unwrap(),
                    HashSet::from([class_e, class_f]),
                ),
                // Regular edges
                (class_a, HashSet::from([class_k])),
                (class_c, HashSet::from([class_l, class_m])),
            ])),
            DepGraphDelta::default(),
        );
        let mut visited = hash::HashSet::<Dep>::default();
        let mut acc = HashTrieSet::new();
        dg.get_extend_deps(&mut visited, class_a, &mut acc);
        assert_eq!(
            HashSet::from_iter(acc.iter().copied()),
            HashSet::from([class_b, class_c, class_d, class_e, class_f])
        );
        assert_eq!(
            HashSet::from_iter(visited.iter().copied()),
            HashSet::from([class_a, class_b, class_c, class_d, class_e, class_f])
        );
    }

    #[test]
    fn test_add_extends_deps() {
        fn class_dep(n: u64) -> Dep {
            Dep::new((n << 1) + 1)
        }
        let class_a = class_dep(0);
        let class_b = class_dep(1);
        let class_c = class_dep(2);
        let class_d = class_dep(3);
        let class_e = class_dep(4);
        let class_f = class_dep(5);
        let class_g = class_dep(6);
        let class_h = class_dep(7);
        let class_i = class_dep(8);
        let class_j = class_dep(9);
        let class_k = class_dep(10);
        let class_l = class_dep(11);
        let class_m = class_dep(12);
        let class_n = class_dep(13);
        let class_o = class_dep(14);
        let class_p = class_dep(15);
        let class_q = class_dep(16);
        let class_r = class_dep(17);

        let dg = DepGraphWithDelta::new(
            Some(SimpleDepGraph::from([
                // 'Extends' edges
                (
                    class_a.class_to_extends().unwrap(),
                    HashSet::from([class_b, class_c]),
                ),
                (
                    class_b.class_to_extends().unwrap(),
                    HashSet::from([class_d]),
                ),
                (
                    class_c.class_to_extends().unwrap(),
                    HashSet::from([class_e, class_f]),
                ),
                (
                    class_f.class_to_extends().unwrap(),
                    HashSet::from([class_g, class_h]),
                ),
                (
                    class_i.class_to_extends().unwrap(),
                    HashSet::from([class_j, class_f]),
                ),
                (
                    class_p.class_to_extends().unwrap(),
                    HashSet::from([class_q, class_r]),
                ),
                (
                    class_r.class_to_extends().unwrap(),
                    HashSet::from([class_c, class_i]),
                ),
                // Regular edges
                (class_a, HashSet::from([class_k])),
                (class_c, HashSet::from([class_l, class_m])),
                (class_i, HashSet::from([class_n])),
                (class_j, HashSet::from([class_o])),
            ])),
            DepGraphDelta::default(),
        );
        let mut acc = HashTrieSet::new().insert(class_a).insert(class_i);
        dg.add_extend_deps(&mut acc);
        assert_eq!(
            HashSet::from_iter(acc.iter().copied()),
            HashSet::from([
                class_a, class_b, class_c, class_d, class_e, class_f, class_g, class_h, class_i,
                class_j
            ])
        );
    }

    #[test]
    fn test_member_fanout() {
        fn class_dep(n: u64) -> Dep {
            Dep::new((n << 1) + 1)
        }
        let class_a = class_dep(0);
        let class_b = class_dep(1);
        let class_c = class_dep(2);
        let class_d = class_dep(3);
        let class_e = class_dep(4);
        let class_f = class_dep(5);
        let class_g = class_dep(6);
        let class_h = class_dep(7);
        let class_i = class_dep(8);
        let class_j = class_dep(9);
        let a_m = class_a.member(DepType::KMethod, "m");
        let b_m = class_b.member(DepType::KMethod, "m");
        let c_m = class_c.member(DepType::KMethod, "m");
        let d_m = class_d.member(DepType::KMethod, "m");
        let e_m = class_e.member(DepType::KMethod, "m");
        let f_m = class_f.member(DepType::KMethod, "m");
        let g_m = class_g.member(DepType::KMethod, "m");
        let h_m = class_h.member(DepType::KMethod, "m");
        let d0 = Dep::new(100);
        let d1 = Dep::new(101);
        let d2 = Dep::new(102);
        let d3 = Dep::new(103);
        let d4 = Dep::new(104);
        let d5 = Dep::new(105);
        let d6 = Dep::new(106);
        let d7 = Dep::new(107);
        let d8 = Dep::new(108);
        let d9 = Dep::new(109);
        let d10 = Dep::new(110);
        let d11 = Dep::new(111);
        let d12 = Dep::new(112);
        let d13 = Dep::new(113);
        let d14 = Dep::new(114);
        let d15 = Dep::new(115);
        let d16 = Dep::new(116);
        let d17 = Dep::new(117);
        let d18 = Dep::new(118);
        let d19 = Dep::new(119);
        let d20 = Dep::new(120);

        let dg = DepGraphWithDelta::new(
            Some(SimpleDepGraph::from([
                // 'Extends' edges
                (
                    class_a.class_to_extends().unwrap(),
                    HashSet::from([class_b]),
                ),
                (
                    class_b.class_to_extends().unwrap(),
                    HashSet::from([class_c, class_f, class_j]),
                ),
                (
                    class_c.class_to_extends().unwrap(),
                    HashSet::from([class_d, class_g, class_i]),
                ),
                (
                    class_d.class_to_extends().unwrap(),
                    HashSet::from([class_e]),
                ),
                (
                    class_g.class_to_extends().unwrap(),
                    HashSet::from([class_h]),
                ),
                (
                    class_i.class_to_extends().unwrap(),
                    HashSet::from([class_f]), // makes the graph a DAG instead of just a tree
                ),
                // Let's introduce a cycle
                (
                    class_j.class_to_extends().unwrap(),
                    HashSet::from([class_b]),
                ),
                // member deps
                (a_m, HashSet::from([d0])),
                (b_m, HashSet::from([Dep::declares(), d1])),
                (c_m, HashSet::from([d2])),
                (d_m, HashSet::from([d3, Dep::declares(), d8])),
                (e_m, HashSet::from([d4, d10])),
                (f_m, HashSet::from([d5, Dep::declares()])),
                (g_m, HashSet::from([d6, d11])),
                (h_m, HashSet::from([d7, Dep::declares(), d9])),
                // I::m and J::m does not appear in the graph
                // Other class deps
                (class_a, HashSet::from([d12])),
                (class_b, HashSet::from([d13])),
                (class_c, HashSet::from([d14])),
                (class_d, HashSet::from([d15])),
                (class_e, HashSet::from([d16])),
                (class_f, HashSet::from([d17])),
                (class_g, HashSet::from([d18])),
                (class_h, HashSet::from([d19])),
                (class_i, HashSet::from([d20])),
            ])),
            DepGraphDelta::default(),
        );
        let mut fanout = HashTrieSet::new();
        dg.get_member_fanout(class_b, DepType::KMethod, "m", &mut fanout);
        assert_eq!(
            fanout.into_iter().copied().sorted().collect::<Vec<_>>(),
            [
                class_b, class_c, class_d, class_f, class_g, class_h, class_i, class_j, d1, d2, d6,
                d11
            ]
            .into_iter()
            .sorted()
            .collect::<Vec<_>>()
        );
    }
}
