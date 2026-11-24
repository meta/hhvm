// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use crate::pos::Pos;
use crate::typing_reason::*;

impl WitnessDecl {
    pub fn pos(&self) -> &Pos {
        match self {
            Self::WitnessFromDecl(pos_or_decl)
            | Self::IdxVectorFromDecl(pos_or_decl)
            | Self::IdxStringFromDecl(pos_or_decl)
            | Self::RetFunKindFromDecl(pos_or_decl, _)
            | Self::Hint(pos_or_decl)
            | Self::ClassClass(pos_or_decl, _)
            | Self::VarParamFromDecl(pos_or_decl)
            | Self::InoutParam(pos_or_decl)
            | Self::TconstNoCstr((pos_or_decl, _))
            | Self::VarrayOrDarrayKey(pos_or_decl)
            | Self::VecOrDictKey(pos_or_decl)
            | Self::MissingOptionalField(pos_or_decl, _)
            | Self::ImplicitUpperBound(pos_or_decl, _)
            | Self::GlobalTypeVariableGenerics(pos_or_decl, _, _)
            | Self::SolveFail(pos_or_decl)
            | Self::CstrOnGenerics(pos_or_decl, _)
            | Self::Enforceable(pos_or_decl)
            | Self::GlobalClassProp(pos_or_decl)
            | Self::GlobalFunParam(pos_or_decl)
            | Self::GlobalFunRet(pos_or_decl)
            | Self::DefaultCapability(pos_or_decl)
            | Self::SupportDynamicType(pos_or_decl)
            | Self::PessimisedInout(pos_or_decl)
            | Self::PessimisedReturn(pos_or_decl, _)
            | Self::PessimisedProp(pos_or_decl, _)
            | Self::PessimisedThis(pos_or_decl)
            | Self::IllegalRecursiveType(pos_or_decl, _)
            | Self::SupportDynamicTypeAssume(pos_or_decl)
            | Self::TupleFromSplat(pos_or_decl)
            | Self::PolymorphicTypeParam(pos_or_decl, _, _, _)
            | Self::EnforcedType(pos_or_decl) => pos_or_decl,
        }
    }
}

impl Reason {
    pub fn pos(&self) -> Option<&Pos> {
        use T_::*;
        match self {
            NoReason | Invalid => None,
            FromWitnessDecl(witness) => Some(witness.pos()),
            Typeconst(deref!(NoReason), (pos, _), _, _) => Some(pos),
            Typeconst(t, _, _, _) | ExprDepType(t, _, _) | Instantiate { type__: t, .. } => t.pos(),
        }
    }
}
