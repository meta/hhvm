// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
use std::fmt::Display;
use std::fmt::Formatter;
use std::fmt::Result;
use std::ops::Range;

use toml::Spanned;

#[derive(Debug, PartialEq)]
pub enum Error {
    UndefinedInclude {
        name: String,
        span: (usize, usize),
    },
    DuplicateIncludePath {
        name: String,
        span: (usize, usize),
    },
    IncompleteDeployment {
        name: String,
        span: (usize, usize),
        missing_pkgs: Vec<Spanned<String>>,
        soft: bool,
    },
    InvalidIncludePath {
        path: String,
        span: (usize, usize),
    },
    MalformedIncludePath {
        include_path: String,
        span: (usize, usize),
    },
    PackagePathNotDirectory {
        path: String,
        span: (usize, usize),
    },
    IncompleteIncludes {
        name: String,
        span: (usize, usize),
        missing_pkgs: Vec<Spanned<String>>,
        soft: bool,
    },
    ImplicitIncludePathsNotAllowed {
        name: String,
        span: (usize, usize),
    },
    OverlappingImplicitPath {
        path: String,
        /// The conflicting entry, already rendered (e.g. "include_path //a/ of
        /// package foo"), so the message can name what the overlap is with.
        other: String,
        span: (usize, usize),
    },
    PackageNamePrefixCollision {
        name: String,
        package: String,
        span: (usize, usize),
    },
    ContradictoryObservabilityFlags {
        name: String,
        span: (usize, usize),
    },
    PackageNameInvalid {
        name: String,
        span: (usize, usize),
    },
    ImplicitFamilyNameInvalid {
        name: String,
        span: (usize, usize),
    },
    ImplicitMemberNameInvalid {
        name: String,
        member: String,
        span: (usize, usize),
    },
    ImplicitMemberDoesNotExist {
        name: String,
        path: String,
        span: (usize, usize),
    },
    DynamicClassLoadErrorRequiresStrictIsolation {
        name: String,
        span: (usize, usize),
    },
    LoaderPathOverlapsStrictPackage {
        path: String,
        /// The strict package's conflicting path, already rendered (e.g.
        /// "include_path //a/ of strict package foo").
        other: String,
        span: (usize, usize),
    },
    NestedLoaderPath {
        path: String,
        outer: String,
        span: (usize, usize),
    },
}

impl Error {
    pub fn undefined_package(x: &Spanned<String>) -> Self {
        let Range { start, end } = x.span();
        Self::UndefinedInclude {
            name: x.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn duplicate_include_path(x: &Spanned<String>) -> Self {
        let Range { start, end } = x.span();
        Self::DuplicateIncludePath {
            name: x.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn invalid_include_path(path: String, span: Range<usize>) -> Self {
        let Range { start, end } = span;
        Self::InvalidIncludePath {
            path,
            span: (start, end),
        }
    }

    pub fn malformed_include_path(include_path: String, span: Range<usize>) -> Self {
        let Range { start, end } = span;
        Self::MalformedIncludePath {
            include_path,
            span: (start, end),
        }
    }

    pub fn package_path_not_directory(path: String, span: Range<usize>) -> Self {
        let Range { start, end } = span;
        Self::PackagePathNotDirectory {
            path,
            span: (start, end),
        }
    }

    pub fn incomplete_deployment(
        deployment: &Spanned<String>,
        missing_pkgs: Vec<Spanned<String>>,
        soft: bool,
    ) -> Self {
        let Range { start, end } = deployment.span();
        Self::IncompleteDeployment {
            name: deployment.get_ref().into(),
            span: (start, end),
            missing_pkgs,
            soft,
        }
    }

    pub fn incomplete_includes(
        package_name: &Spanned<String>,
        missing_pkgs: Vec<Spanned<String>>,
        soft: bool,
    ) -> Self {
        let Range { start, end } = package_name.span();
        Self::IncompleteIncludes {
            name: package_name.get_ref().into(),
            span: (start, end),
            missing_pkgs,
            soft,
        }
    }

    pub fn implicit_include_paths_not_allowed(family: &Spanned<String>) -> Self {
        let Range { start, end } = family.span();
        Self::ImplicitIncludePathsNotAllowed {
            name: family.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn overlapping_implicit_path(path: String, other: String, span: Range<usize>) -> Self {
        let Range { start, end } = span;
        Self::OverlappingImplicitPath {
            path,
            other,
            span: (start, end),
        }
    }

    pub fn package_name_prefix_collision(
        family: &Spanned<String>,
        package: &Spanned<String>,
    ) -> Self {
        let Range { start, end } = family.span();
        Self::PackageNamePrefixCollision {
            name: family.get_ref().into(),
            package: package.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn contradictory_observability_flags(package: &Spanned<String>) -> Self {
        let Range { start, end } = package.span();
        Self::ContradictoryObservabilityFlags {
            name: package.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn package_name_invalid(package: &Spanned<String>) -> Self {
        let Range { start, end } = package.span();
        Self::PackageNameInvalid {
            name: package.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn implicit_family_name_invalid(family: &Spanned<String>) -> Self {
        let Range { start, end } = family.span();
        Self::ImplicitFamilyNameInvalid {
            name: family.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn implicit_member_name_invalid(member_name: &Spanned<String>, member: &str) -> Self {
        let Range { start, end } = member_name.span();
        Self::ImplicitMemberNameInvalid {
            name: member_name.get_ref().into(),
            member: member.into(),
            span: (start, end),
        }
    }

    pub fn implicit_member_does_not_exist(member_name: &Spanned<String>, path: String) -> Self {
        let Range { start, end } = member_name.span();
        Self::ImplicitMemberDoesNotExist {
            name: member_name.get_ref().into(),
            path,
            span: (start, end),
        }
    }

    pub fn dynamic_class_load_error_requires_strict_isolation(package: &Spanned<String>) -> Self {
        let Range { start, end } = package.span();
        Self::DynamicClassLoadErrorRequiresStrictIsolation {
            name: package.get_ref().into(),
            span: (start, end),
        }
    }

    pub fn loader_path_overlaps_strict_package(path: &Spanned<String>, other: String) -> Self {
        let Range { start, end } = path.span();
        Self::LoaderPathOverlapsStrictPackage {
            path: path.get_ref().into(),
            other,
            span: (start, end),
        }
    }

    pub fn nested_loader_path(path: &Spanned<String>, outer: &str) -> Self {
        let Range { start, end } = path.span();
        Self::NestedLoaderPath {
            path: path.get_ref().into(),
            outer: outer.into(),
            span: (start, end),
        }
    }

    pub fn span(&self) -> (usize, usize) {
        match self {
            Self::DuplicateIncludePath { span, .. }
            | Self::UndefinedInclude { span, .. }
            | Self::IncompleteDeployment { span, .. }
            | Self::InvalidIncludePath { span, .. }
            | Self::MalformedIncludePath { span, .. }
            | Self::PackagePathNotDirectory { span, .. }
            | Self::IncompleteIncludes { span, .. }
            | Self::ImplicitIncludePathsNotAllowed { span, .. }
            | Self::OverlappingImplicitPath { span, .. }
            | Self::PackageNamePrefixCollision { span, .. }
            | Self::ContradictoryObservabilityFlags { span, .. }
            | Self::PackageNameInvalid { span, .. }
            | Self::ImplicitFamilyNameInvalid { span, .. }
            | Self::ImplicitMemberNameInvalid { span, .. }
            | Self::ImplicitMemberDoesNotExist { span, .. }
            | Self::DynamicClassLoadErrorRequiresStrictIsolation { span, .. }
            | Self::LoaderPathOverlapsStrictPackage { span, .. }
            | Self::NestedLoaderPath { span, .. } => *span,
        }
    }

    pub fn msg(&self) -> String {
        format!("{}", self)
    }

    pub fn reasons(&self) -> Vec<(usize, usize, String)> {
        // Might need reasons later for more complicated error messages
        vec![]
    }
}
impl Display for Error {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        match self {
            Self::UndefinedInclude { name, .. } => {
                write!(f, "Undefined package: {}", name)?;
            }
            Self::DuplicateIncludePath { name, .. } => {
                write!(
                    f,
                    "This include_path can only be used in one package: {}",
                    name
                )?;
            }
            Self::IncompleteDeployment {
                name,
                missing_pkgs,
                soft,
                ..
            } => {
                let soft_str = if *soft { "soft-" } else { "" };
                write!(
                    f,
                    "{} must {}deploy all nested {}included packages. Missing ",
                    name, soft_str, soft_str
                )?;
                for (i, pkg) in missing_pkgs.iter().enumerate() {
                    if i == missing_pkgs.len() - 1 {
                        write!(f, "{}", pkg.get_ref())?;
                    } else {
                        write!(f, "{}, ", pkg.get_ref())?;
                    }
                }
            }
            Self::InvalidIncludePath { path, .. } => {
                write!(f, "include_path {} does not exist", path)?;
            }
            Self::MalformedIncludePath { include_path, .. } => {
                write!(
                    f,
                    "include_path {} is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
                    include_path
                )?;
            }
            Self::PackagePathNotDirectory { path, .. } => {
                write!(f, "package path //{} must be a directory", path)?;
            }
            Self::IncompleteIncludes {
                name,
                missing_pkgs,
                soft,
                ..
            } => {
                let soft_str = if *soft { "soft-" } else { "" };
                write!(
                    f,
                    "{} must {}include all nested {}included packages. Missing ",
                    name, soft_str, soft_str
                )?;
                for (i, pkg) in missing_pkgs.iter().enumerate() {
                    if i == missing_pkgs.len() - 1 {
                        write!(f, "{}", pkg.get_ref())?;
                    } else {
                        write!(f, "{}, ", pkg.get_ref())?;
                    }
                }
            }
            Self::ImplicitIncludePathsNotAllowed { name, .. } => {
                write!(
                    f,
                    "implicit_packages.{} must not specify include_paths: the include paths are derived from its path",
                    name
                )?;
            }
            Self::OverlappingImplicitPath { path, other, .. } => {
                write!(
                    f,
                    "implicit_packages path //{} conflicts with {}; explicit package paths cannot be nested under a family, and family paths cannot overlap",
                    path, other
                )?;
            }
            Self::PackageNamePrefixCollision { name, package, .. } => {
                write!(
                    f,
                    "implicit_packages family {} collides with package {} (a family name may not equal or be a prefix of a package name)",
                    name, package
                )?;
            }
            Self::ContradictoryObservabilityFlags { name, .. } => {
                write!(
                    f,
                    "{} declares both enable_strict_isolation and allow_deployed_packages_checking, which contradict each other",
                    name
                )?;
            }
            Self::PackageNameInvalid { name, .. } => {
                write!(f, "Package name {} must be a valid Hack identifier", name)?;
            }
            Self::ImplicitFamilyNameInvalid { name, .. } => {
                write!(
                    f,
                    "Implicit package family name {} must be a valid Hack identifier",
                    name
                )?;
            }
            Self::ImplicitMemberNameInvalid { name, member, .. } => {
                write!(
                    f,
                    "Implicit package member segment {} in {} must be a valid Hack identifier",
                    member, name
                )?;
            }
            Self::ImplicitMemberDoesNotExist { name, path, .. } => {
                write!(
                    f,
                    "Implicit package member {} does not exist at //{}",
                    name, path
                )?;
            }
            Self::DynamicClassLoadErrorRequiresStrictIsolation { name, .. } => {
                write!(
                    f,
                    "Package {} must enable strict isolation to raise dynamic class load errors",
                    name
                )?;
            }
            Self::LoaderPathOverlapsStrictPackage { path, other, .. } => {
                write!(
                    f,
                    "strict_isolation.loader_paths entry //{} overlaps {}; entries must lie outside every strict package",
                    path, other
                )?;
            }
            Self::NestedLoaderPath { path, outer, .. } => {
                write!(
                    f,
                    "strict_isolation.loader_paths entry //{} is nested under entry //{}; entries must not be nested",
                    path, outer
                )?;
            }
        };
        Ok(())
    }
}
