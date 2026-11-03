// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::fmt;
use std::fs::File;
use std::io::BufRead;
use std::io::BufReader;
use std::path::Path;

use depgraph_reader::Dep;
use hash::DashMap;
use typing_deps_hash::DepType;

/// A map of `Dep`s to a human readable names.
/// E.g. 9f886cce32ff8ed -> Type Child A
#[derive(Default)]
pub struct HumanReadableDepMap {
    map: DashMap<Dep, (DepType, String)>,
}

impl HumanReadableDepMap {
    pub fn contains(&self, dep: Dep) -> bool {
        self.map.contains_key(&dep)
    }

    /// Parse a text file where each line has the format:
    /// DEP_HASH DEP_TYPE SYMBOL_NAME
    /// and stores the entry. Returns a list of collisions if the same hash is
    /// loaded with a different symbol or kind.
    ///
    /// The contents of a file might look like this:
    ///
    /// 718470156085360007 Type ChildB
    /// 1154245141631872205 Type Base
    /// 8150603439003883592 Constructor Base
    /// 1154245141631872204 Extends Base
    /// 718472355108616429 Type ChildA
    pub fn load(&self, path: &Path) -> anyhow::Result<Vec<DepMapCollision>> {
        let mut collisions = vec![];
        for line in BufReader::new(File::open(path)?).lines() {
            let line = line?;
            let mut parts = line.split(' ');
            let hash: u64 = match parts.next() {
                Some(s) => s.parse()?,
                None => anyhow::bail!("expected hash"),
            };
            let kind: DepType = match parts.next() {
                // Also accept dependency kind names without the leading K.
                Some(s) => s.parse().or_else(|_| format!("K{s}").parse())?,
                None => anyhow::bail!("expected DepType"),
            };
            let sym: &str = match parts.next() {
                Some(s) => s,
                None => anyhow::bail!("expected symbol"),
            };
            match self.map.insert(Dep::new(hash), (kind, sym.into())) {
                Some((old_kind, old_sym)) if old_kind != kind || old_sym != sym => {
                    collisions.push(DepMapCollision {
                        hash,
                        old_kind,
                        old_symbol: old_sym.to_owned(),
                        new_kind: kind,
                        new_symbol: sym.to_owned(),
                    });
                }
                _ => {}
            }
        }
        Ok(collisions)
    }

    /// Get the human readable name of a `Dep`. If the human readable name isn't
    /// found, simply return the `Dep` hash.
    pub fn fmt(&self, dep: Dep) -> String {
        match self.map.get(&dep) {
            Some(e) => {
                let (kind, sym) = &*e;
                format!("{kind:?} {sym}")
            }
            None => {
                format!("{dep:016x} ({dep})")
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DepMapCollision {
    pub hash: u64,
    pub old_kind: DepType,
    pub old_symbol: String,
    pub new_kind: DepType,
    pub new_symbol: String,
}

impl fmt::Display for DepMapCollision {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "(Hash Collision {}: ({:?},{}) != ({:?},{}))",
            self.hash, self.old_kind, self.old_symbol, self.new_kind, self.new_symbol,
        )
    }
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use tempfile::NamedTempFile;

    use super::*;

    #[test]
    fn test_load_and_collisions() -> anyhow::Result<()> {
        let dep_map = HumanReadableDepMap::default();
        let mut f1 = NamedTempFile::new()?;
        let mut f2 = NamedTempFile::new()?;
        writeln!(f1, "123456 Type Foo")?;
        writeln!(f2, "123456 KConstructor Base")?;

        assert_eq!(dep_map.load(f1.path())?, vec![]);
        assert_eq!(
            dep_map.load(f2.path())?,
            vec![DepMapCollision {
                hash: 123456u64,
                old_kind: DepType::KType,
                old_symbol: "Foo".to_owned(),
                new_kind: DepType::KConstructor,
                new_symbol: "Base".to_owned(),
            }],
        );
        Ok(())
    }

    #[test]
    fn test_load_kind_names() -> anyhow::Result<()> {
        let dep_map = HumanReadableDepMap::default();
        let mut file = NamedTempFile::new()?;
        writeln!(file, "1 Type Foo")?;
        writeln!(file, "1 KType Foo")?;
        assert!(dep_map.load(file.path())?.is_empty());

        writeln!(file, "2 Unknown Foo")?;
        assert!(dep_map.load(file.path()).is_err());
        Ok(())
    }
}
