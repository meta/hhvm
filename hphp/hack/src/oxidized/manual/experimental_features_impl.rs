// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::str::FromStr;

use parser_core_types::syntax_error;

use crate::r#gen::experimental_features::FeatureName;
use crate::r#gen::experimental_features::FeatureName::*;
use crate::r#gen::experimental_features::FeatureStatus;
use crate::r#gen::experimental_features::FeatureStatus::*;
use crate::parser_options::ParserOptions;

impl FeatureName {
    pub fn get_feature_status_deprecated(&self) -> FeatureStatus {
        match self {
            UnionIntersectionTypeHints => Unstable,
            ExpressionTrees => Unstable,
            Readonly => Preview,
            ModuleReferences => Unstable,
            ContextAliasDeclaration => Unstable,
            ContextAliasDeclarationShort => Preview,
            TypeConstMultipleBounds => OngoingRelease,
            TypeConstSuperBound => OngoingRelease,
            ClassConstDefault => Migration,
            UpcastExpression => Unstable,
            RequireConstraint => OngoingRelease,
            NewtypeSuperBounds => Unstable,
            CaseTypes => OngoingRelease,
            CaseTypeWhereClauses => Preview,
            RecursiveCaseTypes => Preview,
            ModuleLevelTraits => OngoingRelease,
            ModuleLevelTraitsExtensions => OngoingRelease,
            PipeAwait => OngoingRelease,
            MatchStatements => Unstable,
            StrictSwitch => Unstable,
            ClassType => OngoingRelease,
            FunctionReferences => Unstable,
            FunctionTypeOptionalParams => OngoingRelease,
            AwaitInSplice => OngoingRelease,
            OpenTuples => OngoingRelease,
            TypeSplat => OngoingRelease,
            ExpressionTreeNestedBindings => OngoingRelease,
            LikeTypeHints => Unstable,
            ShapeAndTupleDestructuring => Unstable,
            ExpressionTreeShapeCreation => OngoingRelease,
            NoDisjointUnion => OngoingRelease,
            SimpliHack => Preview,
            PolymorphicFunctionHints => OngoingRelease,
            PolymorphicLambda => OngoingRelease,
            ExpressionTreeCoalesceOperator => OngoingRelease,
            ExpressionTreeNullsafeObjGet => OngoingRelease,
            NamedParameters => Preview,
            XhpTypeConstants => OngoingRelease,
            CapturePipeVariables => Unstable,
            AllowExtendedAwaitSyntax => Unstable,
            AllowConditionalAwaitSyntax => Unstable,
            ExpressionTreeHackArrays => OngoingRelease,
            ExpressionTreeSubscript => OngoingRelease,
            ShapeFieldPunning => Unstable,
            RepresentableAs => Preview,
            ClassAliasesEverywhere => Unstable,
            TestFeature => Unstable,
            WithRefinementAlias => Preview,
            ShapeSplatConcrete => OngoingRelease,
            ShapeSplatTypeParameters => OngoingRelease,
            ShapeSplatExpression => OngoingRelease,
            NamedParametersMethCaller => Unstable,
        }
    }

    pub fn parse_experimental_feature(
        (name, status): (String, String),
    ) -> Result<(String, FeatureStatus), anyhow::Error> {
        let _n = FeatureName::from_str(&name).map_err(|_| ExperimentalFeatureError {
            kind: "name".to_string(),
            bad_name: name.to_string(),
        })?;
        let config_status =
            FeatureStatus::from_str(&status).map_err(|_| ExperimentalFeatureError {
                kind: "status".to_string(),
                bad_name: status.to_string(),
            })?;
        Ok((name, config_status))
    }

    pub fn get_feature_status(&self, po: &ParserOptions) -> FeatureStatus {
        if po.use_legacy_experimental_feature_config {
            self.get_feature_status_deprecated()
        } else {
            po.experimental_features
                .get(&self.to_string())
                .unwrap_or_else(|| get_unspecified_feature(po))
                .clone()
        }
    }

    // Experimental features with an ongoing release should be allowed
    pub fn can_use(
        &self,
        po: &ParserOptions,
        active_experimental_features: &hash::HashSet<FeatureName>,
    ) -> bool {
        (match self {
            UnionIntersectionTypeHints => po.union_intersection_type_hints,
            _ => false,
        }) || active_experimental_features.contains(self)
            || matches!(self.get_feature_status(po), OngoingRelease)
    }

    pub fn enable(
        &self,
        po: &ParserOptions,
        is_hhi: bool,
        active_experimental_features: &mut hash::HashSet<FeatureName>,
        text: &str,
        error: &mut dyn FnMut(syntax_error::Error),
    ) {
        if po.allow_unstable_features
            || is_hhi
            || !matches!(self.get_feature_status(po), FeatureStatus::Unstable)
        {
            active_experimental_features.insert(self.clone());
        } else {
            error(syntax_error::cannot_enable_unstable_feature(
                format!("{} is unstable and unstable features are disabled", text).as_str(),
            ))
        }
    }
}

fn get_unspecified_feature(po: &ParserOptions) -> &FeatureStatus {
    if po.consider_unspecified_experimental_features_released {
        &OngoingRelease
    } else {
        &Unstable
    }
}

#[derive(Debug)]
struct ExperimentalFeatureError {
    // the name of the experimental feature with the error
    bad_name: String,
    // a description of the error
    kind: String,
}

impl std::fmt::Display for ExperimentalFeatureError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Invalid experimental feature {}: {}",
            self.kind, self.bad_name
        )
    }
}

impl std::error::Error for ExperimentalFeatureError {}
