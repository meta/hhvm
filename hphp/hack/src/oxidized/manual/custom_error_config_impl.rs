// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::fs::File;
use std::io::BufReader;
use std::path::Path;
use std::str::FromStr;

use anyhow::Context;
use anyhow::Result;
use hash::HashMap;
use hash::HashSet;
use serde_json;

use crate::custom_error::CustomError;
use crate::custom_error::VersionedErrorMessage;
use crate::custom_error::VersionedPattError;
use crate::custom_error_config::CustomErrorConfig;
use crate::error_message::Elem;
use crate::error_message::ErrorMessage;
use crate::patt_binding_ty::PattBindingTy;
use crate::patt_error::PattError;
use crate::patt_file::FilePath;
use crate::patt_file::PattFile;
use crate::patt_locl_ty::Params;
use crate::patt_locl_ty::PattLoclTy;
use crate::patt_locl_ty::Prim;
use crate::patt_locl_ty::ShapeField;
use crate::patt_locl_ty::ShapeFields;
use crate::patt_locl_ty::ShapeLabel;
use crate::patt_member_name::PattMemberName;
use crate::patt_name::Namespace;
use crate::patt_name::PattName;
use crate::patt_naming_error::PattNameContext;
use crate::patt_naming_error::PattNamingError;
use crate::patt_string::PattString;
use crate::patt_typing_error::Callback;
use crate::patt_typing_error::PattTypingError;
use crate::patt_typing_error::Primary;
use crate::patt_typing_error::ReasonsCallback;
use crate::patt_typing_error::Secondary;
use crate::validation_err::ValidationErr;

impl CustomErrorConfig {
    pub fn new(mut errors: Vec<CustomError>) -> Self {
        let invalid = errors
            .extract_if(.., |e| {
                let mut env = ValidationEnv::default();
                !e.validate(&mut env)
            })
            .collect();
        Self {
            valid: errors,
            invalid,
        }
    }

    pub fn from_path(path: &Path) -> Result<CustomErrorConfig> {
        if path.exists() {
            let file = File::open(path).with_context(|| path.display().to_string())?;
            let reader = BufReader::new(file);
            let errors = serde_json::from_reader(reader)?;
            Ok(Self::new(errors))
        } else {
            Ok(Self::default())
        }
    }
}

impl Default for CustomErrorConfig {
    fn default() -> Self {
        Self {
            valid: vec![],
            invalid: vec![],
        }
    }
}

impl FromStr for CustomErrorConfig {
    type Err = anyhow::Error;
    fn from_str(contents: &str) -> Result<Self, Self::Err> {
        let errors: Vec<CustomError> = serde_json::from_str(contents)?;
        Ok(Self::new(errors))
    }
}

// -- Trait for types that can be validated ------------------------------------

pub trait Validatable {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool;
}

trait Invalidatable {
    fn invalidate(&mut self, errs: &[ValidationErr]);
}

impl<T: Validatable> Validatable for Option<T> {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Some(t) => t.validate(env),
            Self::None => true,
        }
    }
}

// -- Validation environment ---------------------------------------------------

#[derive(Debug, Default, Clone)]
pub struct ValidationEnv(HashMap<String, PattBindingTy>);

impl ValidationEnv {
    fn add(&mut self, name: &str, ty: PattBindingTy) -> Option<ValidationErr> {
        if self.0.contains_key(name) {
            Some(ValidationErr::Shadowed(name.to_string()))
        } else {
            self.0.insert(name.to_string(), ty);
            None
        }
    }

    fn get(&self, name: &str) -> Option<&PattBindingTy> {
        self.0.get(name)
    }

    fn join(&mut self, other: &Self) -> (Vec<ValidationErr>, Vec<ValidationErr>) {
        let mut errs1 = vec![];
        let mut errs2 = vec![];
        let ks1: HashSet<_> = self.0.keys().cloned().collect();
        let ks2: HashSet<_> = other.0.keys().cloned().collect();
        ks1.union(&ks2)
            .for_each(|k| match (self.0.get(k), other.0.get(k)) {
                (p1, p2) if p1 == p2 => (),
                (Some(l), Some(r)) => errs2.push(ValidationErr::Mismatch(l.clone(), r.clone())),
                (Some(_), _) => errs2.push(ValidationErr::Unbound(k.to_string())),
                (_, Some(b)) => {
                    self.0.insert(k.to_string(), b.clone());
                    errs1.push(ValidationErr::Unbound(k.to_string()))
                }
                _ => panic!("Impossible"),
            });
        (errs1, errs2)
    }
}

// -- Individual errors --------------------------------------------------------

impl Validatable for VersionedErrorMessage {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::MessageV1(msg) => msg.validate(env),
        }
    }
}

impl Validatable for VersionedPattError {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::ErrorV1(patt_typing_error) => patt_typing_error.validate(env),
            Self::ErrorV2(patt_error) => patt_error.validate(env),
        }
    }
}

impl Validatable for CustomError {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        self.patt.validate(env) && self.error_message.validate(env)
    }
}

// -- Error patterns -----------------------------------------------------------
impl Validatable for PattError {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Typing(typing) => typing.validate(env),
            Self::Naming(naming) => naming.validate(env),
        }
    }
}

impl Validatable for PattNamingError {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::InvalidNaming { .. } => false,
            Self::UnboundName { name_context, name } => {
                name_context.validate(env) && name.validate(env)
            }
        }
    }
}

impl Validatable for PattNameContext {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

impl Validatable for PattTypingError {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::InvalidTyping { .. } => false,
            Self::Primary(prim) => prim.validate(env),
            Self::Apply { cb, err } => cb.validate(env) && err.validate(env),
            Self::ApplyReasons { rsns_cb, secondary } => {
                rsns_cb.validate(env) && secondary.validate(env)
            }
            Self::Or { fst, snd } => {
                let mut env_snd = env.clone();
                let fst_valid = fst.validate(env);
                let snd_valid = snd.validate(&mut env_snd);
                // Determine the errors in the left- and right-hand side patterns
                // and add bindings present in the right but not the left side
                // to the environment
                let (errs_fst, errs_snd) = env.join(&env_snd);
                // Wrap the left and right side in an invalid marker based on the
                // join errors
                fst.invalidate(&errs_fst);
                snd.invalidate(&errs_snd);
                fst_valid && snd_valid && errs_fst.is_empty() && errs_snd.is_empty()
            }
        }
    }
}

impl Invalidatable for PattTypingError {
    fn invalidate(&mut self, errs_in: &[ValidationErr]) {
        if !errs_in.is_empty() {
            match self {
                Self::InvalidTyping { errs, .. } => errs.extend(errs_in.to_vec()),
                _ => {
                    let patt = std::mem::replace(self, PattTypingError::Primary(Primary::AnyPrim));
                    *self = PattTypingError::InvalidTyping {
                        errs: errs_in.to_vec(),
                        patt: Box::new(patt),
                    };
                }
            }
        }
    }
}

impl Validatable for Secondary {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::AnySnd => true,
            Self::OfError(err) => err.validate(env),
            Self::ViolatedConstraint {
                cstr,
                ty_sub,
                ty_sup,
            } => cstr.validate(env) && ty_sub.validate(env) && ty_sup.validate(env),
            Self::SubtypingError { sub, sup } => sub.validate(env) && sup.validate(env),
        }
    }
}

impl Validatable for Primary {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::AnyPrim => true,
            Self::MemberNotFound {
                class_name,
                member_name,
                ..
            } => class_name.validate(env) && member_name.validate(env),
            Self::AnyPkg {
                use_file,
                decl_file,
            } => use_file.validate(env) && decl_file.validate(env),
            Self::CrossPkgAccess {
                use_file,
                decl_file,
            } => use_file.validate(env) && decl_file.validate(env),
            Self::CrossPkgAccessWithRequirepackage {
                use_file,
                decl_file,
            } => use_file.validate(env) && decl_file.validate(env),
            Self::CrossPkgAccessWithSoftrequirepackage {
                use_file,
                decl_file,
            } => use_file.validate(env) && decl_file.validate(env),
            Self::SoftIncludedAccess {
                use_file,
                decl_file,
            } => use_file.validate(env) && decl_file.validate(env),
            Self::ExpressionTreeUnsupportedOperator {
                class_name,
                member_name,
            } => class_name.validate(env) && member_name.validate(env),
        }
    }
}

impl Validatable for Callback {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

impl Validatable for ReasonsCallback {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

// -- Type patterns ------------------------------------------------------------

impl Validatable for PattLoclTy {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Invalid(..) => false,
            Self::Dynamic | Self::Nonnull | Self::Any => true,
            Self::Apply { name, params } => name.validate(env) && params.validate(env),
            Self::Prim(prim) => prim.validate(env),
            Self::Shape(fields) => fields.validate(env),
            Self::Option(patt) => patt.validate(env),
            Self::Tuple(patts) => patts.validate(env),
            Self::As { lbl, patt } => {
                let errs = env.add(lbl, PattBindingTy::Ty).map_or(vec![], |e| vec![e]);
                let valid = patt.validate(env);
                self.invalidate(&errs);
                valid && errs.is_empty()
            }
            Self::Or { fst, snd } => {
                let mut env_snd = env.clone();
                let fst_valid = fst.validate(env);
                let snd_valid = snd.validate(&mut env_snd);
                // Determine the errors in the left- and right-hand side patterns
                // and add bindings present in the right but not the left side
                // to the environment
                let (errs_fst, errs_snd) = env.join(&env_snd);
                // Wrap the left and right side in an invalid marker based on the
                // join errors
                fst.invalidate(&errs_fst);
                snd.invalidate(&errs_snd);
                fst_valid && snd_valid && errs_fst.is_empty() && errs_snd.is_empty()
            }
        }
    }
}

impl Invalidatable for PattLoclTy {
    fn invalidate(&mut self, errs_in: &[ValidationErr]) {
        if !errs_in.is_empty() {
            match self {
                Self::Invalid(errs, _patt) => errs.extend(errs_in.to_vec()),
                _ => {
                    let patt = std::mem::replace(self, PattLoclTy::Any);
                    *self = PattLoclTy::Invalid(errs_in.to_vec(), Box::new(patt));
                }
            }
        }
    }
}

impl Validatable for Params {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Nil | Self::Wildcard => true,
            Self::Exists(patt) => patt.validate(env),
            Self::Cons { hd, tl } => hd.validate(env) && tl.validate(env),
        }
    }
}

impl Validatable for Prim {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

impl Validatable for ShapeLabel {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

impl Validatable for ShapeField {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        self.lbl.validate(env) && self.patt.validate(env)
    }
}

impl Validatable for ShapeFields {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Fld { fld, rest } => fld.validate(env) && rest.validate(env),
            Self::Open | Self::Closed => true,
        }
    }
}

// -- Name patterns ------------------------------------------------------------

impl Validatable for PattName {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::As { lbl, patt } => {
                let errs = env
                    .add(lbl, PattBindingTy::Name)
                    .map_or(vec![], |e| vec![e]);
                let valid = patt.validate(env);
                self.invalidate(&errs);
                valid && errs.is_empty()
            }
            Self::Name { namespace, name } => namespace.validate(env) && name.validate(env),
            Self::Invalid { .. } => false,
            Self::Wildcard => true,
        }
    }
}

impl Invalidatable for PattName {
    fn invalidate(&mut self, errs_in: &[ValidationErr]) {
        if !errs_in.is_empty() {
            match self {
                Self::Invalid { errs, .. } => errs.extend(errs_in.to_vec()),
                _ => {
                    let patt = std::mem::replace(self, PattName::Wildcard);
                    *self = PattName::Invalid {
                        errs: errs_in.to_vec(),
                        patt: Box::new(patt),
                    };
                }
            }
        }
    }
}

impl Validatable for Namespace {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

impl Validatable for PattMemberName {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::As { lbl, patt } => {
                let errs = env
                    .add(lbl, PattBindingTy::MemberName)
                    .map_or(vec![], |e| vec![e]);
                let valid = patt.validate(env);
                self.invalidate(&errs);
                valid && errs.is_empty()
            }
            Self::MemberName { patt_string } => patt_string.validate(env),
            Self::Invalid { .. } => false,
            Self::Wildcard => true,
        }
    }
}

impl Invalidatable for PattMemberName {
    fn invalidate(&mut self, errs_in: &[ValidationErr]) {
        if !errs_in.is_empty() {
            match self {
                Self::Invalid { errs, .. } => errs.extend(errs_in.to_vec()),
                _ => {
                    let patt = std::mem::replace(self, PattMemberName::Wildcard);
                    *self = PattMemberName::Invalid {
                        errs: errs_in.to_vec(),
                        patt: Box::new(patt),
                    };
                }
            }
        }
    }
}

// -- File patterns ------------------------------------------------------------

impl Validatable for PattFile {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::As { lbl, patt } => {
                let errs = env
                    .add(lbl, PattBindingTy::File)
                    .map_or(vec![], |e| vec![e]);
                let valid = patt.validate(env);
                self.invalidate(&errs);
                valid && errs.is_empty()
            }
            Self::Name {
                patt_file_path,
                patt_file_name,
                patt_file_extension,
                ..
            } => {
                patt_file_path.validate(env)
                    && patt_file_name.validate(env)
                    && patt_file_extension.validate(env)
            }
            Self::Invalid { .. } => false,
            Self::Wildcard => true,
        }
    }
}

impl Invalidatable for PattFile {
    fn invalidate(&mut self, errs_in: &[ValidationErr]) {
        if !errs_in.is_empty() {
            match self {
                Self::Invalid { errs, .. } => errs.extend(errs_in.to_vec()),
                _ => {
                    let patt = std::mem::replace(self, Self::Wildcard);
                    *self = Self::Invalid {
                        errs: errs_in.to_vec(),
                        patt: Box::new(patt),
                    };
                }
            }
        }
    }
}

impl Validatable for FilePath {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::Dot => true,
            Self::Slash { prefix, segment } => prefix.validate(env) && segment.validate(env),
            Self::SlashOpt { prefix, segment } => prefix.validate(env) && segment.validate(env),
        }
    }
}
// -- String patterns ----------------------------------------------------------

impl Validatable for PattString {
    fn validate(&mut self, _env: &mut ValidationEnv) -> bool {
        true
    }
}

// -- Error messages -----------------------------------------------------------

impl Validatable for ErrorMessage {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        self.message.validate(env)
    }
}

impl Validatable for Elem {
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        match self {
            Self::TyVar(name) => matches!(env.get(name), Some(PattBindingTy::Ty)),
            Self::NameVar(name) => matches!(env.get(name), Some(PattBindingTy::Name)),
            Self::FileVar(name) => matches!(env.get(name), Some(PattBindingTy::File)),
            Self::MemberNameVar(name) => matches!(env.get(name), Some(PattBindingTy::MemberName)),
            Self::Lit(_) => true,
        }
    }
}

// -- Trait helpers ------------------------------------------------------------

impl<T> Validatable for Box<T>
where
    T: Validatable,
{
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        (**self).validate(env)
    }
}

impl<T> Validatable for Vec<T>
where
    T: Validatable,
{
    fn validate(&mut self, env: &mut ValidationEnv) -> bool {
        let mut valid = true;
        for x in self {
            valid &= x.validate(env);
        }
        valid
    }
}

// -- Tests --------------------------------------------------------------------
#[cfg(test)]
mod tests {

    use super::*;
    use crate::patt_typing_error::MemberKindPattern;
    use crate::patt_typing_error::StaticPattern;
    use crate::patt_typing_error::VisibilityPattern;

    #[test]
    fn test_validate_ty_unbound_right() {
        let fst = Box::new(PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        });
        let snd = Box::new(PattLoclTy::Nonnull);
        let mut patt = PattLoclTy::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(!valid);
        assert!(matches!(
            patt,
            PattLoclTy::Or {
                snd: deref!(PattLoclTy::Invalid(..)),
                ..
            }
        ));
    }

    #[test]
    fn test_validate_ty_unbound_left() {
        let snd = Box::new(PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        });
        let fst = Box::new(PattLoclTy::Nonnull);
        let mut patt = PattLoclTy::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(!valid);
        assert!(matches!(
            patt,
            PattLoclTy::Or {
                fst: deref!(PattLoclTy::Invalid(..)),
                ..
            }
        ));
    }

    #[test]
    fn test_validate_ty_shadow() {
        let name = PattName::As {
            lbl: "x".to_string(),
            patt: Box::new(PattName::Name {
                namespace: Namespace::Root,
                name: PattString::Exactly("Classy".to_string()),
            }),
        };
        let hd = PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        };

        let tl = Box::new(Params::Nil);
        let params = Box::new(Params::Cons { hd, tl });
        let mut patt = PattLoclTy::Apply { name, params };
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
        assert!(matches!(
            patt,
            PattLoclTy::Apply {
                params: deref!(Params::Cons {
                    hd: PattLoclTy::Invalid(..),
                    ..
                }),
                ..
            }
        ))
    }

    #[test]
    fn test_validate_ty_mismatch() {
        let name = PattName::As {
            lbl: "x".to_string(),
            patt: Box::new(PattName::Name {
                namespace: Namespace::Root,
                name: PattString::Exactly("Classy".to_string()),
            }),
        };
        let hd = PattLoclTy::Dynamic;

        let tl = Box::new(Params::Nil);
        let params = Box::new(Params::Cons { hd, tl });
        let fst = Box::new(PattLoclTy::Apply { name, params });
        let snd = Box::new(PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        });
        let mut patt = PattLoclTy::Or { fst, snd };
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
        assert!(matches!(
            patt,
            PattLoclTy::Or {
                snd: deref!(PattLoclTy::Invalid(..)),
                ..
            }
        ));
    }

    // -- Tests for simple always-true validators ------------------------------

    #[test]
    fn test_validate_patt_string() {
        let mut patt = PattString::Wildcard;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattString::Exactly("test".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattString::StartsWith("test".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattString::EndsWith("test".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattString::Contains("test".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_prim() {
        let mut patt = Prim::Null;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Void;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Int;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Bool;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Float;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::String;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Resource;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Num;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Arraykey;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Prim::Noreturn;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_shape_label() {
        let mut patt = ShapeLabel::RegGroupLabel("field".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = ShapeLabel::StrLbl("my_field".to_string());
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = ShapeLabel::CConstLbl {
            cls_nm: "ClassName".to_string(),
            cnst_nm: "CONSTANT".to_string(),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_namespace() {
        let mut patt = Namespace::Root;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Namespace::Slash {
            prefix: Box::new(Namespace::Root),
            elt: PattString::Exactly("Foo".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_name_context() {
        let mut patt = PattNameContext::AnyNameContext;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_callback() {
        let mut patt = Callback::AnyCallback;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_reasons_callback() {
        let mut patt = ReasonsCallback::AnyReasonsCallback;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for PattLoclTy variants ----------------------------------------

    #[test]
    fn test_validate_ty_invalid() {
        let mut patt = PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic));
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
    }

    #[test]
    fn test_validate_ty_simple_patterns() {
        let mut patt = PattLoclTy::Dynamic;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattLoclTy::Nonnull;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattLoclTy::Any;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_prim() {
        let mut patt = PattLoclTy::Prim(Prim::Int);
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattLoclTy::Prim(Prim::String);
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_option() {
        let mut patt = PattLoclTy::Option(Box::new(PattLoclTy::Dynamic));
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattLoclTy::Option(Box::new(PattLoclTy::Prim(Prim::Int)));
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_tuple() {
        let mut patt = PattLoclTy::Tuple(vec![PattLoclTy::Dynamic, PattLoclTy::Nonnull]);
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattLoclTy::Tuple(vec![
            PattLoclTy::Prim(Prim::Int),
            PattLoclTy::Prim(Prim::String),
        ]);
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_apply_valid() {
        let name = PattName::Name {
            namespace: Namespace::Root,
            name: PattString::Exactly("MyClass".to_string()),
        };
        let params = Box::new(Params::Cons {
            hd: PattLoclTy::Dynamic,
            tl: Box::new(Params::Nil),
        });
        let mut patt = PattLoclTy::Apply { name, params };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_shape_valid() {
        let fields = ShapeFields::Fld {
            fld: ShapeField {
                lbl: ShapeLabel::StrLbl("id".to_string()),
                optional: false,
                patt: Box::new(PattLoclTy::Prim(Prim::Int)),
            },
            rest: Box::new(ShapeFields::Closed),
        };
        let mut patt = PattLoclTy::Shape(fields);
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_ty_as_valid() {
        let mut patt = PattLoclTy::As {
            lbl: "t".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(valid);
        assert!(matches!(env.get("t"), Some(PattBindingTy::Ty)));
    }

    #[test]
    fn test_validate_ty_or_valid() {
        let fst = Box::new(PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        });
        let snd = Box::new(PattLoclTy::As {
            lbl: "x".to_string(),
            patt: Box::new(PattLoclTy::Dynamic),
        });
        let mut patt = PattLoclTy::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(valid);
        // Variable "x" should be bound in the environment
        assert!(matches!(env.get("x"), Some(PattBindingTy::Ty)));
    }

    // -- Tests for Params variants --------------------------------------------

    #[test]
    fn test_validate_params_nil_and_wildcard() {
        let mut patt = Params::Nil;
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Params::Wildcard;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_params_exists() {
        let mut patt = Params::Exists(PattLoclTy::Dynamic);
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = Params::Exists(PattLoclTy::Prim(Prim::Int));
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_params_cons_valid() {
        let mut patt = Params::Cons {
            hd: PattLoclTy::Dynamic,
            tl: Box::new(Params::Nil),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));

        // Multiple params
        let mut patt = Params::Cons {
            hd: PattLoclTy::Prim(Prim::Int),
            tl: Box::new(Params::Cons {
                hd: PattLoclTy::Prim(Prim::String),
                tl: Box::new(Params::Nil),
            }),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_params_cons_invalid() {
        // Invalid head should make the whole thing invalid
        let mut patt = Params::Cons {
            hd: PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic)),
            tl: Box::new(Params::Nil),
        };
        assert!(!patt.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for Shape-related types ----------------------------------------

    #[test]
    fn test_validate_shape_field_valid() {
        let mut field = ShapeField {
            lbl: ShapeLabel::StrLbl("name".to_string()),
            optional: false,
            patt: Box::new(PattLoclTy::Prim(Prim::String)),
        };
        assert!(field.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_shape_field_invalid() {
        let mut field = ShapeField {
            lbl: ShapeLabel::StrLbl("name".to_string()),
            optional: false,
            patt: Box::new(PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic))),
        };
        assert!(!field.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_shape_fields_open_and_closed() {
        let mut fields = ShapeFields::Open;
        assert!(fields.validate(&mut ValidationEnv::default()));

        let mut fields = ShapeFields::Closed;
        assert!(fields.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_shape_fields_fld_valid() {
        let mut fields = ShapeFields::Fld {
            fld: ShapeField {
                lbl: ShapeLabel::StrLbl("id".to_string()),
                optional: false,
                patt: Box::new(PattLoclTy::Prim(Prim::Int)),
            },
            rest: Box::new(ShapeFields::Fld {
                fld: ShapeField {
                    lbl: ShapeLabel::StrLbl("name".to_string()),
                    optional: false,
                    patt: Box::new(PattLoclTy::Prim(Prim::String)),
                },
                rest: Box::new(ShapeFields::Closed),
            }),
        };
        assert!(fields.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_shape_fields_fld_invalid() {
        let mut fields = ShapeFields::Fld {
            fld: ShapeField {
                lbl: ShapeLabel::StrLbl("id".to_string()),
                optional: false,
                patt: Box::new(PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic))),
            },
            rest: Box::new(ShapeFields::Closed),
        };
        assert!(!fields.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for PattName variants ------------------------------------------

    #[test]
    fn test_validate_patt_name_wildcard() {
        let mut patt = PattName::Wildcard;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_name_invalid() {
        let mut patt = PattName::Invalid {
            errs: vec![],
            patt: Box::new(PattName::Wildcard),
        };
        assert!(!patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_name_name_valid() {
        let mut patt = PattName::Name {
            namespace: Namespace::Root,
            name: PattString::Exactly("MyClass".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattName::Name {
            namespace: Namespace::Slash {
                prefix: Box::new(Namespace::Root),
                elt: PattString::Exactly("Foo".to_string()),
            },
            name: PattString::Exactly("Bar".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_name_as_valid() {
        let mut patt = PattName::As {
            lbl: "cls".to_string(),
            patt: Box::new(PattName::Name {
                namespace: Namespace::Root,
                name: PattString::Exactly("MyClass".to_string()),
            }),
        };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(valid);
        assert!(matches!(env.get("cls"), Some(PattBindingTy::Name)));
    }

    #[test]
    fn test_validate_patt_name_as_shadowing() {
        // First bind a variable
        let mut env = ValidationEnv::default();
        env.add("x", PattBindingTy::Name);

        // Try to bind the same variable again - should fail
        let mut patt = PattName::As {
            lbl: "x".to_string(),
            patt: Box::new(PattName::Name {
                namespace: Namespace::Root,
                name: PattString::Exactly("MyClass".to_string()),
            }),
        };
        let valid = patt.validate(&mut env);
        assert!(!valid);
        assert!(matches!(patt, PattName::Invalid { .. }));
    }

    #[test]
    fn test_validate_patt_name_as_invalid_inner() {
        let mut patt = PattName::As {
            lbl: "cls".to_string(),
            patt: Box::new(PattName::Invalid {
                errs: vec![],
                patt: Box::new(PattName::Wildcard),
            }),
        };
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
    }

    // -- Tests for PattMemberName variants ------------------------------------

    #[test]
    fn test_validate_patt_member_name_wildcard() {
        let mut patt = PattMemberName::Wildcard;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_member_name_invalid() {
        let mut patt = PattMemberName::Invalid {
            errs: vec![],
            patt: Box::new(PattMemberName::Wildcard),
        };
        assert!(!patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_member_name_member_name_valid() {
        let mut patt = PattMemberName::MemberName {
            patt_string: PattString::Exactly("myMethod".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));

        let mut patt = PattMemberName::MemberName {
            patt_string: PattString::StartsWith("get".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_member_name_as_valid() {
        let mut patt = PattMemberName::As {
            lbl: "method".to_string(),
            patt: Box::new(PattMemberName::MemberName {
                patt_string: PattString::Exactly("myMethod".to_string()),
            }),
        };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(valid);
        assert!(matches!(env.get("method"), Some(PattBindingTy::MemberName)));
    }

    #[test]
    fn test_validate_patt_member_name_as_shadowing() {
        // First bind a variable
        let mut env = ValidationEnv::default();
        env.add("m", PattBindingTy::MemberName);

        // Try to bind the same variable again - should fail
        let mut patt = PattMemberName::As {
            lbl: "m".to_string(),
            patt: Box::new(PattMemberName::MemberName {
                patt_string: PattString::Exactly("foo".to_string()),
            }),
        };
        let valid = patt.validate(&mut env);
        assert!(!valid);
        assert!(matches!(patt, PattMemberName::Invalid { .. }));
    }

    #[test]
    fn test_validate_patt_member_name_as_invalid_inner() {
        let mut patt = PattMemberName::As {
            lbl: "method".to_string(),
            patt: Box::new(PattMemberName::Invalid {
                errs: vec![],
                patt: Box::new(PattMemberName::Wildcard),
            }),
        };
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
    }

    // -- Tests for FilePath variants ------------------------------------------

    #[test]
    fn test_validate_file_path_dot() {
        let mut patt = FilePath::Dot;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_file_path_slash() {
        let mut patt = FilePath::Slash {
            prefix: Box::new(FilePath::Dot),
            segment: PattString::Exactly("src".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));

        // Multiple segments
        let mut patt = FilePath::Slash {
            prefix: Box::new(FilePath::Slash {
                prefix: Box::new(FilePath::Dot),
                segment: PattString::Exactly("src".to_string()),
            }),
            segment: PattString::Exactly("lib".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_file_path_slash_opt() {
        let mut patt = FilePath::SlashOpt {
            prefix: Box::new(FilePath::Dot),
            segment: PattString::StartsWith("test".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for PattFile variants ------------------------------------------

    #[test]
    fn test_validate_patt_file_wildcard() {
        let mut patt = PattFile::Wildcard;
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_file_invalid() {
        let mut patt = PattFile::Invalid {
            errs: vec![],
            patt: Box::new(PattFile::Wildcard),
        };
        assert!(!patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_file_name_valid() {
        let mut patt = PattFile::Name {
            allow_glob: false,
            patt_file_path: Some(FilePath::Slash {
                prefix: Box::new(FilePath::Dot),
                segment: PattString::Exactly("src".to_string()),
            }),
            patt_file_name: PattString::Exactly("lib".to_string()),
            patt_file_extension: PattString::Exactly("rs".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));

        // Without path
        let mut patt = PattFile::Name {
            allow_glob: false,
            patt_file_path: None,
            patt_file_name: PattString::Exactly("main".to_string()),
            patt_file_extension: PattString::Exactly("rs".to_string()),
        };
        assert!(patt.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_file_as_valid() {
        let mut patt = PattFile::As {
            lbl: "file".to_string(),
            patt: Box::new(PattFile::Name {
                allow_glob: false,
                patt_file_path: None,
                patt_file_name: PattString::Exactly("main".to_string()),
                patt_file_extension: PattString::Exactly("rs".to_string()),
            }),
        };
        let mut env = ValidationEnv::default();
        let valid = patt.validate(&mut env);
        assert!(valid);
        assert!(matches!(env.get("file"), Some(PattBindingTy::File)));
    }

    #[test]
    fn test_validate_patt_file_as_shadowing() {
        // First bind a variable
        let mut env = ValidationEnv::default();
        env.add("f", PattBindingTy::File);

        // Try to bind the same variable again - should fail
        let mut patt = PattFile::As {
            lbl: "f".to_string(),
            patt: Box::new(PattFile::Name {
                allow_glob: false,
                patt_file_path: None,
                patt_file_name: PattString::Exactly("main".to_string()),
                patt_file_extension: PattString::Exactly("rs".to_string()),
            }),
        };
        let valid = patt.validate(&mut env);
        assert!(!valid);
        assert!(matches!(patt, PattFile::Invalid { .. }));
    }

    #[test]
    fn test_validate_patt_file_as_invalid_inner() {
        let mut patt = PattFile::As {
            lbl: "file".to_string(),
            patt: Box::new(PattFile::Invalid {
                errs: vec![],
                patt: Box::new(PattFile::Wildcard),
            }),
        };
        let valid = patt.validate(&mut ValidationEnv::default());
        assert!(!valid);
    }

    // -- Tests for ErrorMessage and Elem --------------------------------------

    #[test]
    fn test_validate_elem_lit() {
        let mut elem = Elem::Lit("Error message".to_string());
        assert!(elem.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_elem_ty_var_valid() {
        let mut env = ValidationEnv::default();
        env.add("t", PattBindingTy::Ty);

        let mut elem = Elem::TyVar("t".to_string());
        assert!(elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_ty_var_invalid() {
        // Variable not bound
        let mut elem = Elem::TyVar("t".to_string());
        assert!(!elem.validate(&mut ValidationEnv::default()));

        // Wrong binding type
        let mut env = ValidationEnv::default();
        env.add("t", PattBindingTy::Name);
        let mut elem = Elem::TyVar("t".to_string());
        assert!(!elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_name_var_valid() {
        let mut env = ValidationEnv::default();
        env.add("cls", PattBindingTy::Name);

        let mut elem = Elem::NameVar("cls".to_string());
        assert!(elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_name_var_invalid() {
        // Variable not bound
        let mut elem = Elem::NameVar("cls".to_string());
        assert!(!elem.validate(&mut ValidationEnv::default()));

        // Wrong binding type
        let mut env = ValidationEnv::default();
        env.add("cls", PattBindingTy::Ty);
        let mut elem = Elem::NameVar("cls".to_string());
        assert!(!elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_file_var_valid() {
        let mut env = ValidationEnv::default();
        env.add("file", PattBindingTy::File);

        let mut elem = Elem::FileVar("file".to_string());
        assert!(elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_file_var_invalid() {
        // Variable not bound
        let mut elem = Elem::FileVar("file".to_string());
        assert!(!elem.validate(&mut ValidationEnv::default()));

        // Wrong binding type
        let mut env = ValidationEnv::default();
        env.add("file", PattBindingTy::Name);
        let mut elem = Elem::FileVar("file".to_string());
        assert!(!elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_member_name_var_valid() {
        let mut env = ValidationEnv::default();
        env.add("method", PattBindingTy::MemberName);

        let mut elem = Elem::MemberNameVar("method".to_string());
        assert!(elem.validate(&mut env));
    }

    #[test]
    fn test_validate_elem_member_name_var_invalid() {
        // Variable not bound
        let mut elem = Elem::MemberNameVar("method".to_string());
        assert!(!elem.validate(&mut ValidationEnv::default()));

        // Wrong binding type
        let mut env = ValidationEnv::default();
        env.add("method", PattBindingTy::Ty);
        let mut elem = Elem::MemberNameVar("method".to_string());
        assert!(!elem.validate(&mut env));
    }

    #[test]
    fn test_validate_error_message_valid() {
        let mut env = ValidationEnv::default();
        env.add("t", PattBindingTy::Ty);
        env.add("cls", PattBindingTy::Name);

        let mut msg = ErrorMessage {
            message: vec![
                Elem::Lit("Expected type ".to_string()),
                Elem::TyVar("t".to_string()),
                Elem::Lit(" but got ".to_string()),
                Elem::NameVar("cls".to_string()),
            ],
        };
        assert!(msg.validate(&mut env));
    }

    #[test]
    fn test_validate_error_message_invalid() {
        // Reference to unbound variable
        let mut msg = ErrorMessage {
            message: vec![
                Elem::Lit("Expected type ".to_string()),
                Elem::TyVar("t".to_string()),
            ],
        };
        assert!(!msg.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for Secondary variants -----------------------------------------

    #[test]
    fn test_validate_secondary_any_snd() {
        let mut sec = Secondary::AnySnd;
        assert!(sec.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_secondary_of_error() {
        // Valid nested error
        let mut sec = Secondary::OfError(PattTypingError::Primary(Primary::AnyPrim));
        assert!(sec.validate(&mut ValidationEnv::default()));

        // Invalid nested error
        let mut sec = Secondary::OfError(PattTypingError::InvalidTyping {
            errs: vec![],
            patt: Box::new(PattTypingError::Primary(Primary::AnyPrim)),
        });
        assert!(!sec.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_secondary_violated_constraint() {
        let mut sec = Secondary::ViolatedConstraint {
            cstr: PattString::Exactly("Ctx".to_string()),
            ty_sub: PattLoclTy::Prim(Prim::Int),
            ty_sup: PattLoclTy::Prim(Prim::String),
        };
        assert!(sec.validate(&mut ValidationEnv::default()));

        // Invalid type pattern
        let mut sec = Secondary::ViolatedConstraint {
            cstr: PattString::Wildcard,
            ty_sub: PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic)),
            ty_sup: PattLoclTy::Dynamic,
        };
        assert!(!sec.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_secondary_subtyping_error() {
        let mut sec = Secondary::SubtypingError {
            sub: PattLoclTy::Prim(Prim::Int),
            sup: PattLoclTy::Prim(Prim::String),
        };
        assert!(sec.validate(&mut ValidationEnv::default()));

        // Invalid sub
        let mut sec = Secondary::SubtypingError {
            sub: PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic)),
            sup: PattLoclTy::Dynamic,
        };
        assert!(!sec.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for Primary variants -------------------------------------------

    #[test]
    fn test_validate_primary_any_prim() {
        let mut prim = Primary::AnyPrim;
        assert!(prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_member_not_found_valid() {
        let mut prim = Primary::MemberNotFound {
            is_static: None,
            kind: MemberKindPattern::AnyMemberKind,
            class_name: PattName::Name {
                namespace: Namespace::Root,
                name: PattString::Exactly("MyClass".to_string()),
            },
            member_name: PattMemberName::MemberName {
                patt_string: PattString::Exactly("myMethod".to_string()),
            },
            visibility: None,
        };
        assert!(prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_member_not_found_invalid() {
        let mut prim = Primary::MemberNotFound {
            is_static: Some(StaticPattern::StaticOnly),
            kind: MemberKindPattern::MethodOnly,
            class_name: PattName::Invalid {
                errs: vec![],
                patt: Box::new(PattName::Wildcard),
            },
            member_name: PattMemberName::Wildcard,
            visibility: Some(VisibilityPattern::PublicOnly),
        };
        assert!(!prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_cross_pkg_access_valid() {
        let mut prim = Primary::CrossPkgAccess {
            use_file: PattFile::Name {
                allow_glob: false,
                patt_file_path: None,
                patt_file_name: PattString::Exactly("main".to_string()),
                patt_file_extension: PattString::Exactly("hack".to_string()),
            },
            decl_file: PattFile::Name {
                allow_glob: false,
                patt_file_path: None,
                patt_file_name: PattString::Exactly("lib".to_string()),
                patt_file_extension: PattString::Exactly("hack".to_string()),
            },
        };
        assert!(prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_cross_pkg_access_invalid() {
        let mut prim = Primary::CrossPkgAccess {
            use_file: PattFile::Invalid {
                errs: vec![],
                patt: Box::new(PattFile::Wildcard),
            },
            decl_file: PattFile::Wildcard,
        };
        assert!(!prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_cross_pkg_access_with_requirepackage() {
        let mut prim = Primary::CrossPkgAccessWithRequirepackage {
            use_file: PattFile::Wildcard,
            decl_file: PattFile::Wildcard,
        };
        assert!(prim.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_primary_expression_tree_unsupported_operator() {
        let mut prim = Primary::ExpressionTreeUnsupportedOperator {
            class_name: PattName::Wildcard,
            member_name: PattMemberName::Wildcard,
        };
        assert!(prim.validate(&mut ValidationEnv::default()));
    }

    // -- Tests for PattTypingError variants -----------------------------------

    #[test]
    fn test_validate_patt_typing_error_invalid_typing() {
        let mut err = PattTypingError::InvalidTyping {
            errs: vec![],
            patt: Box::new(PattTypingError::Primary(Primary::AnyPrim)),
        };
        assert!(!err.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_typing_error_primary() {
        // Valid
        let mut err = PattTypingError::Primary(Primary::AnyPrim);
        assert!(err.validate(&mut ValidationEnv::default()));

        // Invalid
        let mut err = PattTypingError::Primary(Primary::MemberNotFound {
            is_static: None,
            kind: MemberKindPattern::AnyMemberKind,
            class_name: PattName::Invalid {
                errs: vec![],
                patt: Box::new(PattName::Wildcard),
            },
            member_name: PattMemberName::Wildcard,
            visibility: None,
        });
        assert!(!err.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_typing_error_apply() {
        // Valid
        let mut err = PattTypingError::Apply {
            cb: Callback::AnyCallback,
            err: Box::new(PattTypingError::Primary(Primary::AnyPrim)),
        };
        assert!(err.validate(&mut ValidationEnv::default()));

        // Invalid nested error
        let mut err = PattTypingError::Apply {
            cb: Callback::AnyCallback,
            err: Box::new(PattTypingError::InvalidTyping {
                errs: vec![],
                patt: Box::new(PattTypingError::Primary(Primary::AnyPrim)),
            }),
        };
        assert!(!err.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_typing_error_apply_reasons() {
        // Valid
        let mut err = PattTypingError::ApplyReasons {
            rsns_cb: ReasonsCallback::AnyReasonsCallback,
            secondary: Box::new(Secondary::AnySnd),
        };
        assert!(err.validate(&mut ValidationEnv::default()));

        // Invalid secondary
        let mut err = PattTypingError::ApplyReasons {
            rsns_cb: ReasonsCallback::AnyReasonsCallback,
            secondary: Box::new(Secondary::SubtypingError {
                sub: PattLoclTy::Invalid(vec![], Box::new(PattLoclTy::Dynamic)),
                sup: PattLoclTy::Dynamic,
            }),
        };
        assert!(!err.validate(&mut ValidationEnv::default()));
    }

    #[test]
    fn test_validate_patt_typing_error_or_valid() {
        // Both sides bind the same variable with same type
        let fst = Box::new(PattTypingError::Apply {
            cb: Callback::AnyCallback,
            err: Box::new(PattTypingError::Primary(Primary::MemberNotFound {
                is_static: None,
                kind: MemberKindPattern::AnyMemberKind,
                class_name: PattName::As {
                    lbl: "cls".to_string(),
                    patt: Box::new(PattName::Wildcard),
                },
                member_name: PattMemberName::Wildcard,
                visibility: None,
            })),
        });

        let snd = Box::new(PattTypingError::Primary(Primary::CrossPkgAccess {
            use_file: PattFile::As {
                lbl: "cls".to_string(), // Same variable name, same binding type
                patt: Box::new(PattFile::Wildcard),
            },
            decl_file: PattFile::Wildcard,
        }));

        let mut err = PattTypingError::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = err.validate(&mut env);

        // Should fail - 'cls' bound with different types (Name vs File)
        assert!(!valid);
    }

    #[test]
    fn test_validate_patt_typing_error_or_unbound_right() {
        // Left side binds a variable, right side doesn't
        let fst = Box::new(PattTypingError::Primary(Primary::MemberNotFound {
            is_static: None,
            kind: MemberKindPattern::AnyMemberKind,
            class_name: PattName::As {
                lbl: "cls".to_string(),
                patt: Box::new(PattName::Wildcard),
            },
            member_name: PattMemberName::Wildcard,
            visibility: None,
        }));

        let snd = Box::new(PattTypingError::Primary(Primary::AnyPrim));

        let mut err = PattTypingError::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = err.validate(&mut env);

        assert!(!valid);
        // Right side should be invalidated
        assert!(matches!(
            err,
            PattTypingError::Or {
                snd: deref!(PattTypingError::InvalidTyping { .. }),
                ..
            }
        ));
    }

    #[test]
    fn test_validate_patt_typing_error_or_unbound_left() {
        // Right side binds a variable, left side doesn't
        let fst = Box::new(PattTypingError::Primary(Primary::AnyPrim));

        let snd = Box::new(PattTypingError::Primary(Primary::MemberNotFound {
            is_static: None,
            kind: MemberKindPattern::AnyMemberKind,
            class_name: PattName::As {
                lbl: "cls".to_string(),
                patt: Box::new(PattName::Wildcard),
            },
            member_name: PattMemberName::Wildcard,
            visibility: None,
        }));

        let mut err = PattTypingError::Or { fst, snd };
        let mut env = ValidationEnv::default();
        let valid = err.validate(&mut env);

        assert!(!valid);
        // Left side should be invalidated
        assert!(matches!(
            err,
            PattTypingError::Or {
                fst: deref!(PattTypingError::InvalidTyping { .. }),
                ..
            }
        ));
    }
}
