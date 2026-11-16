// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::borrow::Cow;
use std::collections::VecDeque;

use hack_name::is_valid_identifier;
use hash::HashSet;
use serde::Deserialize;
use toml::Spanned;

use crate::error::Error;
use crate::types::DeploymentMap;
pub use crate::types::ImplicitPackageMap;
use crate::types::NameSet;
pub use crate::types::Package;
pub use crate::types::PackageMap;
pub use crate::types::StrictIsolation;

#[derive(Debug, Deserialize)]
pub struct Config {
    pub packages: PackageMap,
    pub deployments: Option<DeploymentMap>,
    /// `[implicit_packages]` families. Empty when the section is absent.
    #[serde(default)]
    pub implicit_packages: ImplicitPackageMap,
    #[serde(default)]
    pub strict_isolation: StrictIsolation,
}

/// Splits a (possibly synthesized) package name `F.D` into its family name `F`
/// and member segment `D`, splitting on the *first* `.`. Returns `None` if there
/// is no `.` separator, or if either side is empty. Both `F` and `D` must be
/// valid Hack identifiers; `PackageInfo` reports invalid names separately.
pub(crate) fn split_member_name(name: &str) -> Option<(&str, &str)> {
    let (family, member) = name.split_once('.')?;
    if family.is_empty() || member.is_empty() {
        None
    } else {
        Some((family, member))
    }
}

impl Config {
    pub fn check_config(&self, errors: &mut Vec<Error>) {
        // Set of declared implicit-package family names, used to recognize
        // family and member references and normalize `F.D` back to `F` for
        // transitive-closure checks.
        let family_key: HashSet<&str> = self
            .implicit_packages
            .keys()
            .map(|k| k.get_ref().as_str())
            .collect();

        let check_member_names = |errors: &mut Vec<Error>, names: &Option<NameSet>| {
            let Some(names) = names else {
                return;
            };
            errors.extend(names.iter().filter_map(|name| {
                let (family, member) = name.get_ref().split_once('.')?;
                (family_key.contains(family) && !is_valid_identifier(member))
                    .then(|| Error::implicit_member_name_invalid(name, member))
            }));
        };
        // A name is "defined" if it is a hand-written package, an implicit
        // family `F`, or a synthesized member `F.D` of a declared family. The
        // member case is validated structurally (no filesystem access): we do
        // NOT verify that directory `D` currently exists.
        let name_is_defined = |name: &Spanned<String>| -> bool {
            let n = name.get_ref().as_str();
            self.packages.contains_key(name)
                || family_key.contains(n)
                || split_member_name(n).is_some_and(|(f, _)| family_key.contains(f))
        };

        let check_packages_are_defined =
            |errors: &mut Vec<Error>, pkgs: &Option<NameSet>, soft_pkgs: &Option<NameSet>| {
                if let Some(packages) = pkgs {
                    packages.iter().for_each(|package| {
                        if !name_is_defined(package) {
                            errors.push(Error::undefined_package(package))
                        }
                    })
                }
                if let Some(packages) = soft_pkgs {
                    packages.iter().for_each(|package| {
                        if !name_is_defined(package) {
                            errors.push(Error::undefined_package(package))
                        }
                    })
                }
            };
        let mut used_include_paths = NameSet::default();
        let mut check_each_include_path_is_used_once =
            |errors: &mut Vec<Error>, include_paths: &Option<NameSet>| {
                if let Some(l) = include_paths {
                    l.iter().for_each(|include_path| {
                        if used_include_paths.contains(include_path) {
                            errors.push(Error::duplicate_include_path(include_path))
                        }
                        used_include_paths.insert(include_path.clone());
                    })
                }
            };
        // Augmented map used for transitive-closure checks: the hand-written
        // packages plus synthetic nodes for implicit families and referenced
        // members. A member node keeps its exact `F.D` name while carrying the
        // relationships declared by `F`.
        let closure_map: Cow<'_, PackageMap> = if self.implicit_packages.is_empty() {
            Cow::Borrowed(&self.packages)
        } else {
            let mut augmented = self.packages.clone();
            augmented.extend(self.implicit_packages.iter().map(|(fname, fam)| {
                (
                    fname.clone(),
                    Package {
                        includes: fam.includes.clone(),
                        soft_includes: fam.soft_includes.clone(),
                        include_paths: None,
                        // Synthetic node used only for include-closure checks;
                        // the flags are irrelevant here.
                        enable_strict_isolation: false,
                        allow_deployed_packages_checking: false,
                        raise_dynamic_class_load_error: None,
                    },
                )
            }));

            let mut add_member = |name: &Spanned<String>| {
                let Some((family_name, _)) = split_member_name(name.get_ref()) else {
                    return;
                };
                let Some(family) = self.implicit_packages.get(family_name) else {
                    return;
                };
                if augmented.contains_key(name.get_ref().as_str()) {
                    return;
                }
                augmented.insert(
                    name.clone(),
                    Package {
                        includes: family.includes.clone(),
                        soft_includes: family.soft_includes.clone(),
                        include_paths: None,
                        // Synthetic node used only for include-closure checks;
                        // the flags are irrelevant here.
                        enable_strict_isolation: false,
                        allow_deployed_packages_checking: false,
                        raise_dynamic_class_load_error: None,
                    },
                );
            };
            for package in self.packages.values() {
                for name in package.includes.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
                for name in package.soft_includes.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
            }
            for deployment in self.deployments.iter().flat_map(|d| d.values()) {
                for name in deployment.packages.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
                for name in deployment.soft_packages.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
            }
            for family in self.implicit_packages.values() {
                for name in family.includes.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
                for name in family.soft_includes.as_ref().unwrap_or_default().iter() {
                    add_member(name);
                }
            }
            Cow::Owned(augmented)
        };

        let check_package_includes_are_transitively_closed =
            |errors: &mut Vec<Error>, package_name: &Spanned<String>, package: &Package| {
                let mut includes = package.includes.clone().unwrap_or_default();
                includes.insert(package_name.clone());
                let soft_includes = package.soft_includes.clone().unwrap_or_default();
                let (missing_pkgs, missing_soft_pkgs) =
                    find_missing_packages_from_transitive_closure(
                        &closure_map,
                        &includes,
                        &soft_includes,
                    );
                if !missing_pkgs.is_empty() {
                    errors.push(Error::incomplete_includes(
                        package_name,
                        missing_pkgs,
                        false,
                    ));
                }
                if !missing_soft_pkgs.is_empty() {
                    errors.push(Error::incomplete_includes(
                        package_name,
                        missing_soft_pkgs,
                        true,
                    ));
                }
            };
        let check_deployed_packages_are_transitively_closed =
            |errors: &mut Vec<Error>,
             deployment: &Spanned<String>,
             pkgs: &Option<NameSet>,
             soft_pkgs: &Option<NameSet>| {
                let deployed = pkgs.clone().unwrap_or_default();
                let soft_deployed = soft_pkgs.clone().unwrap_or_default();
                let (missing_pkgs, missing_soft_pkgs) =
                    find_missing_packages_from_transitive_closure(
                        &closure_map,
                        &deployed,
                        &soft_deployed,
                    );
                if !missing_pkgs.is_empty() {
                    errors.push(Error::incomplete_deployment(
                        deployment,
                        missing_pkgs,
                        false,
                    ));
                }
                if !missing_soft_pkgs.is_empty() {
                    errors.push(Error::incomplete_deployment(
                        deployment,
                        missing_soft_pkgs,
                        true,
                    ));
                }
            };
        for (package_name, package) in self.packages.iter() {
            if !is_valid_identifier(package_name.get_ref()) {
                errors.push(Error::package_name_invalid(package_name));
            }
            if package.enable_strict_isolation && package.allow_deployed_packages_checking {
                errors.push(Error::contradictory_observability_flags(package_name));
            }
            if package.raise_dynamic_class_load_error == Some(true)
                && !package.enable_strict_isolation
            {
                errors.push(Error::dynamic_class_load_error_requires_strict_isolation(
                    package_name,
                ));
            }
            check_member_names(errors, &package.includes);
            check_member_names(errors, &package.soft_includes);
            check_packages_are_defined(errors, &package.includes, &package.soft_includes);
            check_each_include_path_is_used_once(errors, &package.include_paths);
            check_package_includes_are_transitively_closed(errors, package_name, package);
        }
        if let Some(deployments) = &self.deployments {
            for (positioned_name, deployment) in deployments.iter() {
                check_member_names(errors, &deployment.packages);
                check_member_names(errors, &deployment.soft_packages);
                check_packages_are_defined(errors, &deployment.packages, &deployment.soft_packages);
                check_deployed_packages_are_transitively_closed(
                    errors,
                    positioned_name,
                    &deployment.packages,
                    &deployment.soft_packages,
                );
            }
        };

        // Validate the implicit-package families themselves. Note these checks
        // are purely structural / textual -- none of them reads the filesystem,
        // so parsing remains a pure function of PACKAGES.toml's contents.
        for (fname, fam) in self.implicit_packages.iter() {
            if !is_valid_identifier(fname.get_ref()) {
                errors.push(Error::implicit_family_name_invalid(fname));
            }
            check_member_names(errors, &fam.includes);
            check_member_names(errors, &fam.soft_includes);

            // (0) `include_paths` is not permitted on a family stanza.
            if fam.include_paths.is_some() {
                errors.push(Error::implicit_include_paths_not_allowed(fname));
            }

            // (1) A broader explicit path may contain a family. An explicit
            // path at or below the family would take precedence and is invalid.
            let fpath = fam.path.get_ref().as_str();
            let overlaps = |other: &str| fpath.starts_with(other) || other.starts_with(fpath);
            for (pname, package) in self.packages.iter() {
                // One error per conflicting package, not per include_path.
                let overlapping = package
                    .include_paths
                    .iter()
                    .flat_map(|ips| ips.iter())
                    .find(|ip| ip.get_ref().starts_with(fpath));
                if let Some(ip) = overlapping {
                    errors.push(Error::overlapping_implicit_path(
                        fpath.to_owned(),
                        format!(
                            "include_path //{} of package {}",
                            ip.get_ref(),
                            pname.get_ref()
                        ),
                        fam.path.span(),
                    ));
                }
            }
            // Family roots cannot overlap because neither family can win.
            for (other_name, other_fam) in self.implicit_packages.iter() {
                // Compare each unordered pair once; skip self.
                if other_name.get_ref() <= fname.get_ref() {
                    continue;
                }
                let opath = other_fam.path.get_ref().as_str();
                if overlaps(opath) {
                    errors.push(Error::overlapping_implicit_path(
                        fpath.to_owned(),
                        format!(
                            "path //{} of implicit_packages family {}",
                            opath,
                            other_name.get_ref()
                        ),
                        fam.path.span(),
                    ));
                }
            }

            // (2) A family name must not collide with, or namespace-shadow, any
            // hand-written package name (`F` itself or anything under `F.`).
            let f = fname.get_ref().as_str();
            let dotted = format!("{}.", f);
            for pname in self.packages.keys() {
                let p = pname.get_ref().as_str();
                if p == f || p.starts_with(&dotted) {
                    errors.push(Error::package_name_prefix_collision(fname, pname));
                }
            }

            // (3) A family's own includes must be transitively closed, exactly
            // as for a hand-written package. Every member shares these includes,
            // so checking the family once suffices for all (current and future)
            // members.
            check_packages_are_defined(errors, &fam.includes, &fam.soft_includes);
            check_package_includes_are_transitively_closed(
                errors,
                fname,
                &Package {
                    includes: fam.includes.clone(),
                    soft_includes: fam.soft_includes.clone(),
                    include_paths: None,
                    // Synthetic node used only for include-closure checks; the
                    // flags are irrelevant here.
                    enable_strict_isolation: false,
                    allow_deployed_packages_checking: false,
                    raise_dynamic_class_load_error: None,
                },
            );
        }
    }
}

// The function takes a starting set of package names and a PackageMap, and
// returns two HashSets: one with packages included by the starting set (transitive closure),
// and the other with packages soft-included by the starting set (transitive closure) but not included.
fn analyze_includes<'a>(
    starting_set: &'a NameSet,
    package_map: &'a PackageMap,
) -> (HashSet<&'a Spanned<String>>, HashSet<&'a Spanned<String>>) {
    // Sets to store packages that are included and soft-included, respectively
    let mut included = HashSet::default();
    let mut soft_included = HashSet::default();

    // Queue of package names and whether the package is soft included
    let mut queue = VecDeque::new();

    // Add the starting set of package names to the queue, with 'is_soft' flag set to false
    for package_name in starting_set.iter() {
        queue.push_back((package_name, false));
    }

    while let Some((current_package_name, is_soft)) = queue.pop_front() {
        if let Some(package) = package_map.get(current_package_name) {
            // If the package is not soft-included, add it to the 'included' set
            if !is_soft {
                included.insert(current_package_name);
            } else if !included.contains(current_package_name) {
                // If the package is soft-included and not in the 'included' set, add it to the 'soft_included' set
                soft_included.insert(current_package_name);
            }

            if let Some(ref includes) = package.includes {
                for include in includes.iter() {
                    if !included.contains(include) {
                        queue.push_back((include, false));
                    }
                }
            }

            if let Some(ref soft_includes) = package.soft_includes {
                for soft_include in soft_includes.iter() {
                    if !included.contains(soft_include) && !soft_included.contains(soft_include) {
                        queue.push_back((soft_include, true));
                    }
                }
            }
        }
    }
    (included, soft_included)
}

fn find_missing_packages_from_transitive_closure(
    package_map: &PackageMap,
    regular: &NameSet,
    soft: &NameSet,
) -> (Vec<Spanned<String>>, Vec<Spanned<String>>) {
    // Taking the transitive closure of all nested included packages so that the user
    // could complete the input set upon the first error message they receive, as
    // opposed to iterating upon it over multiple checks and going through a full init
    // every time they update the config.
    // TODO: simplify after incremental mode (T148526825)

    let (included, soft_included) = analyze_includes(regular, package_map);
    let soft_or_regular = regular.union(soft).cloned().collect();

    fn get_missing<'a>(
        included: &HashSet<&'a Spanned<String>>,
        set: &NameSet,
        package_map: &PackageMap,
    ) -> Vec<Spanned<String>> {
        let mut missing_pkgs = included
            .iter()
            .filter_map(|pkg| {
                let pkg_name = pkg.get_ref().as_str();
                if !set.contains(pkg_name) {
                    let (positioned_pkg_name, _) = package_map.get_key_value(pkg_name).unwrap();
                    Some(positioned_pkg_name.clone())
                } else {
                    None
                }
            })
            .collect::<Vec<_>>();
        missing_pkgs.sort();
        missing_pkgs
    }
    (
        get_missing(&included, regular, package_map),
        get_missing(&soft_included, &soft_or_regular, package_map),
    )
}
