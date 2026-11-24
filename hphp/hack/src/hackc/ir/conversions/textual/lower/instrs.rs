// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use ir::BareThisOp;
use ir::Call;
use ir::FCallArgsFlags;
use ir::Func;
use ir::FuncBuilder;
use ir::FuncBuilderEx as _;
use ir::GlobalId;
use ir::Immediate;
use ir::InitPropOp;
use ir::Instr;
use ir::InstrId;
use ir::IrRepr;
use ir::IsTypeOp;
use ir::LocId;
use ir::LocalId;
use ir::MemberOpBuilder;
use ir::MethodName;
use ir::ObjMethodOp;
use ir::PropName;
use ir::ReadonlyOp;
use ir::SetOpOp;
use ir::SpecialClsRef;
use ir::StringId;
use ir::TypeStructEnforceKind;
use ir::TypeStructResolveOp;
use ir::ValueId;
use ir::VerifyRetKind;
use ir::func_builder::TransformInstr;
use ir::func_builder::TransformState;
use ir::instr::CallDetail;
use ir::instr::CmpOp;
use ir::instr::HasLoc;
use ir::instr::HasLocals;
use ir::instr::HasOperands;
use ir::instr::Hhbc;
use ir::instr::IteratorArgs;
use ir::instr::MemberOp;
use ir::instr::MemoGet;
use ir::instr::MemoGetEager;
use ir::instr::Predicate;
use ir::instr::Special;
use ir::instr::Terminator;
use ir::instr::Textual;
use ir::type_struct::TypeStruct;
use itertools::Itertools;
use naming_special_names_rust::special_idents;

use super::func_builder::FuncBuilderEx as _;
use crate::class::IsStatic;
use crate::func::FuncInfo;
use crate::func::lookup_constant;
use crate::func::lookup_constant_string;
use crate::hack;

/// Lower individual Instrs in the Func to simpler forms.
pub(crate) fn lower_instrs(builder: &mut FuncBuilder, func_info: &FuncInfo<'_>) {
    let mut lowerer = LowerInstrs {
        changed: false,
        func_info,
    };

    let mut bid = IrRepr::ENTRY_BID;
    while bid.0 < builder.func.repr.blocks.len() as u32 {
        lowerer.changed = true;
        while lowerer.changed {
            // The lowered instructions may have emitted stuff that needs to be
            // lowered again. This could be optimized by only setting `changed`
            // when we think we need to loop.
            lowerer.changed = false;
            builder.rewrite_block(bid, &mut lowerer);
        }
        bid.0 += 1;
    }
}

struct LowerInstrs<'a> {
    changed: bool,
    func_info: &'a FuncInfo<'a>,
}

impl LowerInstrs<'_> {
    fn c_get_g(&self, builder: &mut FuncBuilder, vid: ValueId, _loc: LocId) -> Option<Instr> {
        // A lot of times the name for the global comes from a static
        // string name - see if we can dig it up and turn this into a
        // Textual::LoadGlobal.
        vid.imm().and_then(|cid| match builder.func.repr.imm(cid) {
            Immediate::String(s) => {
                match ir::StringId::from_bytes(*s) {
                    Ok(s) => {
                        let id = ir::GlobalId::new(s);
                        Some(Instr::Special(Special::Textual(Textual::LoadGlobal {
                            id,
                            is_const: false,
                        })))
                    }
                    Err(_) => {
                        // Non-utf8 string value cannot be a symbol name
                        None
                    }
                }
            }
            _ => None,
        })
    }

    fn handle_hhbc_with_builtin(&self, hhbc: &Hhbc) -> Option<hack::Hhbc> {
        let builtin = match hhbc {
            Hhbc::Add(..) => hack::Hhbc::Add,
            Hhbc::AddElemC(..) => hack::Hhbc::AddElemC,
            Hhbc::AddNewElemC(..) => hack::Hhbc::AddNewElemC,
            Hhbc::Await(..) => hack::Hhbc::Await,
            Hhbc::AwaitAll(..) => hack::Hhbc::AwaitAll,
            Hhbc::BitAnd(..) => hack::Hhbc::BitAnd,
            Hhbc::BitOr(..) => hack::Hhbc::BitOr,
            Hhbc::BitXor(..) => hack::Hhbc::BitXor,
            Hhbc::CastBool(..) => hack::Hhbc::CastBool,
            Hhbc::CastDict(..) => hack::Hhbc::CastDict,
            Hhbc::CastDouble(..) => hack::Hhbc::CastDouble,
            Hhbc::CastInt(..) => hack::Hhbc::CastInt,
            Hhbc::CastKeyset(..) => hack::Hhbc::CastKeyset,
            Hhbc::CastString(..) => hack::Hhbc::CastString,
            Hhbc::CastVec(..) => hack::Hhbc::CastVec,
            Hhbc::ChainFaults(..) => hack::Hhbc::ChainFaults,
            Hhbc::CheckClsRGSoft(..) => hack::Hhbc::CheckClsRGSoft,
            Hhbc::ClassGetC(..) => hack::Hhbc::ClassGetC,
            Hhbc::CmpOp(_, CmpOp::Eq, _) => hack::Hhbc::CmpEq,
            Hhbc::CmpOp(_, CmpOp::Gt, _) => hack::Hhbc::CmpGt,
            Hhbc::CmpOp(_, CmpOp::Gte, _) => hack::Hhbc::CmpGte,
            Hhbc::CmpOp(_, CmpOp::Lt, _) => hack::Hhbc::CmpLt,
            Hhbc::CmpOp(_, CmpOp::Lte, _) => hack::Hhbc::CmpLte,
            Hhbc::CmpOp(_, CmpOp::NSame, _) => hack::Hhbc::CmpNSame,
            Hhbc::CmpOp(_, CmpOp::Neq, _) => hack::Hhbc::CmpNeq,
            Hhbc::CmpOp(_, CmpOp::Same, _) => hack::Hhbc::CmpSame,
            Hhbc::CombineAndResolveTypeStruct(..) => hack::Hhbc::CombineAndResolveTypeStruct,
            Hhbc::Concat(..) => hack::Hhbc::Concat,
            Hhbc::ConcatN(..) => hack::Hhbc::ConcatN,
            Hhbc::Div(..) => hack::Hhbc::Div,
            Hhbc::GetClsRGProp(..) => hack::Hhbc::GetClsRGProp,
            Hhbc::GetMemoKeyL(..) => hack::Hhbc::GetMemoKeyL,
            Hhbc::Idx(..) => hack::Hhbc::Idx,
            Hhbc::IsLateBoundCls(..) => hack::Hhbc::IsLateBoundCls,
            Hhbc::IsTypeC(_, IsTypeOp::ArrLike, _) => hack::Hhbc::IsTypeArrLike,
            Hhbc::IsTypeC(_, IsTypeOp::Bool, _) => hack::Hhbc::IsTypeBool,
            Hhbc::IsTypeC(_, IsTypeOp::Class, _) => hack::Hhbc::IsTypeClass,
            Hhbc::IsTypeC(_, IsTypeOp::ClsMeth, _) => hack::Hhbc::IsTypeClsMeth,
            Hhbc::IsTypeC(_, IsTypeOp::Dbl, _) => hack::Hhbc::IsTypeDbl,
            Hhbc::IsTypeC(_, IsTypeOp::Dict, _) => hack::Hhbc::IsTypeDict,
            Hhbc::IsTypeC(_, IsTypeOp::Func, _) => hack::Hhbc::IsTypeFunc,
            Hhbc::IsTypeC(_, IsTypeOp::Int, _) => hack::Hhbc::IsTypeInt,
            Hhbc::IsTypeC(_, IsTypeOp::Keyset, _) => hack::Hhbc::IsTypeKeyset,
            Hhbc::IsTypeC(_, IsTypeOp::LegacyArrLike, _) => hack::Hhbc::IsTypeLegacyArrLike,
            Hhbc::IsTypeC(_, IsTypeOp::Null, _) => hack::Hhbc::IsTypeNull,
            Hhbc::IsTypeC(_, IsTypeOp::Obj, _) => hack::Hhbc::IsTypeObj,
            Hhbc::IsTypeC(_, IsTypeOp::Res, _) => hack::Hhbc::IsTypeRes,
            Hhbc::IsTypeC(_, IsTypeOp::Scalar, _) => hack::Hhbc::IsTypeScalar,
            Hhbc::IsTypeC(_, IsTypeOp::Str, _) => hack::Hhbc::IsTypeStr,
            Hhbc::IsTypeC(_, IsTypeOp::Vec, _) => hack::Hhbc::IsTypeVec,
            Hhbc::IterBase(..) => hack::Hhbc::IterBase,
            Hhbc::Modulo(..) => hack::Hhbc::Modulo,
            Hhbc::Mul(..) => hack::Hhbc::Mul,
            Hhbc::NewDictArray(..) => hack::Hhbc::NewDictArray,
            Hhbc::NewKeysetArray(..) => hack::Hhbc::NewKeysetArray,
            Hhbc::NewVec(..) => hack::Hhbc::NewVec,
            Hhbc::Not(..) => hack::Hhbc::Not,
            Hhbc::Pow(..) => hack::Hhbc::Pow,
            Hhbc::Print(..) => hack::Hhbc::Print,
            Hhbc::RecordReifiedGeneric(..) => hack::Hhbc::RecordReifiedGeneric,
            Hhbc::SetImplicitContextByValue(..) => hack::Hhbc::SetImplicitContextByValue,
            Hhbc::GetMemoAgnosticImplicitContext(..) => hack::Hhbc::GetMemoAgnosticImplicitContext,
            Hhbc::GetWholeImplicitContext(..) => hack::Hhbc::GetWholeImplicitContext,
            Hhbc::Shl(..) => hack::Hhbc::Shl,
            Hhbc::Shr(..) => hack::Hhbc::Shr,
            Hhbc::Sub(..) => hack::Hhbc::Sub,
            Hhbc::ThrowNonExhaustiveSwitch(..) => hack::Hhbc::ThrowNonExhaustiveSwitch,
            Hhbc::WHResult(..) => hack::Hhbc::WHResult,
            _ => return None,
        };
        Some(builtin)
    }

    fn handle_terminator_with_builtin(&self, term: &Terminator) -> Option<hack::Hhbc> {
        let builtin = match term {
            Terminator::Exit(..) => hack::Hhbc::Exit,
            Terminator::ThrowAsTypeStructException(..) => hack::Hhbc::ThrowAsTypeStructException,
            _ => return None,
        };
        Some(builtin)
    }

    fn handle_with_builtin(&self, builder: &mut FuncBuilder, instr: &Instr) -> Option<Instr> {
        let loc = instr.loc_id();
        match instr {
            Instr::Hhbc(hhbc) => {
                let hhbc = self.handle_hhbc_with_builtin(hhbc)?;
                Some(
                    match (instr.locals().is_empty(), instr.operands().is_empty()) {
                        (true, true) => builder.hhbc_builtin(hhbc, &[], loc),
                        (true, false) => builder.hhbc_builtin(hhbc, instr.operands(), loc),
                        (false, true) => {
                            let ops = Self::get_locals(builder, instr.locals(), loc);
                            builder.hhbc_builtin(hhbc, &ops, loc)
                        }
                        (false, false) => panic!(
                            "Unable to handle mixed instruction (locals and operands) as a built-in: {hhbc:?}"
                        ),
                    },
                )
            }
            Instr::MemberOp(member_op) => self.handle_member_op(builder, member_op),
            Instr::Terminator(term) => {
                let hhbc = self.handle_terminator_with_builtin(term)?;
                builder.emit_hhbc_builtin(hhbc, instr.operands(), loc);
                Some(Instr::unreachable())
            }
            _ => None,
        }
    }

    fn handle_member_op(&self, builder: &mut FuncBuilder, member_op: &MemberOp) -> Option<Instr> {
        use ir::instr::BaseOp;

        match member_op.base_op {
            BaseOp::BaseSC {
                mode,
                readonly,
                loc,
            } => {
                let mut operands = member_op.operands.iter().copied();
                let locals = member_op.locals.iter().copied();
                let intermediates = member_op.intermediate_ops.iter().cloned();

                let prop = operands.next().unwrap();
                let base = operands.next().unwrap();
                if let Some(propname) = lookup_constant_string(&builder.func, prop) {
                    // Convert BaseSC w/ constant string to BaseST
                    let pid = PropName::from_bytes(propname).expect("non-utf8 property name");
                    let mut mop = MemberOpBuilder::base_st(base, pid, mode, readonly, loc);
                    mop.operands.extend(operands);
                    mop.locals.extend(locals);
                    mop.intermediate_ops.extend(intermediates);
                    return Some(Instr::MemberOp(mop.final_op(member_op.final_op.clone())));
                }
            }
            _ => {}
        }

        None
    }

    fn check_prop(&self, builder: &mut FuncBuilder, _: PropName, _: LocId) -> Instr {
        // CheckProp checks to see if the prop has already been initialized - we'll just always say "no".
        Instr::copy(builder.emit_imm(Immediate::Bool(false)))
    }

    fn init_prop(
        &self,
        builder: &mut FuncBuilder,
        vid: ValueId,
        pid: PropName,
        _op: InitPropOp,
        loc: LocId,
    ) -> Instr {
        // $this->pid = vid
        MemberOpBuilder::base_h(loc).emit_set_m_pt(builder, pid, vid);
        Instr::tombstone()
    }

    fn iter_init(&self, builder: &mut FuncBuilder, args: IteratorArgs) -> Instr {
        // iterator ^0 init from $base jmp to b2 else b1 with $index
        // ->
        // %n = hack::iter_init(&iter0, /* key */ null, &$index, $base)
        // if %n jmp b1 else b2

        let loc = args.loc;
        let iter_lid = iter_var_name(args.iter_id);

        let base_var = builder.emit(Instr::Hhbc(Hhbc::CGetL(args.base_lid, loc)));

        let iter_var = builder.emit(Textual::deref(iter_lid));

        let pred = builder.emit_hhbc_builtin(hack::Hhbc::IterInit, &[iter_var, base_var], loc);

        Instr::jmp_op(
            pred,
            Predicate::NonZero,
            args.next_bid(),
            args.done_bid(),
            loc,
        )
    }

    fn iter_next(&self, builder: &mut FuncBuilder, args: IteratorArgs) -> Instr {
        // iterator ^0 next from $base jmp to b2 else b1 with $index
        // ->
        // %n = hack::iter_next(&iter0, /* key */ null, &$index, $base)
        // if %n jmp b1 else b2

        let loc = args.loc;
        let iter_lid = iter_var_name(args.iter_id);

        let base_var = builder.emit(Instr::Hhbc(Hhbc::CGetL(args.base_lid, loc)));

        let iter_var = builder.emit(Instr::Hhbc(Hhbc::CGetL(iter_lid, loc)));
        let pred = builder.emit_hhbc_builtin(hack::Hhbc::IterNext, &[iter_var, base_var], loc);

        Instr::jmp_op(
            pred,
            Predicate::NonZero,
            args.next_bid(),
            args.done_bid(),
            loc,
        )
    }

    fn get_locals(builder: &mut FuncBuilder, locals: &[LocalId], loc: LocId) -> Vec<ValueId> {
        locals
            .iter()
            .map(|lid| builder.emit(Instr::Hhbc(Hhbc::CGetL(*lid, loc))))
            .collect()
    }

    fn compute_memo_ops(
        &self,
        builder: &mut FuncBuilder,
        locals: &[LocalId],
        loc: LocId,
    ) -> Vec<ValueId> {
        let name = match *self.func_info {
            FuncInfo::Method(ref mi) => {
                crate::mangle::FunctionName::method(mi.class.name, mi.is_static, mi.name)
            }
            FuncInfo::Function(ref fi) => crate::mangle::FunctionName::Function(fi.name),
        };

        let mut ops = Vec::new();

        let name = format!("{}", name.display());
        let name = GlobalId::new(ir::intern(format!(
            "memocache::{}",
            crate::util::escaped_ident(name.into())
        )));
        // NOTE: This is called 'LocalId' but actually represents a GlobalId
        // (because we don't have a DerefGlobal instr - but it would work the
        // same with just a different type).
        let global = builder.emit(Textual::deref(LocalId::Named(name.id)));
        ops.push(global);

        match *self.func_info {
            FuncInfo::Method(_) => {
                let this = ir::intern(special_idents::THIS);
                let lid = LocalId::Named(this);
                ops.push(builder.emit(Instr::Hhbc(Hhbc::CGetL(lid, loc))));
            }
            FuncInfo::Function(_) => {}
        };

        ops.extend(Self::get_locals(builder, locals, loc));
        ops
    }

    fn memo_get(&self, builder: &mut FuncBuilder, memo_get: MemoGet) -> Instr {
        // memo_get([$a, $b], b1, b2)
        // ->
        // global NAME
        // if memo_isset($this, $a, $b) {
        //   tmp = memo_get($this, $a, $b)
        //   goto b1(tmp);
        // } else {
        //   goto b2;
        // }
        let loc = memo_get.loc;

        let ops = self.compute_memo_ops(builder, &memo_get.locals, loc);

        let pred = builder.emit_hack_builtin(hack::Builtin::MemoIsset, &ops, loc);
        builder.emit_if_then_else(
            pred,
            loc,
            |builder| {
                let value = builder.emit_hack_builtin(hack::Builtin::MemoGet, &ops, loc);
                Instr::jmp_args(memo_get.edges[0], &[value], loc)
            },
            |_| Instr::jmp(memo_get.edges[1], loc),
        );

        Instr::tombstone()
    }

    fn memo_get_eager(&self, builder: &mut FuncBuilder, mge: MemoGetEager) -> Instr {
        // MemoGetEager is the async version of MemoGet.  Ignore the async edge
        // and just treat it as a MemoGet.
        self.memo_get(
            builder,
            MemoGet::new(mge.eager_edge(), mge.no_value_edge(), &mge.locals, mge.loc),
        )
    }

    fn memo_set(
        &self,
        builder: &mut FuncBuilder,
        vid: ValueId,
        locals: &[LocalId],
        loc: LocId,
    ) -> Instr {
        let mut ops = self.compute_memo_ops(builder, locals, loc);
        ops.push(vid);
        builder.hhbc_builtin(hack::Hhbc::MemoSet, &ops, loc)
    }

    fn memo_set_eager(
        &self,
        builder: &mut FuncBuilder,
        vid: ValueId,
        locals: &[LocalId],
        loc: LocId,
    ) -> Instr {
        // MemoSetEager is the async version of MemoSet. Ignore the async edge
        // and just treat it as a MemoSet.
        self.memo_set(builder, vid, locals, loc)
    }

    fn verify_out_type(
        &self,
        builder: &mut FuncBuilder,
        obj: ValueId,
        lid: LocalId,
        loc: LocId,
    ) -> Instr {
        let param = builder
            .func
            .repr
            .get_param_by_lid(lid)
            .expect("Unknown parameter in verify_out_type()");
        let param_type = ir::EnforceableType::from_type_info(&param.ty());
        let pred = builder.emit_is(obj, &param_type, loc);
        builder.emit_hack_builtin(hack::Builtin::VerifyTypePred, &[obj, pred], loc);
        Instr::copy(obj)
    }

    fn verify_param_type_ts(
        &self,
        builder: &mut FuncBuilder,
        lid: LocalId,
        ts: ValueId,
        loc: LocId,
    ) -> Instr {
        let obj = builder.emit(Instr::Hhbc(Hhbc::CGetL(lid, loc)));
        let builtin = hack::Hhbc::VerifyParamTypeTS;
        builder.emit_hhbc_builtin(builtin, &[obj, ts], loc);
        Instr::tombstone()
    }

    fn get_verify_kind(&self, func: &Func, kind: VerifyRetKind) -> VerifyRetKind {
        match kind {
            VerifyRetKind::None => kind,
            VerifyRetKind::All => {
                let return_type = ir::EnforceableType::from_type_info(&func.return_type());
                if return_type
                    .modifiers
                    .contains(ir::TypeConstraintFlags::TypeVar)
                    || return_type.ty == ir::BaseType::Noreturn
                {
                    return VerifyRetKind::None;
                }
                kind
            }
            _ => unreachable!(),
        }
    }

    fn verify_ret_type_ts(
        &self,
        builder: &mut FuncBuilder,
        obj: ValueId,
        ts: ValueId,
        loc: LocId,
    ) -> Instr {
        let builtin = hack::Hhbc::VerifyParamTypeTS;
        builder.emit_hhbc_builtin(builtin, &[obj, ts], loc);
        Instr::copy(obj)
    }

    fn verify_type_ts(
        &self,
        builder: &mut FuncBuilder,
        obj: ValueId,
        ts: ValueId,
        loc: LocId,
    ) -> Instr {
        let builtin = hack::Hhbc::VerifyTypeTS;
        builder.emit_hhbc_builtin(builtin, &[obj, ts], loc);
        Instr::copy(obj)
    }

    fn emit_special_cls_ref(
        &mut self,
        builder: &mut FuncBuilder,
        clsref: SpecialClsRef,
        loc: LocId,
    ) -> ValueId {
        match clsref {
            SpecialClsRef::SelfCls => builder.emit(Instr::Hhbc(Hhbc::SelfCls(loc))),
            SpecialClsRef::LateBoundCls => builder.emit(Instr::Hhbc(Hhbc::LateBoundCls(loc))),
            SpecialClsRef::ParentCls => builder.emit(Instr::Hhbc(Hhbc::ParentCls(loc))),
            _ => unreachable!(),
        }
    }

    fn set_s(
        &mut self,
        builder: &mut FuncBuilder,
        field: ValueId,
        class: ValueId,
        vid: ValueId,
        _readonly: ReadonlyOp,
        loc: LocId,
    ) -> Option<Instr> {
        // Some magic - if the class points to a ClassGetC with a
        // constant parameter and the field is a constant then replace
        // this with a Textual::SetPropD.
        if let Some(propname) = lookup_constant_string(&builder.func, field) {
            if let Some(class) = class.instr() {
                let class_instr = builder.func.repr.instr(class);
                // The ClassGetC will have already been converted to a
                // textual builtin.
                if let Instr::Special(Special::Textual(Textual::HackBuiltin {
                    target,
                    values: deref!([cid]),
                    loc: _,
                })) = class_instr
                {
                    if target == hack::Hhbc::ClassGetC.as_str() {
                        if let Some(classname) = lookup_constant_string(&builder.func, *cid) {
                            let cid = builder.emit_imm(Immediate::String(classname));
                            let pid = builder.emit_imm(Immediate::String(propname));
                            return Some(builder.hack_builtin(
                                hack::Builtin::SetStaticProp,
                                &[cid, pid, vid],
                                loc,
                            ));
                        }
                    }
                }
            }
        }

        None
    }
}

impl TransformInstr for LowerInstrs<'_> {
    fn apply(
        &mut self,
        iid: InstrId,
        instr: Instr,
        builder: &mut FuncBuilder,
        state: &mut TransformState,
    ) -> Instr {
        use hack::Builtin;

        if let Some(instr) = self.handle_with_builtin(builder, &instr) {
            self.changed = true;
            return instr;
        }

        let instr = match instr {
            // NOTE: Be careful trying to lower most Instr::Call - the details
            // matter and it's hard to rewrite them to preserve those details
            // properly.
            Instr::Call(
                deref!(
                    mut call @ Call {
                        detail: CallDetail::FCallCtor,
                        ..
                    }
                ),
            ) => {
                let flavor = ObjMethodOp::NullThrows;
                let method = MethodName::constructor();
                call.detail = CallDetail::FCallObjMethodD { flavor, method };
                Instr::Call(Box::new(call))
            }
            Instr::Call(
                deref!(call @ Call {
                    detail:
                        CallDetail::FCallObjMethod {
                            flavor: ObjMethodOp::NullSafe,
                            ..
                        }
                        | CallDetail::FCallObjMethodD {
                            flavor: ObjMethodOp::NullSafe,
                            ..
                        },
                    ..
                }),
            ) => rewrite_nullsafe_call(iid, builder, call, state),
            Instr::Hhbc(Hhbc::BareThis(_op, loc)) => {
                let this = ir::intern(special_idents::THIS);
                let lid = LocalId::Named(this);
                Instr::Hhbc(Hhbc::CGetL(lid, loc))
            }
            Instr::Hhbc(Hhbc::SetS([field, class, vid], readonly, loc)) => {
                if let Some(replace) = self.set_s(builder, field, class, vid, readonly, loc) {
                    replace
                } else {
                    return instr;
                }
            }
            Instr::Hhbc(Hhbc::CheckProp(pid, loc)) => self.check_prop(builder, pid, loc),
            Instr::Hhbc(Hhbc::CheckThis(loc)) => {
                let builtin = hack::Hhbc::CheckThis;
                let op = BareThisOp::NoNotice;
                let this = builder.emit(Instr::Hhbc(Hhbc::BareThis(op, loc)));
                builder.hhbc_builtin(builtin, &[this], loc)
            }
            Instr::Hhbc(Hhbc::CGetG(name, loc)) => {
                if let Some(instr) = self.c_get_g(builder, name, loc) {
                    instr
                } else {
                    return Instr::Hhbc(Hhbc::CGetG(name, loc));
                }
            }
            Instr::Hhbc(Hhbc::ClsCns(cls, const_id, loc)) => {
                let builtin = hack::Hhbc::ClsCns;
                let const_id = builder.emit_imm(Immediate::String(const_id.as_bytes_id()));
                builder.hhbc_builtin(builtin, &[cls, const_id], loc)
            }
            Instr::Hhbc(Hhbc::ClsCnsD(const_id, cid, loc)) => {
                // ClsCnsD(id, cid) -> CGetS(id, cid)
                let cid = builder.emit_imm(Immediate::String(cid.as_bytes_id()));
                let const_id = builder.emit_imm(Immediate::String(const_id.as_bytes_id()));
                Instr::Hhbc(Hhbc::CGetS([const_id, cid], ReadonlyOp::Readonly, loc))
            }
            Instr::Hhbc(Hhbc::ColFromArray(vid, col_type, loc)) => {
                use ir::CollectionType;
                let builtin = match col_type {
                    CollectionType::ImmMap => hack::Hhbc::ColFromArrayImmMap,
                    CollectionType::ImmSet => hack::Hhbc::ColFromArrayImmSet,
                    CollectionType::ImmVector => hack::Hhbc::ColFromArrayImmVector,
                    CollectionType::Map => hack::Hhbc::ColFromArrayMap,
                    CollectionType::Pair => hack::Hhbc::ColFromArrayPair,
                    CollectionType::Set => hack::Hhbc::ColFromArraySet,
                    CollectionType::Vector => hack::Hhbc::ColFromArrayVector,
                    other => panic!("Unknown collection type: {:?}", other),
                };

                builder.hhbc_builtin(builtin, &[vid], loc)
            }
            Instr::Hhbc(Hhbc::InitProp(vid, pid, op, loc)) => {
                self.init_prop(builder, vid, pid, op, loc)
            }
            Instr::Hhbc(Hhbc::IsTypeStructC([obj, ts], op, kind, loc)) => {
                let builtin = hack::Hhbc::IsTypeStructC;
                let op = match op {
                    TypeStructResolveOp::DontResolve => 0,
                    TypeStructResolveOp::Resolve => 1,
                    _ => unreachable!(),
                };
                let kind = match kind {
                    TypeStructEnforceKind::Deep => 0,
                    TypeStructEnforceKind::Shallow => 1,
                    _ => unreachable!(),
                };
                match rewrite_constant_type_check(builder, obj, ts, loc) {
                    Some(null_check) => null_check,
                    None => {
                        let op = builder.emit_imm(Immediate::Int(op));
                        let kind = builder.emit_imm(Immediate::Int(kind));
                        builder.hhbc_builtin(builtin, &[obj, ts, op, kind], loc)
                    }
                }
            }
            Instr::Hhbc(Hhbc::LateBoundCls(loc)) => match *self.func_info {
                FuncInfo::Method(ref mi) => match mi.is_static {
                    IsStatic::Static => {
                        let this = builder.emit(Instr::Hhbc(Hhbc::This(loc)));
                        builder.hack_builtin(Builtin::GetClass, &[this], loc)
                    }
                    IsStatic::NonStatic => {
                        let this = builder.emit(Instr::Hhbc(Hhbc::This(loc)));
                        builder.hack_builtin(Builtin::GetStaticClass, &[this], loc)
                    }
                },
                FuncInfo::Function(_) => unreachable!(),
            },
            Instr::Hhbc(Hhbc::LockObj(obj, loc)) => {
                builder.emit_hhbc_builtin(hack::Hhbc::LockObj, &[obj], loc);
                Instr::copy(obj)
            }
            Instr::Hhbc(Hhbc::MemoSet(vid, locals, loc)) => {
                self.memo_set(builder, vid, &locals, loc)
            }
            Instr::Hhbc(Hhbc::MemoSetEager(vid, locals, loc)) => {
                self.memo_set_eager(builder, vid, &locals, loc)
            }
            Instr::Hhbc(Hhbc::NewStructDict(names, values, loc)) => {
                let args = names
                    .iter()
                    .zip(values.iter().copied())
                    .flat_map(|(name, value)| [builder.emit_imm(Immediate::String(*name)), value])
                    .collect_vec();
                builder.hack_builtin(Builtin::NewDict, &args, loc)
            }
            Instr::Hhbc(Hhbc::NewObj(cls, loc)) => {
                let method = MethodName::factory();
                let operands = vec![cls].into_boxed_slice();
                let context = StringId::EMPTY;
                let flavor = ObjMethodOp::NullThrows;
                let detail = CallDetail::FCallObjMethodD { flavor, method };
                let flags = FCallArgsFlags::default();
                Instr::Call(Box::new(Call {
                    operands,
                    context,
                    detail,
                    flags,
                    num_rets: 1,
                    inouts: None,
                    readonly: None,
                    named_arg_names: vec![],
                    loc,
                }))
            }
            Instr::Hhbc(Hhbc::NewObjS(clsref, loc)) => {
                let cls = self.emit_special_cls_ref(builder, clsref, loc);
                Instr::Hhbc(Hhbc::NewObj(cls, loc))
            }
            Instr::Hhbc(Hhbc::SelfCls(loc)) if self.func_info.declared_in_trait() => {
                // In a trait, turn the `self` keyword into the fresh parameter we add
                // to the type signatures.
                let lid = ir::intern("self");
                let lid = LocalId::Named(lid);
                Instr::Hhbc(Hhbc::CGetL(lid, loc))
            }
            Instr::Hhbc(Hhbc::SetOpL(vid, lid, op, loc)) => {
                let src = builder.emit(Instr::Hhbc(Hhbc::CGetL(lid, loc)));
                let op = match op {
                    SetOpOp::AndEqual => Instr::Hhbc(Hhbc::BitAnd([src, vid], loc)),
                    SetOpOp::ConcatEqual => Instr::Hhbc(Hhbc::Concat([src, vid], loc)),
                    SetOpOp::DivEqual => Instr::Hhbc(Hhbc::Div([src, vid], loc)),
                    SetOpOp::MinusEqual => Instr::Hhbc(Hhbc::Sub([src, vid], loc)),
                    SetOpOp::ModEqual => Instr::Hhbc(Hhbc::Modulo([src, vid], loc)),
                    SetOpOp::MulEqual => Instr::Hhbc(Hhbc::Mul([src, vid], loc)),
                    SetOpOp::OrEqual => Instr::Hhbc(Hhbc::BitOr([src, vid], loc)),
                    SetOpOp::PlusEqual => Instr::Hhbc(Hhbc::Add([src, vid], loc)),
                    SetOpOp::PowEqual => Instr::Hhbc(Hhbc::Pow([src, vid], loc)),
                    SetOpOp::SlEqual => Instr::Hhbc(Hhbc::Shl([src, vid], loc)),
                    SetOpOp::SrEqual => Instr::Hhbc(Hhbc::Shr([src, vid], loc)),
                    SetOpOp::XorEqual => Instr::Hhbc(Hhbc::BitXor([src, vid], loc)),
                    _ => unreachable!(),
                };
                let dst = builder.emit(op);
                Instr::Hhbc(Hhbc::SetL(dst, lid, loc))
            }
            Instr::Hhbc(Hhbc::VerifyOutType(vid, lid, loc)) => {
                self.verify_out_type(builder, vid, lid, loc)
            }
            Instr::Hhbc(Hhbc::VerifyParamType(vid, _lid, _loc)) => {
                // This is used to check the param type of default
                // parameters. We'll hold off on actually checking for now since
                // it's hard to turn the param type into a checkable TypeStruct
                // (and right now the param type has been erased in this version
                // of the function).
                Instr::Special(Special::Copy(vid))
            }
            Instr::Hhbc(Hhbc::VerifyParamTypeTS(ts, lid, loc)) => {
                self.verify_param_type_ts(builder, lid, ts, loc)
            }
            Instr::Hhbc(Hhbc::VerifyRetTypeTS([obj, ts], loc)) => {
                self.verify_ret_type_ts(builder, obj, ts, loc)
            }
            Instr::Hhbc(Hhbc::VerifyTypeTS([obj, ts], loc)) => {
                self.verify_type_ts(builder, obj, ts, loc)
            }
            Instr::Hhbc(Hhbc::IterFree(id, loc)) => {
                let lid = iter_var_name(id);
                let value = builder.emit(Instr::Hhbc(Hhbc::CGetL(lid, loc)));
                builder.hhbc_builtin(hack::Hhbc::IterFree, &[value], loc)
            }
            Instr::Hhbc(Hhbc::IterGetKey(iter_args, base_lid, loc)) => {
                let iter_lid = iter_var_name(iter_args.iter_id);
                let iter_value = builder.emit(Instr::Hhbc(Hhbc::CGetL(iter_lid, loc)));
                let base_value = builder.emit(Instr::Hhbc(Hhbc::CGetL(base_lid, loc)));
                builder.hhbc_builtin(hack::Hhbc::IterGetKey, &[iter_value, base_value], loc)
            }
            Instr::Hhbc(Hhbc::IterGetValue(iter_args, base_lid, loc)) => {
                let iter_lid = iter_var_name(iter_args.iter_id);
                let iter_value = builder.emit(Instr::Hhbc(Hhbc::CGetL(iter_lid, loc)));
                let base_value = builder.emit(Instr::Hhbc(Hhbc::CGetL(base_lid, loc)));
                builder.hhbc_builtin(hack::Hhbc::IterGetValue, &[iter_value, base_value], loc)
            }
            Instr::Hhbc(Hhbc::ClassHasReifiedGenerics(..) | Hhbc::HasReifiedParent(..))
            | Instr::Hhbc(Hhbc::ReifiedInit(..)) => {
                // Reified generics generate a lot of IR that is opaque to the analysis and actually
                // negatively affects the precision. Lowering these checks to a constant value
                // 'false' allows us to skip the whole branches related to reified generics.
                let value = builder.emit_imm(Immediate::Bool(false));
                Instr::copy(value)
            }
            Instr::Terminator(Terminator::JmpOp {
                cond,
                pred,
                targets: [true_bid, false_bid],
                loc,
            }) => {
                match lookup_constant(&builder.func, cond) {
                    // TODO(arr): do we need to check any other known falsy/truthy values here?
                    Some(Immediate::Bool(cond)) => {
                        let reachable_branch = if *cond && pred == Predicate::NonZero
                            || !*cond && pred == Predicate::Zero
                        {
                            true_bid
                        } else {
                            false_bid
                        };
                        Instr::Terminator(Terminator::Jmp(reachable_branch, loc))
                    }
                    _ => {
                        return instr;
                    }
                }
            }
            Instr::Terminator(Terminator::IterInit(args)) => self.iter_init(builder, args),
            Instr::Terminator(Terminator::IterNext(args)) => self.iter_next(builder, args),
            Instr::Terminator(Terminator::MemoGet(memo)) => self.memo_get(builder, memo),
            Instr::Terminator(Terminator::MemoGetEager(memo)) => self.memo_get_eager(builder, memo),
            Instr::Terminator(Terminator::RetM(ops, verify, loc)) => {
                // ret a, b;
                // =>
                // ret vec[a, b];
                let builtin = hack::Hhbc::NewVec;
                let vec = builder.emit_hhbc_builtin(builtin, &ops, loc);
                Instr::ret(vec, self.get_verify_kind(&builder.func, verify), loc)
            }
            Instr::Terminator(Terminator::Ret(vid, verify, loc)) => {
                let update = self.get_verify_kind(&builder.func, verify);
                if update != verify {
                    return Instr::ret(vid, update, loc);
                }
                return instr;
            }
            Instr::Terminator(Terminator::SSwitch {
                cond,
                cases,
                targets,
                loc,
            }) => {
                // if (StrEq(cond, case_0)) jmp targets[0];
                // else if (StrEq(cond, case_1)) jmp targets[1];
                // ...
                // else jmp targets.last();

                // Last case MUST be 'default'.
                let iter = cases.iter().take(cases.len() - 1).zip(targets.iter());

                for (case, target) in iter {
                    let string = builder.emit_imm(Immediate::String(*case));
                    let pred = builder.emit_hhbc_builtin(hack::Hhbc::CmpSame, &[string, cond], loc);
                    let false_bid = builder.alloc_bid_based_on_cur();
                    builder.emit(Instr::jmp_op(
                        pred,
                        Predicate::NonZero,
                        *target,
                        false_bid,
                        loc,
                    ));
                    builder.start_block(false_bid);
                }

                Instr::jmp(*targets.last().unwrap(), loc)
            }
            instr => {
                return instr;
            }
        };
        self.changed = true;
        instr
    }
}

fn rewrite_nullsafe_call(
    original_iid: InstrId,
    builder: &mut FuncBuilder,
    mut call: Call,
    state: &mut TransformState,
) -> Instr {
    // rewrite a call like `$a?->b()` into `$a ? $a->b() : null`
    call.detail = match call.detail {
        CallDetail::FCallObjMethod { .. } => CallDetail::FCallObjMethod {
            flavor: ObjMethodOp::NullThrows,
        },
        CallDetail::FCallObjMethodD { method, .. } => CallDetail::FCallObjMethodD {
            flavor: ObjMethodOp::NullThrows,
            method,
        },
        _ => unreachable!(),
    };

    // We need to be careful about multi-return!
    //   (ret, a, b) = obj?->call(inout a, inout b)
    // ->
    //   if (obj) {
    //     (ret, a, b)
    //   } else {
    //     (null, a, b)
    //   }

    let loc = call.loc;
    let obj = call.obj();
    let num_rets = call.num_rets;
    let pred = builder.emit_hack_builtin(hack::Builtin::Hhbc(hack::Hhbc::IsTypeNull), &[obj], loc);

    // Need to customize the if/then/else because of multi-return.
    let join_bid = builder.alloc_bid_based_on_cur();
    let null_bid = builder.alloc_bid_based_on_cur();
    let nonnull_bid = builder.alloc_bid_based_on_cur();
    builder.emit(Instr::jmp_op(
        pred,
        Predicate::NonZero,
        null_bid,
        nonnull_bid,
        loc,
    ));

    // Null case - main value should be null. Inouts should be passed through.
    builder.start_block(null_bid);
    let mut args = Vec::with_capacity(num_rets as usize);
    args.push(builder.emit_imm(Immediate::Null));
    if let Some(inouts) = call.inouts.as_ref() {
        for inout_idx in inouts.iter() {
            let op = call.operands[*inout_idx as usize];
            args.push(op);
        }
    }
    builder.emit(Instr::jmp_args(join_bid, &args, loc));

    // Nonnull case - make call and pass Select values.
    builder.start_block(nonnull_bid);
    args.clear();
    let iid = builder.emit(Instr::Call(Box::new(call)));
    if num_rets > 1 {
        for idx in 0..num_rets {
            args.push(builder.emit(Instr::Special(Special::Select(iid, idx))));
        }
    } else {
        args.push(iid);
    }
    builder.emit(Instr::jmp_args(join_bid, &args, loc));

    // Join
    builder.start_block(join_bid);
    args.clear();
    if num_rets > 1 {
        // we need to swap out the original Select statements.
        for idx in 0..num_rets {
            let param = builder.alloc_param();
            args.push(param);
            state.rewrite_select_idx(original_iid, idx as usize, param);
        }
        Instr::tombstone()
    } else {
        let param = builder.alloc_param();
        args.push(param);
        Instr::copy(args[0])
    }
}

fn iter_var_name(id: ir::IterId) -> LocalId {
    let name = ir::intern(format!("iter{}", id));
    LocalId::Named(name)
}

/// `is_type_struct_c` is used to type-check (among other things) against _unresolved_ but known
/// class name and for `is null`/`is nonnull` checks. This transformation detects such _constant_
/// type-checks and rewrites them using a simpler form.
fn rewrite_constant_type_check(
    builder: &mut FuncBuilder,
    obj: ValueId,
    typestruct: ValueId,
    loc: LocId,
) -> Option<Instr> {
    let typestruct = lookup_constant(&builder.func, typestruct)?;
    let typestruct = match typestruct {
        Immediate::Vec(_) | Immediate::Dict(_) | Immediate::Keyset(_) => {
            let tv = typestruct.clone().try_into().unwrap();
            TypeStruct::try_from_typed_value(&tv)?
        }
        _ => return None,
    };
    match typestruct {
        TypeStruct::Null => Some(builder.hhbc_builtin(hack::Hhbc::IsTypeNull, &[obj], loc)),
        TypeStruct::Nonnull => {
            let is_null = builder.emit_hhbc_builtin(hack::Hhbc::IsTypeNull, &[obj], loc);
            Some(builder.hhbc_builtin(hack::Hhbc::Not, &[is_null], loc))
        }
        TypeStruct::Unresolved(clsid, nullable) => {
            let instr = Instr::Hhbc(Hhbc::InstanceOfD(obj, clsid, nullable, loc));
            Some(instr)
        }
    }
}
