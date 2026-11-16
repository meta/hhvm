// Copyright (c) Meta, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::path::Path;

use anyhow::Context;
use anyhow::Result;
use hack_name::is_valid_identifier;
use toml::Spanned;

use crate::config::*;
use crate::error::*;
use crate::types::DeploymentMap;
use crate::types::ImplicitPackage;
use crate::types::ImplicitPackageMap;
pub use crate::types::NameSet;
use crate::types::Package;
use crate::types::PackageMap;

struct PackagePathValidation {
    valid: bool,
    is_directory: Option<bool>,
}

struct PackagePathValidator<'a> {
    packages_toml_dir: &'a Path,
    strict: bool,
}

impl PackagePathValidator<'_> {
    fn validate_and_normalize(
        &self,
        configured_path: &mut Spanned<String>,
        errors: &mut Vec<Error>,
    ) -> PackagePathValidation {
        let original = configured_path.get_ref().clone();
        let span = configured_path.span();
        let relative = original.strip_prefix("//").unwrap_or(&original).to_owned();
        *configured_path.get_mut() = relative.clone();

        let mut valid = true;
        if !original.starts_with("//") || original.contains("./") {
            errors.push(Error::malformed_include_path(
                original.clone(),
                span.clone(),
            ));
            valid = false;
        }

        if !self.strict {
            return PackagePathValidation {
                valid,
                is_directory: None,
            };
        }

        let filesystem_path = relative.trim_end_matches('/');
        let is_directory = match std::fs::metadata(self.packages_toml_dir.join(filesystem_path)) {
            Ok(metadata) => {
                if metadata.is_dir() && !original.ends_with('/') {
                    if valid {
                        errors.push(Error::malformed_include_path(relative, span.clone()));
                    }
                    valid = false;
                }
                Some(metadata.is_dir())
            }
            Err(_) => {
                errors.push(Error::invalid_include_path(relative, span));
                valid = false;
                None
            }
        };
        PackagePathValidation {
            valid,
            is_directory,
        }
    }
}

/// Whether normalized package path `path` covers `other`. A directory, or the
/// empty root, covers everything under it; a file covers only itself.
fn covers(path: &str, other: &str) -> bool {
    if path.is_empty() || path.ends_with('/') {
        other.starts_with(path)
    } else {
        other == path
    }
}

/// Normalizes `[strict_isolation] loader_paths`, dropping invalid and nested entries
/// and any that overlap a strict package, so that no strict package can load another.
fn validate_loader_paths(
    config: &mut Config,
    path_validator: &PackagePathValidator<'_>,
    errors: &mut Vec<Error>,
) -> Vec<String> {
    let Some(loader_paths) = config.strict_isolation.loader_paths.take() else {
        return vec![];
    };
    let strict_paths: Vec<(&str, String)> = config
        .packages
        .iter()
        .filter(|(_, package)| package.enable_strict_isolation)
        .flat_map(|(name, package)| {
            package
                .include_paths
                .iter()
                .flat_map(|paths| paths.iter())
                .map(move |path| {
                    let path = path.get_ref().as_str();
                    let other =
                        format!("include_path //{path} of strict package {}", name.get_ref());
                    (path, other)
                })
        })
        .chain(config.implicit_packages.iter().map(|(name, family)| {
            let path = family.path.get_ref().as_str();
            let other = format!(
                "path //{path} of implicit_packages family {}",
                name.get_ref()
            );
            (path, other)
        }))
        .collect();

    let candidates: Vec<Spanned<String>> = loader_paths
        .iter()
        .filter_map(|loader_path| {
            let mut loader_path = loader_path.clone();
            let validation = path_validator.validate_and_normalize(&mut loader_path, errors);
            if !validation.valid {
                return None;
            }
            let path = loader_path.get_ref().as_str();
            if validation.is_directory == Some(false) && path.ends_with('/') {
                errors.push(Error::package_path_not_directory(
                    path.to_owned(),
                    loader_path.span(),
                ));
                return None;
            }
            // Package membership is a raw prefix match, so a strict file entry
            // also claims the paths it prefixes.
            if let Some((_, other)) = strict_paths
                .iter()
                .find(|(strict_path, _)| path.starts_with(strict_path) || covers(path, strict_path))
            {
                errors.push(Error::loader_path_overlaps_strict_package(
                    &loader_path,
                    other.clone(),
                ));
                return None;
            }
            Some(loader_path)
        })
        .collect();

    candidates
        .iter()
        .filter(|inner| {
            let outer = candidates.iter().find(|outer| {
                outer.get_ref() != inner.get_ref() && covers(outer.get_ref(), inner.get_ref())
            });
            if let Some(outer) = outer {
                errors.push(Error::nested_loader_path(inner, outer.get_ref()));
            }
            outer.is_none()
        })
        .map(|loader_path| loader_path.get_ref().clone())
        .collect()
}

fn validate_implicit_member_paths(config: &Config, packages_toml: &str, errors: &mut Vec<Error>) {
    let packages_toml_path = Path::new(packages_toml).parent().unwrap_or(Path::new("/"));
    let validate_names = |names: &Option<NameSet>, errors: &mut Vec<Error>| {
        for name in names.iter().flat_map(|names| names.iter()) {
            let Some((family_name, member_name)) = split_member_name(name.get_ref()) else {
                continue;
            };
            if !is_valid_identifier(member_name) {
                continue;
            }
            let Some(family) = config
                .implicit_packages
                .iter()
                .find_map(|(name, family)| (name.get_ref() == family_name).then_some(family))
            else {
                continue;
            };
            let member_path = format!("{}{}/", family.path.get_ref(), member_name);
            if !packages_toml_path.join(&member_path).is_dir() {
                errors.push(Error::implicit_member_does_not_exist(name, member_path));
            }
        }
    };

    for package in config.packages.values() {
        validate_names(&package.includes, errors);
        validate_names(&package.soft_includes, errors);
    }
    if let Some(deployments) = &config.deployments {
        for deployment in deployments.values() {
            validate_names(&deployment.packages, errors);
            validate_names(&deployment.soft_packages, errors);
        }
    }
    for family in config.implicit_packages.values() {
        validate_names(&family.includes, errors);
        validate_names(&family.soft_includes, errors);
    }
}

/// A declaration referenced by the ordered package path map.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PackageOrImplicitPackage {
    Package(Spanned<String>, Package),
    ImplicitPackage(Spanned<String>, ImplicitPackage),
}

#[derive(Debug, Default)]
pub struct PackageInfo {
    packages: PackageMap,
    deployments: Option<DeploymentMap>,
    implicit_packages: ImplicitPackageMap,
    include_path_to_package_map: Vec<(String, PackageOrImplicitPackage)>,
    loader_paths: Vec<String>,
    line_offsets: Vec<usize>,
    errors: Vec<Error>,
}

impl PackageInfo {
    fn from_text(
        strict: bool,
        enable_implicit_packages: bool,
        packages_toml: &str,
    ) -> Result<PackageInfo> {
        let mut errors = vec![];

        // read the PACKAGES.toml file
        let contents = std::fs::read_to_string(packages_toml)
            .with_context(|| format!("Failed to read config file with path: {}", packages_toml))?;
        let mut config: Config = toml::from_str(&contents)
            .with_context(|| format!("Failed to parse config file with contents: {}", contents))?;
        let line_offsets = contents
            .char_indices()
            .filter(|&(_i, c)| c == '\n')
            .map(|(i, _)| i)
            .collect::<Vec<_>>();
        let path_validator = PackagePathValidator {
            packages_toml_dir: Path::new(packages_toml).parent().unwrap_or(Path::new("/")),
            strict,
        };

        // perform error check on include_paths
        for (_, package) in config.packages.iter_mut() {
            if let Some(dirs) = &mut package.include_paths {
                let dirs_cloned = dirs.clone();
                dirs_cloned.iter().for_each(|d| {
                    let mut spanned_dir = dirs.take(d).unwrap();
                    path_validator.validate_and_normalize(&mut spanned_dir, &mut errors);
                    dirs.insert(spanned_dir);
                });
                dirs.sort_by(|a, b| b.get_ref().cmp(a.get_ref()));
            }
        }

        if !enable_implicit_packages {
            // Keep the feature inert when gated off: ignore family declarations
            // before validating or transporting package configuration.
            config.implicit_packages.clear();
        } else {
            // Normalize family paths like include_paths. A family path is always
            // directory-shaped, and strict parsing also validates it against the
            // filesystem through the same helper used for include_paths.
            let mut invalid: Vec<Spanned<String>> = vec![];
            for (name, fam) in config.implicit_packages.iter_mut() {
                let original = fam.path.get_ref().clone();
                let span = fam.path.span();
                let mut validation =
                    path_validator.validate_and_normalize(&mut fam.path, &mut errors);
                if validation.valid && !original.ends_with('/') {
                    errors.push(Error::malformed_include_path(original, span.clone()));
                    validation.valid = false;
                } else if validation.valid && validation.is_directory == Some(false) {
                    errors.push(Error::package_path_not_directory(
                        fam.path.get_ref().clone(),
                        span,
                    ));
                    validation.valid = false;
                }
                if !validation.valid {
                    invalid.push(name.clone());
                }
            }
            for name in invalid {
                config.implicit_packages.shift_remove(&name);
            }
        }

        if strict {
            validate_implicit_member_paths(&config, packages_toml, &mut errors);
        }
        let loader_paths = validate_loader_paths(&mut config, &path_validator, &mut errors);
        config.check_config(&mut errors);

        // Sort paths in reverse lexicographic order so the first prefix match
        // is the most specific package path.
        let mut include_path_to_package_map = config
            .packages
            .iter()
            .flat_map(|(name, package)| {
                package
                    .include_paths
                    .as_ref()
                    .into_iter()
                    .flat_map(|paths| paths.iter())
                    .map(|path| {
                        (
                            path.get_ref().clone(),
                            PackageOrImplicitPackage::Package(name.clone(), package.clone()),
                        )
                    })
            })
            .chain(config.implicit_packages.iter().map(|(name, family)| {
                (
                    family.path.get_ref().clone(),
                    PackageOrImplicitPackage::ImplicitPackage(name.clone(), family.clone()),
                )
            }))
            .collect::<Vec<_>>();
        include_path_to_package_map.sort_by(|(left, _), (right, _)| right.cmp(left));

        Ok(Self {
            packages: config.packages,
            deployments: config.deployments,
            implicit_packages: config.implicit_packages,
            include_path_to_package_map,
            loader_paths,
            line_offsets,
            errors,
        })
    }

    pub fn from_text_strict(
        enable_implicit_packages: bool,
        packages_toml: &str,
    ) -> Result<PackageInfo> {
        PackageInfo::from_text(true, enable_implicit_packages, packages_toml)
    }

    pub fn from_text_non_strict(
        enable_implicit_packages: bool,
        packages_toml: &str,
    ) -> Result<PackageInfo> {
        PackageInfo::from_text(false, enable_implicit_packages, packages_toml)
    }

    pub fn packages(&self) -> &PackageMap {
        &self.packages
    }

    pub fn deployments(&self) -> Option<&DeploymentMap> {
        self.deployments.as_ref()
    }

    pub fn implicit_packages(&self) -> &ImplicitPackageMap {
        &self.implicit_packages
    }

    /// Returns package paths in reverse lexicographic order so prefix lookup
    /// selects the most specific configured path first.
    pub fn include_path_to_package_map(&self) -> &[(String, PackageOrImplicitPackage)] {
        &self.include_path_to_package_map
    }

    /// Normalized `[strict_isolation] loader_paths` that passed validation.
    pub fn loader_paths(&self) -> &[String] {
        &self.loader_paths
    }

    pub fn errors(&self) -> &[Error] {
        &self.errors[..]
    }

    pub fn line_number(&self, byte_offset: usize) -> usize {
        match self.line_offsets.binary_search(&byte_offset) {
            Ok(n) | Err(n) => n + 1,
        }
    }

    pub fn beginning_of_line(&self, line_number: usize) -> usize {
        if line_number == 1 {
            1
        } else {
            let line_idx = line_number - 1;
            let prev_line_idx = line_idx - 1;
            let prev_line_end = self.line_offsets[prev_line_idx];
            prev_line_end + 1
        }
    }
}

#[cfg(test)]
mod test {
    use std::path::Path;
    use std::path::PathBuf;
    use std::sync::LazyLock;

    use regex::Regex;

    use super::*;

    pub static SRCDIR: LazyLock<PathBuf> =
        LazyLock::new(|| Path::new(file!()).parent().unwrap().to_path_buf());

    #[test]
    fn test_parsing_basic_file() {
        let test_path = SRCDIR.as_path().join("tests/package-1.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        assert!(info.errors.is_empty());

        let foo = &info.packages()["foo"];
        assert!(foo.includes.is_none());

        let bar = &info.packages()["bar"];
        assert_eq!(bar.includes.as_ref().unwrap()[0].get_ref(), "foo");

        let baz = &info.packages()["baz"];
        assert_eq!(baz.includes.as_ref().unwrap()[0].get_ref(), "foo");
        assert_eq!(baz.includes.as_ref().unwrap()[1].get_ref(), "bar");

        let my_prod = &info.deployments().unwrap()["my-prod"];
        assert_eq!(my_prod.packages.as_ref().unwrap()[0].get_ref(), "foo");
        assert_eq!(my_prod.packages.as_ref().unwrap()[1].get_ref(), "bar");
    }

    #[test]
    fn test_contradictory_observability_flags() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-contradictory-flags.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        assert_eq!(
            errors,
            vec![String::from(
                "contradictory declares both enable_strict_isolation and \
                 allow_deployed_packages_checking, which contradict each other"
            )]
        );
    }

    #[test]
    fn test_dynamic_class_load_error_policy() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-dynamic-class-load-error.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();

        assert!(info.packages()["strict"].should_raise_dynamic_class_load_error());
        assert!(
            info.packages()["strict"]
                .raise_dynamic_class_load_error
                .is_none()
        );
        assert!(!info.packages()["notice"].should_raise_dynamic_class_load_error());
        assert!(info.packages()["explicit_error"].should_raise_dynamic_class_load_error());
        assert!(!info.packages()["default"].should_raise_dynamic_class_load_error());
        assert!(info.implicit_packages()["prototypes"].should_raise_dynamic_class_load_error());
        assert!(
            info.implicit_packages()["prototypes"]
                .raise_dynamic_class_load_error
                .is_none()
        );
        assert!(!info.implicit_packages()["notices"].should_raise_dynamic_class_load_error());
        assert!(
            info.implicit_packages()["explicit_errors"].should_raise_dynamic_class_load_error()
        );
        assert_eq!(
            info.errors().iter().map(Error::msg).collect::<Vec<_>>(),
            vec!["Package invalid must enable strict isolation to raise dynamic class load errors"]
        );
    }

    #[test]
    fn test_config_errors1() {
        let test_path = SRCDIR.as_path().join("tests/package-3.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        assert_eq!(info.errors.len(), 2);
        assert_eq!(info.errors[0].msg(), "Undefined package: baz");
        assert_eq!(info.errors[1].msg(), "Undefined package: baz");
    }

    #[test]
    fn test_config_errors2() {
        let test_path = SRCDIR.as_path().join("tests/package-4.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        let errors = info
            .errors
            .iter()
            .map(|e| e.msg())
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(
            errors,
            [
                String::from(
                    "my-prod must deploy all nested included packages. Missing e, g, h, i",
                ),
                String::from("a must include all nested included packages. Missing c, d, e, f, g"),
                String::from("b must include all nested included packages. Missing a, e, f, g"),
                String::from("c must include all nested included packages. Missing b, d, f, g"),
                String::from("d must include all nested included packages. Missing f, g"),
                String::from("f must include all nested included packages. Missing g"),
                String::from("g must include all nested included packages. Missing f"),
                String::from("h must include all nested included packages. Missing i"),
                String::from("i must include all nested included packages. Missing j"),
                String::from("j must include all nested included packages. Missing h"),
            ]
            .iter()
            .cloned()
            .collect::<std::collections::HashSet<_>>()
        );
    }

    #[test]
    fn test_config_internprod() {
        let test_path = SRCDIR.as_path().join("tests/package-internprod.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        let errors = info
            .errors
            .iter()
            .map(|e| e.msg())
            .collect::<std::collections::HashSet<_>>();
        eprintln!("{:?}", errors);
        assert_eq!(
            errors,
            [
                String::from("intern3 must soft-include all nested soft-included packages. Missing prod_pulled_from_intern"),
                String::from("prod3 must soft-deploy all nested soft-included packages. Missing prod_pulled_from_intern"),
            ]
            .iter()
            .cloned()
            .collect::<std::collections::HashSet<_>>()
        );
    }

    #[test]
    fn test_soft() {
        let test_path = SRCDIR.as_path().join("tests/package-5.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        let c = &info.packages()["c"];
        let errors = info
            .errors
            .iter()
            .map(|e| e.msg())
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(
            errors,
            [
                String::from("a must include all nested included packages. Missing c"),
                String::from("f must soft-deploy all nested soft-included packages. Missing b"),
                String::from("g must deploy all nested included packages. Missing c"),
            ]
            .iter()
            .cloned()
            .collect::<std::collections::HashSet<_>>()
        );

        assert_eq!(c.soft_includes.as_ref().unwrap()[0].get_ref(), "b");

        let d = &info.deployments().unwrap()["d"];
        assert_eq!(d.packages.as_ref().unwrap()[0].get_ref(), "c");
        assert_eq!(d.soft_packages.as_ref().unwrap()[0].get_ref(), "b");
    }

    #[test]
    fn test_include_paths1() {
        let test_path = SRCDIR.as_path().join("tests/package-6.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();
        let included_dirs = info.packages()["foo"].include_paths.as_ref().unwrap();
        assert_eq!(included_dirs.len(), 2);
        assert!(
            Regex::new("doesnotexist.php")
                .unwrap()
                .is_match(included_dirs[1].get_ref())
        );
        assert!(
            Regex::new("doesnotexist/")
                .unwrap()
                .is_match(included_dirs[0].get_ref())
        );
    }

    #[test]
    fn test_include_paths_error() {
        let test_path = SRCDIR.as_path().join("tests/package-6.toml");
        let info = PackageInfo::from_text(true, false, test_path.to_str().unwrap()).unwrap();

        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();

        assert_eq!(errors.len(), 9);

        let expected = [
            String::from(
                "include_path * is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ),
            String::from(r#"include_path doesnotexist.php does not exist"#),
            String::from(
                "include_path bar is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ),
            String::from(
                "include_path bar/ is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ),
        ];

        assert!(expected[0] == errors[7]);
        assert!(Regex::new(&expected[1]).unwrap().is_match(&errors[0]));
        assert!(expected[2] == errors[2]);
        assert!(expected[3] == errors[5]);
    }

    #[test]
    fn test_include_paths_non_strict() {
        let test_path = SRCDIR.as_path().join("tests/package-6.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        assert!(errors.len() == 3);
        // with non-strict PackageInfo parsing only "malformed path" errors should be generated
        let expected = Regex::new(r#".*malformed.*"#).unwrap();
        let filtered_errors = errors.iter().filter(|x| !expected.is_match(x));
        assert!(filtered_errors.count() == 0);
    }

    #[test]
    fn test_include_paths_error_2() {
        let test_path = SRCDIR.as_path().join("tests/package-7.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        let expected = [
            String::from(
                "include_path //doesnotexist/./bar/ is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ),
            String::from(
                "include_path //doesnotexist/../bar/ is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ),
        ];
        assert!(errors[0] == expected[0]);
        assert!(errors[1] == expected[1]);
    }

    #[test]
    fn test_include_paths_is_reverse_sorted_in_package() {
        let test_path = SRCDIR.as_path().join("tests/package-8.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        let baz = &info.packages()["baz"];
        let include_paths = &baz.include_paths.as_ref().unwrap();
        assert!(include_paths[0].get_ref().ends_with("longest/"));
        assert!(include_paths[1].get_ref().ends_with("longer/"));
        assert!(include_paths[2].get_ref().ends_with("long/"));
    }

    #[test]
    fn test_no_duplicate_include_paths() {
        let test_path = SRCDIR.as_path().join("tests/package-9.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        let expected = [
            String::from("This include_path can only be used in one package: path/to/longest/"),
            String::from("This include_path can only be used in one package: path/to/long/"),
        ];
        assert!(errors[0] == expected[0]);
        assert!(errors[1] == expected[1]);
    }

    #[test]
    fn test_implicit_packages_basic() {
        let test_path = SRCDIR.as_path().join("tests/package-implicit.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(
            info.errors().is_empty(),
            "unexpected errors: {:?}",
            info.errors().iter().map(|e| e.msg()).collect::<Vec<_>>()
        );
        // The family is recorded but NOT expanded into per-directory packages:
        // no member entry appears in the package map at parse time.
        assert!(info.packages().get("prototypes.foo").is_none());
        let fam = &info.implicit_packages()["prototypes"];
        // `path` is normalized to the leading-`//`-stripped form, like include_paths.
        assert_eq!(fam.path.get_ref(), "www/prototypes/");
        assert_eq!(fam.includes.as_ref().unwrap()[0].get_ref(), "intern");
        assert_eq!(info.include_path_to_package_map().len(), 2);
        assert_eq!(info.include_path_to_package_map()[0].0, "www/prototypes/");
        assert!(matches!(
            &info.include_path_to_package_map()[0].1,
            PackageOrImplicitPackage::ImplicitPackage(name, _) if name.get_ref() == "prototypes"
        ));
        assert_eq!(info.include_path_to_package_map()[1].0, "www/");
        assert!(matches!(
            &info.include_path_to_package_map()[1].1,
            PackageOrImplicitPackage::Package(name, _) if name.get_ref() == "intern"
        ));
    }

    #[test]
    fn test_implicit_packages_validation_errors() {
        let test_path = SRCDIR.as_path().join("tests/package-implicit-bad.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        // Sorted rather than in emission order, so reordering the checks does
        // not churn the test -- but a Vec, not a set, so a check that fires
        // twice for one problem still shows up as a duplicate.
        let mut errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        errors.sort();
        assert_eq!(
            errors,
            vec![
                String::from(
                    "implicit_packages family www_pkg collides with package www_pkg (a family name may not equal or be a prefix of a package name)",
                ),
                String::from(
                    "implicit_packages path //www/prototypes/ conflicts with include_path //www/prototypes/shared/ of package www_pkg; explicit package paths cannot be nested under a family, and family paths cannot overlap",
                ),
                String::from(
                    "implicit_packages.www_pkg must not specify include_paths: the include paths are derived from its path",
                ),
            ]
        );
    }

    #[test]
    fn test_implicit_family_malformed_path() {
        // A family whose `path` is malformed is reported once and dropped, so it
        // contributes nothing to the later checks: the first family's path is a
        // prefix of `www_pkg`'s include_path, yet the disjointness check does not
        // fire.
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-malformed.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        let malformed = |p: &str| {
            format!(
                "include_path {} is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
                p
            )
        };
        assert_eq!(
            errors,
            vec![
                malformed("www/prototypes/"),
                malformed("//www/prototypes"),
                malformed("//www/./prototypes/"),
            ]
        );
        // Every family was dropped, so nothing is carried downstream.
        assert!(info.implicit_packages().is_empty());
        assert_eq!(info.include_path_to_package_map().len(), 1);
    }

    #[test]
    fn test_implicit_family_strict_path_validation() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-strict-paths.toml");
        let info = PackageInfo::from_text(true, true, test_path.to_str().unwrap()).unwrap();
        let errors = info
            .errors
            .iter()
            .map(|error| error.msg())
            .collect::<Vec<_>>();

        assert_eq!(
            errors,
            vec![
                String::from("include_path missing-family-root/ does not exist"),
                String::from("package path //package-implicit.toml/ must be a directory"),
            ]
        );
        assert_eq!(
            info.implicit_packages()
                .keys()
                .map(|name| name.get_ref().as_str())
                .collect::<Vec<_>>(),
            vec!["root"]
        );
    }

    #[test]
    fn test_implicit_member_strict_path_validation() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-strict-members.toml");

        let non_strict = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(
            !non_strict
                .errors()
                .iter()
                .any(|error| matches!(error, Error::ImplicitMemberDoesNotExist { .. })),
            "non-strict parsing must not validate implicit member paths"
        );

        let strict = PackageInfo::from_text(true, true, test_path.to_str().unwrap()).unwrap();
        let mut errors = strict
            .errors()
            .iter()
            .filter(|error| matches!(error, Error::ImplicitMemberDoesNotExist { .. }))
            .map(|error| error.msg())
            .collect::<Vec<_>>();
        errors.sort();
        assert_eq!(
            errors,
            vec![
                String::from(
                    "Implicit package member family.missing_deployment does not exist at //strict-members/missing_deployment/",
                ),
                String::from(
                    "Implicit package member family.missing_family does not exist at //strict-members/missing_family/",
                ),
                String::from(
                    "Implicit package member family.missing_family_soft does not exist at //strict-members/missing_family_soft/",
                ),
                String::from(
                    "Implicit package member family.missing_package does not exist at //strict-members/missing_package/",
                ),
                String::from(
                    "Implicit package member family.missing_soft_deployment does not exist at //strict-members/missing_soft_deployment/",
                ),
            ]
        );
    }

    #[test]
    fn test_implicit_packages_disabled() {
        // With the feature gated off (the default), an [implicit_packages]
        // stanza is ignored and the family is not processed.
        let test_path = SRCDIR.as_path().join("tests/package-implicit.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        assert!(info.errors().is_empty());
        // The family is dropped, so nothing is carried downstream.
        assert!(info.implicit_packages().is_empty());
        // Ignoring the stanza is otherwise inert: the declared package still
        // parses, and the family does not leak in as a regular package.
        assert_eq!(
            info.packages()
                .keys()
                .map(|k| k.get_ref().as_str())
                .collect::<Vec<_>>(),
            vec!["intern"]
        );
    }

    #[test]
    fn test_implicit_family_vs_family_overlap() {
        // Two families whose paths are prefix-related must be rejected, else a
        // file under the overlap would have order-dependent membership.
        let test_path = SRCDIR.as_path().join("tests/package-implicit-overlap.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        assert_eq!(
            errors,
            vec![String::from(
                "implicit_packages path //www/prototypes/sub/ conflicts with path //www/prototypes/ of implicit_packages family prototypes; explicit package paths cannot be nested under a family, and family paths cannot overlap",
            )]
        );
    }

    #[test]
    fn test_implicit_deployment_closure() {
        // A deployment may name a family (expands to all members) or an
        // individual member `F.D`; both normalize to the family `F` for the
        // transitive-closure check, so deploying it together with its hard
        // include `intern` is complete.
        let test_path = SRCDIR.as_path().join("tests/package-implicit-deploy.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(
            info.errors().is_empty(),
            "unexpected errors: {:?}",
            info.errors().iter().map(|e| e.msg()).collect::<Vec<_>>()
        );
    }

    #[test]
    fn test_implicit_family_references_must_be_defined() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-undefined-include.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert_eq!(
            info.errors()
                .iter()
                .map(|error| error.msg())
                .collect::<Vec<_>>(),
            vec![
                "Undefined package: missing_hard",
                "Undefined package: missing_soft",
            ]
        );
    }

    #[test]
    fn test_implicit_member_references() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-member-reference.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(
            info.errors().is_empty(),
            "unexpected errors: {:?}",
            info.errors()
                .iter()
                .map(|error| error.msg())
                .collect::<Vec<_>>()
        );
        let consumer = &info.packages()["consumer"];
        assert_eq!(
            consumer.includes.as_ref().unwrap()[0].get_ref(),
            "prototypes.package_hard"
        );
        assert_eq!(
            consumer.soft_includes.as_ref().unwrap()[0].get_ref(),
            "prototypes.package_soft"
        );
        let deployment = &info.deployments().unwrap()["prod"];
        assert!(
            deployment
                .packages
                .as_ref()
                .unwrap()
                .contains("prototypes.deploy_hard")
        );
        assert!(
            deployment
                .soft_packages
                .as_ref()
                .unwrap()
                .contains("prototypes.deploy_soft")
        );
    }

    #[test]
    fn test_implicit_member_transitive_closure() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-member-transitive-closure.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert_eq!(
            info.errors()
                .iter()
                .map(|error| error.msg())
                .collect::<Vec<_>>(),
            vec![
                "consumer_missing_member must include all nested included packages. Missing prototypes.checkout",
                "missing_member must deploy all nested included packages. Missing prototypes.checkout",
            ]
        );
    }

    #[test]
    fn test_implicit_member_name_must_be_identifier() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-invalid-member-reference.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert_eq!(
            info.errors()
                .iter()
                .map(|error| error.msg())
                .collect::<Vec<_>>(),
            vec![
                "Implicit package member segment bad-name in prototypes.bad-name must be a valid Hack identifier"
            ]
        );
    }

    #[test]
    fn test_implicit_family_and_member_deployment_override() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-implicit-deployment-conflict.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(info.errors().is_empty());
    }

    #[test]
    fn test_package_names_must_be_identifiers() {
        let test_path = SRCDIR.as_path().join("tests/package-invalid-names.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        let errors = info
            .errors()
            .iter()
            .map(|error| error.msg())
            .collect::<Vec<_>>();
        assert_eq!(
            errors,
            vec![
                "Package name explicit.bad must be a valid Hack identifier",
                "Implicit package member segment bad-name in prototypes.bad-name must be a valid Hack identifier",
                "Implicit package member segment deploy-bad in prototypes.deploy-bad must be a valid Hack identifier",
                "Implicit package member segment family-bad in prototypes.family-bad must be a valid Hack identifier",
                "Implicit package family name proto.v1 must be a valid Hack identifier",
            ]
        );
    }

    #[test]
    fn test_rollout_transitivity() {
        let test_path = SRCDIR.as_path().join("tests/package-rollout.toml");
        let info = PackageInfo::from_text(false, false, test_path.to_str().unwrap()).unwrap();
        let errors = info.errors.iter().map(|e| e.msg()).collect::<Vec<_>>();
        let expected = [String::from(
            "tmp must include all nested included packages. Missing prod, soft",
        )];
        assert!(errors[0] == expected[0]);
    }

    #[test]
    fn test_loader_paths_must_not_overlap_strict_packages() {
        let test_path = SRCDIR.as_path().join("tests/package-loader-paths.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();

        assert_eq!(
            info.loader_paths(),
            ["loader/", "loose/loader.php", "loose/script", "strictly/"],
            "only well-formed paths outside every strict package survive"
        );
        assert_eq!(
            info.errors().iter().map(Error::msg).collect::<Vec<_>>(),
            vec![
                "strict_isolation.loader_paths entry //strict/nested/ overlaps include_path //strict/ of strict package strict; entries must lie outside every strict package",
                "strict_isolation.loader_paths entry //strict_file.php overlaps include_path //strict_file.php of strict package strict; entries must lie outside every strict package",
                "strict_isolation.loader_paths entry //strict_file.php_extra/ overlaps include_path //strict_file.php of strict package strict; entries must lie outside every strict package",
                "strict_isolation.loader_paths entry //family/member/ overlaps path //family/ of implicit_packages family family; entries must lie outside every strict package",
                "strict_isolation.loader_paths entry // overlaps include_path //strict_file.php of strict package strict; entries must lie outside every strict package",
                "include_path loader-without-slashes/ is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
            ]
        );
    }

    #[test]
    fn test_loader_paths_strict_validation() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-loader-paths-strict.toml");

        let non_strict = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();
        assert!(
            non_strict.errors().is_empty(),
            "non-strict parsing does not consult the filesystem"
        );

        let strict = PackageInfo::from_text(true, true, test_path.to_str().unwrap()).unwrap();
        assert_eq!(
            strict.loader_paths(),
            [
                "loader-paths/loader/",
                "loader-paths/nested/dir/marker.toml"
            ]
        );
        assert_eq!(
            strict.errors().iter().map(Error::msg).collect::<Vec<_>>(),
            vec![
                "include_path loader-paths/missing/ does not exist",
                "include_path loader-paths/loader is malformed: paths must start with // and cannot include ./ or ../, directories must end with /",
                "package path //loader-paths/nested/dir/marker.toml/ must be a directory",
            ]
        );
    }

    #[test]
    fn test_loader_paths_must_not_nest() {
        let test_path = SRCDIR
            .as_path()
            .join("tests/package-loader-paths-nested.toml");
        let info = PackageInfo::from_text(false, true, test_path.to_str().unwrap()).unwrap();

        assert_eq!(
            info.loader_paths(),
            ["loader/", "loose/script", "loose/script_extra/"],
            "a file entry covers only itself, so it does not contain the directory it prefixes"
        );
        assert_eq!(
            info.errors().iter().map(Error::msg).collect::<Vec<_>>(),
            vec![
                "strict_isolation.loader_paths entry //loader/nested/ is nested under entry //loader/; entries must not be nested",
                "strict_isolation.loader_paths entry //loader/deep/file.php is nested under entry //loader/; entries must not be nested",
            ]
        );
    }
}
