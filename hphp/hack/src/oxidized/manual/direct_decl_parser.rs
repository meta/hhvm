// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

pub use shallow_decl_defs::Decl;

pub use crate::direct_decl_parser_types::Decls;
pub use crate::direct_decl_parser_types::ParsedFile;
use crate::file_info;
use crate::file_info::NameType;
use crate::shallow_decl_defs;
use crate::typing_defs;

/// This is a store of decls. It allows iteration of decls in forward lexical order,
/// and you can .rev() to iterate in reverse lexical order.
/// By construction, you can safely trust that whoever constructed this object has explicitly
/// passed `deregister_php_stdlib`, which we hope and trust they got from .hhconfig.
#[derive(Clone, Debug)]
pub struct ParsedFileWithHashes {
    pub mode: Option<file_info::Mode>,

    /// `file_decls_hash` is computed before php stdlib decls and duplicates
    /// are removed, as is computed over the decls in reverse lexical order
    /// (the hash is order-sensitive)
    pub file_decls_hash: hh24_types::FileDeclsHash,

    /// Decls and position-sensitive hash. Internally they're stored in reverse
    /// lexical order. The choice of what to go into this list (remove dupes? remove php_stdlib
    /// in hhi files? transform class decls by removing php_stdlib members?) is determined
    /// by how it was constructed. The field is private: the only way to access it
    /// are through accessors .iter() and .into_iter(), which give the illusion of it being
    /// in forward lexical order.
    pub decls: Vec<(String, Decl, hh24_types::DeclHash)>,
}

impl ParsedFileWithHashes {
    pub fn new(
        parsed_file: ParsedFile,
        deregister_php_stdlib_if_hhi: bool,
        prefix: relative_path::Prefix,
    ) -> Self {
        let file_decls_hash = hh24_types::FileDeclsHash::from_u64(
            hh_hash::position_insensitive_hash(&parsed_file.decls),
        );
        // Note: parsed_file.decls is in reverse lexical order, and our self.decls must
        // also be in reverse lexical order, so that as `OcamlReverseParsedFileWithHashes`
        // we ffi correctly to ocaml.
        let decls = parsed_file
            .decls
            .into_iter()
            .filter_map(|(name, mut decl)| {
                if deregister_php_stdlib_if_hhi && prefix == relative_path::Prefix::Hhi {
                    match filter_php_stdlib_decls(decl) {
                        Some(altered_decl) => decl = altered_decl,
                        None => return None,
                    }
                }
                let hash = hh24_types::DeclHash::from_u64(hh_hash::hash(&decl));
                Some((name, decl, hash))
            })
            .collect();
        Self {
            mode: parsed_file.mode,
            file_decls_hash,
            decls,
        }
    }

    /// This method is a code smell. It shows that the caller didn't take into account
    /// the .hhconfig flag `deregister_php_stdlib`.
    pub fn new_without_deregistering_do_not_use(parsed_file: ParsedFile) -> Self {
        let file_decls_hash = hh24_types::FileDeclsHash::from_u64(
            hh_hash::position_insensitive_hash(&parsed_file.decls),
        );
        let decls = parsed_file
            .decls
            .into_iter()
            .map(|(name, decl)| {
                let hash = hh24_types::DeclHash::from_u64(hh_hash::hash(&decl));
                (name, decl, hash)
            })
            .collect();
        Self {
            mode: parsed_file.mode,
            file_decls_hash,
            decls,
        }
    }

    /// This iterates the decls in forward lexical order
    /// (Use iter().rev() if you want reverse order)
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = &(String, Decl, hh24_types::DeclHash)> {
        // Note that our `self.decls` are stored in reverse order, so we have to reverse now.
        self.decls.iter().rev()
    }
}

impl IntoIterator for ParsedFileWithHashes {
    type Item = (String, Decl, hh24_types::DeclHash);
    type IntoIter = std::iter::Rev<std::vec::IntoIter<Self::Item>>;

    /// This iterates the decls in forward lexical order
    /// (Use into_iter().rev() if you want reverse order)
    fn into_iter(self) -> Self::IntoIter {
        // Note that our `self.decls` are stored in reverse order, so we have to reverse now.
        self.decls.into_iter().rev()
    }
}

/// WARNING! These decls do not respect the `deregister_php_stdlib` flag in .hhconfig,
/// and are in reverse declaration order, and have not been de-duped. You should
/// probably be consuming ParsedFileWithHashes instead.
impl Decls {
    pub fn empty() -> Self {
        Self(Vec::new())
    }

    pub fn get(&self, kind: NameType, symbol: &str) -> Option<&Decl> {
        self.iter().find_map(|(name, decl)| {
            if decl.kind() == kind && name == symbol {
                Some(decl)
            } else {
                None
            }
        })
    }

    pub fn add(&mut self, name: String, decl: Decl) {
        self.0.push((name, decl))
    }

    pub fn rev(&mut self) {
        self.0.reverse()
    }

    pub fn iter(&self) -> impl Iterator<Item = &(String, Decl)> {
        self.0.iter()
    }

    pub fn classes(&self) -> impl Iterator<Item = (&str, &shallow_decl_defs::ShallowClass)> {
        self.iter().filter_map(|(name, decl)| match decl {
            Decl::Class(decl) => Some((name.as_str(), decl)),
            Decl::Fun(_) | Decl::Typedef(_) | Decl::Const(_) | Decl::Module(_) => None,
        })
    }
    pub fn funs(&self) -> impl Iterator<Item = (&str, &typing_defs::FunElt)> {
        self.iter().filter_map(|(name, decl)| match decl {
            Decl::Fun(decl) => Some((name.as_str(), decl)),
            Decl::Class(_) | Decl::Typedef(_) | Decl::Const(_) | Decl::Module(_) => None,
        })
    }
    pub fn typedefs(&self) -> impl Iterator<Item = (&str, &typing_defs::TypedefType)> {
        self.iter().filter_map(|(name, decl)| match decl {
            Decl::Typedef(decl) => Some((name.as_str(), decl)),
            Decl::Class(_) | Decl::Fun(_) | Decl::Const(_) | Decl::Module(_) => None,
        })
    }
    pub fn consts(&self) -> impl Iterator<Item = (&str, &typing_defs::ConstDecl)> {
        self.iter().filter_map(|(name, decl)| match decl {
            Decl::Const(decl) => Some((name.as_str(), decl)),
            Decl::Class(_) | Decl::Fun(_) | Decl::Typedef(_) | Decl::Module(_) => None,
        })
    }
    pub fn types(&self) -> impl Iterator<Item = &(String, Decl)> {
        self.iter().filter(|(_, decl)| match decl.kind() {
            NameType::Class | NameType::Typedef => true,
            NameType::Fun | NameType::Const | NameType::Module => false,
        })
    }

    pub fn modules(&self) -> impl Iterator<Item = (&str, &typing_defs::ModuleDefType)> {
        self.iter().filter_map(|(name, decl)| match decl {
            Decl::Module(decl) => Some((name.as_str(), decl)),
            _ => None,
        })
    }
}

// c.f. `hphp/hack/src/providers/direct_decl_utils.ml`
fn filter_php_stdlib_decls(decl: Decl) -> Option<Decl> {
    use crate::method_flags::MethodFlags;
    use crate::prop_flags::PropFlags;
    match decl {
        Decl::Fun(f) if f.php_std_lib => None,
        Decl::Class(c) if (c.user_attributes.iter()).any(|ua| ua.name.1 == "__PHPStdLib") => None,
        Decl::Class(mut c) => {
            (c.props).retain(|p| !p.flags.contains(PropFlags::PHP_STD_LIB));
            (c.sprops).retain(|p| !p.flags.contains(PropFlags::PHP_STD_LIB));
            (c.methods).retain(|m| !m.flags.contains(MethodFlags::PHP_STD_LIB));
            (c.static_methods).retain(|m| !m.flags.contains(MethodFlags::PHP_STD_LIB));
            Some(Decl::Class(c))
        }
        _ => Some(decl),
    }
}

impl IntoIterator for Decls {
    type Item = (String, Decl);
    type IntoIter = std::vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

impl std::fmt::Debug for Decls {
    fn fmt(&self, fmt: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        fmt.debug_map()
            .entries(self.iter().map(|(k, v)| (k, v)))
            .finish()
    }
}
