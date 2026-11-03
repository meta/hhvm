// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

#![cfg_attr(test, feature(test))]

use std::cell::RefCell;
use std::ffi::OsString;
use std::panic::catch_unwind;

use dep::Dep;
use deps_rust::DEP_GRAPH;
use deps_rust::DepGraphLoadError;
use deps_rust::DepSet;
use deps_rust::RawTypingDepsMode;
use deps_rust::VisitedSet;
use hash::HashSet;
use ocamlrep::ToOcamlRep;
use ocamlrep::Value;
use ocamlrep_custom::CamlSerialize;
use ocamlrep_custom::Custom;
use ocamlrep_ocamlpool::ocaml_ffi;
use rpds::HashTrieSet;
use typing_deps_hash::DepType;
use typing_deps_hash::hash1;
use typing_deps_hash::hash2;

fn tag_to_dep_type(tag: u8) -> DepType {
    match DepType::from_u8(tag) {
        Some(dep_type) => dep_type,
        None => panic!("Invalid dep type: {:?}", tag),
    }
}

/// Hashes an `int` and `string`, arising from one of the one-argument cases of
/// `Typing_deps.Dep.variant`.
///
/// # Safety
///
/// `name1` must point to a valid OCaml string. It must not be concurrently
/// modified while this function holds a reference to it.
///
/// This function is only called from OCaml, and it is passed a value allocated
/// entirely by OCaml code, so the argument will be a valid string. The OCaml
/// runtime is interrupted by the FFI call to this function, none of the
/// transitively called functions from here call into the OCaml runtime, and we
/// do not spawn threads in our OCaml code, so the pointed-to value will not be
/// concurrently modified.
#[unsafe(no_mangle)]
unsafe extern "C" fn hash1_ocaml(dep_type_tag: usize, name1: usize) -> usize {
    unsafe {
        fn do_hash(dep_type_tag: Value<'_>, name1: Value<'_>) -> Value<'static> {
            let dep_type_tag = dep_type_tag
                .as_int()
                .expect("dep_type_tag could not be converted to int");
            let dep_type = tag_to_dep_type(dep_type_tag as u8);
            let name1 = name1
                .as_byte_string()
                .expect("name1 could not be converted to byte string");

            let result: u64 = hash1(dep_type, name1);

            // In Rust, a numeric cast between two integers of the same size
            // is a no-op. We require a 64-bit word size.
            let result = result as isize;

            Value::int(result)
        }

        let dep_type_tag = Value::from_bits(dep_type_tag);
        let name1 = Value::from_bits(name1);
        let result = do_hash(dep_type_tag, name1);
        Value::to_bits(result)
    }
}

/// Hashes an `int` and two `string`s, arising from one of the two-argument cases of
/// `Typing_deps.Dep.variant`.
///
/// # Safety
///
/// `type_hash` must be a valid OCaml int.
/// `member_name` must point to a valid OCaml string. It must not be concurrently
/// modified while this function holds a reference to it.
///
/// This function is only called from OCaml, and it is passed values allocated
/// entirely by OCaml code, so the argument will be a valid string. The OCaml
/// runtime is interrupted by the FFI call to this function, none of the
/// transitively called functions from here call into the OCaml runtime, and we
/// do not spawn threads in our OCaml code, so the pointed-to value will not be
/// concurrently modified.
#[unsafe(no_mangle)]
unsafe extern "C" fn hash2_ocaml(
    dep_type_tag: usize,
    type_hash: usize,
    member_name: usize,
) -> usize {
    unsafe {
        fn do_hash(
            dep_type_tag: Value<'_>,
            type_hash: Value<'_>,
            member_name: Value<'_>,
        ) -> Value<'static> {
            let dep_type_tag = dep_type_tag
                .as_int()
                .expect("dep_type_tag could not be converted to int");
            let dep_type = tag_to_dep_type(dep_type_tag as u8);

            // Ocaml ints are i63, signed extended to i64. clear the MSB while
            // converting to u64, to match FromOcamlRep for Dep.
            let type_hash = type_hash
                .as_int()
                .expect("type_hash could not be converted to int");
            let type_hash = type_hash as u64 & !(1 << 63);

            let member_name = member_name
                .as_byte_string()
                .expect("member_name could not be converted to byte string");

            let result: u64 = hash2(dep_type, type_hash, member_name);

            // In Rust, a numeric cast between two integers of the same size
            // is a no-op. We require a 64-bit word size.
            let result = result as isize;

            Value::int(result)
        }

        let dep_type_tag = Value::from_bits(dep_type_tag);
        let type_hash = Value::from_bits(type_hash);
        let member_name = Value::from_bits(member_name);
        let result = do_hash(dep_type_tag, type_hash, member_name);
        Value::to_bits(result)
    }
}

// Functions to register custom Rust types with the OCaml runtime
ocaml_ffi! {
    fn hh_custom_dep_graph_register_custom_types() {
        // Safety: The OCaml runtime is currently interrupted by a call into
        // this function, so it's safe to interact with it.
        unsafe {
            DepSet::register();
            VisitedSet::register();
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, ToOcamlRep)]
enum HasEdgeStatus {
    Absent,
    Present,
    NotLoaded,
    InternalError,
}

// Functions to query the dependency graph
ocaml_ffi! {
    fn hh_custom_dep_graph_load(mode: RawTypingDepsMode) -> Result<(), DepGraphLoadError> {
        DEP_GRAPH.load(mode)
    }

    fn hh_custom_dep_graph_replace(mode: RawTypingDepsMode) -> Result<(), DepGraphLoadError> {
        DEP_GRAPH.replace(mode)
    }

    fn hh_depgraph_has_edge(dependent: Dep, dependency: Dep) -> HasEdgeStatus {
        match catch_unwind(|| {
            let Some(graph) = DEP_GRAPH.read_loaded() else {
                return HasEdgeStatus::NotLoaded;
            };
            if graph.has_edge(dependent, dependency) {
                HasEdgeStatus::Present
            } else {
                HasEdgeStatus::Absent
            }
        }) {
            Ok(status) => status,
            Err(_) => HasEdgeStatus::InternalError,
        }
    }

    fn hh_custom_dep_graph_get_ideps_from_hash(mode: RawTypingDepsMode, dep: Dep) -> Result<Custom<DepSet>, DepGraphLoadError> {
        get_ideps_from_hash(mode, dep)
    }

    // Returns the union of the provided dep set and their direct typing dependents.
    fn hh_custom_dep_graph_add_typing_deps(mode: RawTypingDepsMode, query: Custom<DepSet>) -> Result<Custom<DepSet>, DepGraphLoadError> {
        query_and_accumulate_typing_deps(mode, query)
    }

    // As above, but stops once the result passes `max`.
    fn hh_custom_dep_graph_add_typing_deps_bounded(mode: RawTypingDepsMode, query: Custom<DepSet>, max: usize) -> Result<Custom<DepSet>, DepGraphLoadError> {
        query_and_accumulate_typing_deps_bounded(mode, query, max)
    }

    // The direct dependents of `dep`, stopping once there are more than `max`.
    fn hh_custom_dep_graph_get_ideps_from_hash_bounded(mode: RawTypingDepsMode, dep: Dep, max: usize) -> Result<Custom<DepSet>, DepGraphLoadError> {
        get_ideps_from_hash_bounded(mode, dep, max)
    }

    // Returns the union of the provided dep set and their recursive 'extends' dependents.
    fn hh_custom_dep_graph_add_extend_deps(mode: RawTypingDepsMode, query: Custom<DepSet>) -> Result<Custom<DepSet>, DepGraphLoadError> {
        let mut acc = query.clone();
        DEP_GRAPH.read(mode)?.add_extend_deps(&mut acc);
        Ok(Custom::from(acc.into()))
    }

    // Returns the recursive 'extends' dependents of a dep.
    fn hh_custom_dep_graph_get_extend_deps(
        mode: RawTypingDepsMode,
        visited: Custom<VisitedSet>,
        source_class: Dep,
        acc: Custom<DepSet>,
    ) -> Result<Custom<DepSet>, DepGraphLoadError> {
        let mut acc = acc.clone();
        DEP_GRAPH.read(mode)?.get_extend_deps(&mut visited.borrow_mut(), source_class, &mut acc);
        Ok(Custom::from(acc.into()))
    }

    fn hh_get_member_fanout(
        mode: RawTypingDepsMode,
        class_dep: Dep,
        member_type: DepType,
        member_name: String,
        fanout_acc: Custom<DepSet>,
    ) -> Result<Custom<DepSet>, DepGraphLoadError> {
        let mut fanout_acc = fanout_acc.clone();
        DEP_GRAPH.read(mode)?.get_member_fanout(
            class_dep,
            member_type,
            &member_name,
            &mut fanout_acc,
        );
        Ok(Custom::from(fanout_acc.into()))
    }

    fn hh_get_not_subtype_fanout(
        mode: RawTypingDepsMode,
        deps: Custom<DepSet>,
        fanout_acc: Custom<DepSet>,
    ) -> Result<Custom<DepSet>, DepGraphLoadError> {
        let mut fanout_acc = fanout_acc.clone();
        DEP_GRAPH.read(mode)?.get_not_subtype_fanout(
            &deps,
            &mut fanout_acc,
        );
        Ok(Custom::from(fanout_acc.into()))
    }

    // Add edge into the in-memory depgraph delta
    fn hh_custom_dep_graph_register_discovered_dep_edge(
        dependent: Dep,
        dependency: Dep,
    ) {
        DEP_GRAPH.write_delta().insert(dependent, dependency);
    }

    fn hh_custom_dep_graph_remove_edge(dependent: Dep, dependency: Dep) {
        DEP_GRAPH.write_delta().remove(dependent, dependency);
    }

    fn hh_custom_dep_graph_dep_graph_delta_num_edges() -> usize {
        DEP_GRAPH.read_delta().added_edges_count()
    }

    fn hh_custom_dep_graph_save_delta(dest: OsString, reset_state_after_saving: bool) -> usize {
        save_delta(dest, reset_state_after_saving)
    }
}

fn get_ideps_from_hash(
    mode: RawTypingDepsMode,
    dep: Dep,
) -> Result<Custom<DepSet>, DepGraphLoadError> {
    let deps = DEP_GRAPH
        .read(mode)?
        .iter_dependents_with_duplicates(dep, |iter_dep| iter_dep.collect::<HashTrieSet<_>>());
    Ok(Custom::from(DepSet::from(deps)))
}

/// Returns the union of the provided dep set and their direct typing dependents.
fn query_and_accumulate_typing_deps(
    mode: RawTypingDepsMode,
    query: Custom<DepSet>,
) -> Result<Custom<DepSet>, DepGraphLoadError> {
    let mut acc = query.clone();
    let graph = DEP_GRAPH.read(mode)?;
    for dependency in query.iter() {
        graph.iter_dependents_with_duplicates(*dependency, |iter| {
            iter.for_each(|dependent| acc.insert_mut(dependent))
        })
    }
    Ok(Custom::from(DepSet::from(acc)))
}

/// As above, but may stop early once the result is larger than `max`: the
/// complete union, or some partial subset of it that is big enough to show the
/// limit was exceeded.
fn query_and_accumulate_typing_deps_bounded(
    mode: RawTypingDepsMode,
    query: Custom<DepSet>,
    max: usize,
) -> Result<Custom<DepSet>, DepGraphLoadError> {
    let mut acc = query.clone();
    DEP_GRAPH
        .read(mode)?
        .add_dependents_bounded(&query, &mut acc, max);
    Ok(Custom::from(DepSet::from(acc)))
}

/// The direct dependents of one hash, stopping early once there are more than
/// `max`: the complete set, or some partial subset of it that is big enough to
/// show the limit was exceeded.
fn get_ideps_from_hash_bounded(
    mode: RawTypingDepsMode,
    dep: Dep,
    max: usize,
) -> Result<Custom<DepSet>, DepGraphLoadError> {
    let deps = DEP_GRAPH.read(mode)?.get_dependents_bounded(dep, max);
    Ok(Custom::from(DepSet::from(deps)))
}

fn save_delta(dest: OsString, reset_state_after_saving: bool) -> usize {
    let f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dest)
        .unwrap();
    let mut w = std::io::BufWriter::new(f);
    let hashes_added = DEP_GRAPH
        .write_delta()
        .write_added_edges_to(&mut w)
        .unwrap();
    w.into_inner().unwrap();

    if reset_state_after_saving {
        DEP_GRAPH.write_delta().clear();
    }
    hashes_added
}

// Auxiliary functions for Typing_deps.DepSet/Typing_deps.VisitedSet
ocaml_ffi! {
    fn hh_visited_set_make() -> Custom<VisitedSet> {
        Custom::from(RefCell::new(HashSet::default()).into())
    }

    fn hh_dep_set_make() -> Custom<DepSet> {
        Custom::from(HashTrieSet::new().into())
    }

    fn hh_dep_set_singleton(dep: Dep) -> Custom<DepSet> {
        let mut s = HashTrieSet::new();
        s.insert_mut(dep);
        Custom::from(s.into())
    }

    fn hh_dep_set_add(s: Custom<DepSet>, dep: Dep) -> Custom<DepSet> {
        let mut s = s.clone();
        s.insert_mut(dep);
        Custom::from(s.into())
    }

    fn hh_dep_set_union(s1: Custom<DepSet>, s2: Custom<DepSet>) -> Custom<DepSet> {
        Custom::from(s1.union(&s2))
    }

    fn hh_dep_set_inter(s1: Custom<DepSet>, s2: Custom<DepSet>) -> Custom<DepSet> {
        Custom::from(s1.intersect(&s2))
    }

    fn hh_dep_set_diff(s1: Custom<DepSet>, s2: Custom<DepSet>) -> Custom<DepSet> {
        Custom::from(s1.difference(&s2))
    }

    fn hh_dep_set_mem(s: Custom<DepSet>, dep: Dep) -> bool {
        s.contains(&dep)
    }

    fn hh_dep_set_elements(s: Custom<DepSet>) -> Vec<Dep> {
        s.iter().copied().collect()
    }

     fn hh_dep_set_cardinal(s: Custom<DepSet>) -> usize {
         s.size()
     }

     fn hh_dep_set_is_empty(s: Custom<DepSet>) -> bool {
         s.is_empty()
     }

     fn hh_dep_set_of_list(xs: Vec<Dep>) -> Custom<DepSet> {
         Custom::from(HashTrieSet::from_iter(xs).into())
     }
}

#[cfg(test)]
mod tests {
    extern crate test;

    use ocamlrep::Arena;
    use ocamlrep::ToOcamlRep;
    use ocamlrep::Value;
    use test::Bencher;

    const SHORT_CLASS_NAME: &str = "\\Foo";
    const LONG_CLASS_NAME: &str = "\\EntReasonablyLongClassNameSinceSomeClassNamesAreLong";

    #[bench]
    fn bench_hash1_short(b: &mut Bencher) {
        b.iter(|| {
            crate::hash1(crate::DepType::KType, SHORT_CLASS_NAME.as_bytes());
        });
    }

    #[bench]
    fn bench_hash1_long(b: &mut Bencher) {
        b.iter(|| {
            crate::hash1(crate::DepType::KType, LONG_CLASS_NAME.as_bytes());
        });
    }

    #[bench]
    fn bench_hash2_short(b: &mut Bencher) {
        b.iter(|| {
            let type_hash = crate::hash1(crate::DepType::KType, SHORT_CLASS_NAME.as_bytes());
            crate::hash2(crate::DepType::KConst, type_hash, b"\\T");
        });
    }

    #[bench]
    fn bench_hash2_long(b: &mut Bencher) {
        b.iter(|| {
            let type_hash = crate::hash1(crate::DepType::KType, LONG_CLASS_NAME.as_bytes());
            crate::hash2(crate::DepType::KConst, type_hash, b"\\TSomeTypeConstant");
        });
    }

    #[bench]
    fn bench_hash1_ocaml(b: &mut Bencher) {
        let arena = Arena::new();
        let dep_type = crate::DepType::KType;
        let name1 = arena.add(LONG_CLASS_NAME);
        b.iter(|| unsafe {
            crate::hash1_ocaml(Value::int(dep_type as isize).to_bits(), name1.to_bits())
        });
    }

    #[bench]
    fn bench_hash2_ocaml(b: &mut Bencher) {
        let arena = Arena::new();
        let dep_type = crate::DepType::KConst;
        let type_hash = crate::hash1(crate::DepType::KType, LONG_CLASS_NAME.as_bytes());
        let member_name = arena.add("\\TSomeTypeConstant");
        b.iter(|| unsafe {
            crate::hash2_ocaml(
                Value::int(dep_type as isize).to_bits(),
                type_hash.to_ocamlrep(&arena).to_bits(),
                member_name.to_bits(),
            )
        });
    }
}
