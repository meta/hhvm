/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/ir-opcode.h"

#include "hphp/runtime/vm/jit/extra-data.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/trace.h"

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhir)

#define NF     0
#define PRc    ProducesRC
#define PNRc   (ProducesNewRC|ProducesRC)
#define T      Terminal
#define B      Branch
#define P      Passthrough
#define LA     LayoutAgnostic
#define LP     (LayoutPreserving|LayoutAgnostic)

#define ND             0
#define D(n)           HasDest
#define DofS(n)        HasDest
#define DRefineS(n)    HasDest
#define DParam(t)      HasDest
#define DLdObjCls      HasDest
#define DAllocObj      HasDest
#define DBespokeElem   HasDest
#define DBespokeElemUninit   HasDest
#define DBespokePosKey HasDest
#define DBespokePosVal HasDest
#define DIterKey       HasDest
#define DIterVal       HasDest
#define DVecElem       HasDest
#define DDictElem      HasDest
#define DKeysetElem    HasDest
#define DModified(n)   HasDest
#define DArrLikeSet    HasDest
#define DArrLikeSetPos HasDest
#define DArrLikeUnset  HasDest
#define DArrLikeAppend HasDest
#define DEscalateToVanilla HasDest
#define DVecKey           HasDest
#define DFirstElem        HasDest
#define DLastElem         HasDest
#define DFirstKey         HasDest
#define DLastKey          HasDest
#define DLoggingArrLike   HasDest
#define DStructDict    HasDest
#define DTypeStructElem   HasDest
#define DCol           HasDest
#define DMulti         NaryDest
#define DSetElem       HasDest
#define DPtrToParam    HasDest
#define DBuiltin       HasDest
#define DCall          HasDest
#define DGenIter       HasDest
#define DSubtract(n,t) HasDest
#define DUnion(...)    HasDest
#define DMemoKey       HasDest
#define DLvalOfPtr     HasDest
#define DTypeCnsClsName HasDest
#define DVerifyCoerce  HasDest
#define DPropLval      HasDest
#define DElemLval      HasDest
#define DElemLvalPos   HasDest
#define DCOW           HasDest
#define DStructTypeBound HasDest
#define DLdCls         HasDest

#define CR    IRSrcFlag::Consume
#define MMR   IRSrcFlag::Consume | IRSrcFlag::MayMove
#define MR    IRSrcFlag::Consume | IRSrcFlag::MayMove | IRSrcFlag::Move

constexpr auto processIRSrcFlags = [] (const vector<IRSrcFlag>& flags) {
  if (flags.empty()) return IRSrcFlag::None;
  assertx(flags.size() == 1);
  return flags[0];
};

#define NA
#define C(T, ...)                 processIRSrcFlags({__VA_ARGS__}),
#define CStr(...)                 processIRSrcFlags({__VA_ARGS__}),
#define SBespokeArr(...)          processIRSrcFlags({__VA_ARGS__}),
#define SMonotypeVec(...)         processIRSrcFlags({__VA_ARGS__}),
#define SMonotypeDict(...)        processIRSrcFlags({__VA_ARGS__}),
#define STypeStructure(...)       processIRSrcFlags({__VA_ARGS__}),
#define SNullptr(T, ...)          processIRSrcFlags({__VA_ARGS__}),
#define SVar(T, ...)              processIRSrcFlags({__VA_ARGS__}),
#define SStructDict(...)          processIRSrcFlags({__VA_ARGS__}),
#define S(T, ...)                 processIRSrcFlags({__VA_ARGS__}),
#define SCrossTrace               IRSrcFlag::None, IRSrcFlag::None, MMR,

namespace {
template<Opcode op, uint64_t flags>
struct op_flags {
  static constexpr uint64_t value =
    (OpHasExtraData<op>::value ? HasExtra : 0) | flags;

  static_assert(!(value & ProducesRC) ||
                (value & (HasDest | NaryDest)) == HasDest,
                "ProducesRC instructions must have exactly one dest");
};
}

OpInfo g_opInfo[] = {
#define O(name, dsts, srcs, flags)                      \
    { #name,                                            \
      op_flags<name, dsts | flags>::value,              \
      {srcs}                                            \
    },
  IR_OPCODES
#undef O
  { 0 }
};

#undef NF
#undef C
#undef E
#undef PRc
#undef PNRc
#undef Er
#undef T
#undef B
#undef P
#undef K

#undef ND
#undef D
#undef DofS
#undef DRefineS
#undef DParam
#undef DLdObjCls
#undef DBespokeElemUninit
#undef DBespokeElem
#undef DBespokePosKey
#undef DBespokePosVal
#undef DIterKey
#undef DIterVal
#undef DVecElem
#undef DDictElem
#undef DKeysetElem
#undef DModified
#undef DArrLikeSet
#undef DArrLikeSetPos
#undef DArrLikeUnset
#undef DArrLikeAppend
#undef DEscalateToVanilla
#undef DVecFirstElem
#undef DVecLastElem
#undef DVecKey
#undef DDictFirstElem
#undef DDictLastElem
#undef DDictFirstKey
#undef DDictLastKey
#undef DKeysetFirstElem
#undef DKeysetLastElem
#undef DLoggingArrLike
#undef DStructDict
#undef DTypeStructElem
#undef DCol
#undef DAllocObj
#undef DMulti
#undef DSetElem
#undef DPtrToParam
#undef DBuiltin
#undef DCall
#undef DGenIter
#undef DSubtract
#undef DUnion
#undef DMemoKey
#undef DLvalOfPtr
#undef DTypeCnsClsName
#undef DVerifyCoerce
#undef DPropLval
#undef DElemLval
#undef DElemLvalPos
#undef DCOW
#undef DStructTypeBound
#undef DLdCls

#undef CR
#undef MMR
#undef MR

#undef NA
#undef S
#undef SNullptr
#undef C
#undef CStr
#undef SVar
#undef SCrossTrace
#undef SBespokeArr
#undef SMonotypeVec
#undef SMonotypeDict
#undef STypeStructure
#undef SStructDict

///////////////////////////////////////////////////////////////////////////////

Optional<Opcode> negateCmpOp(Opcode opc) {
  switch (opc) {
    case GtBool:              return LteBool;
    case GteBool:             return LtBool;
    case LtBool:              return GteBool;
    case LteBool:             return GtBool;
    case EqBool:              return NeqBool;
    case NeqBool:             return EqBool;

    case GtInt:               return LteInt;
    case GteInt:              return LtInt;
    case LtInt:               return GteInt;
    case LteInt:              return GtInt;
    case EqInt:               return NeqInt;
    case NeqInt:              return EqInt;

    // Due to NaN only equality comparisons with doubles can be negated.
    case EqDbl:               return NeqDbl;
    case NeqDbl:              return EqDbl;

    case GtStr:               return LteStr;
    case GteStr:              return LtStr;
    case LtStr:               return GteStr;
    case LteStr:              return GtStr;
    case EqStr:               return NeqStr;
    case NeqStr:              return EqStr;
    case SameStr:             return NSameStr;
    case NSameStr:            return SameStr;

    // Objects can contain a property with NaN, so only equality comparisons can
    // be negated.
    case EqObj:               return NeqObj;
    case NeqObj:              return EqObj;
    case SameObj:             return NSameObj;
    case NSameObj:            return SameObj;

    // Arrays/vec/dicts can contain an element with NaN, so only equality
    // comparisons can be negated.
    case EqArrLike:           return NeqArrLike;
    case NeqArrLike:          return EqArrLike;
    case SameArrLike:         return NSameArrLike;
    case NSameArrLike:        return SameArrLike;

    case GtRes:               return LteRes;
    case GteRes:              return LtRes;
    case LtRes:               return GteRes;
    case LteRes:              return GtRes;
    case EqRes:               return NeqRes;
    case NeqRes:              return EqRes;

    default:                  return std::nullopt;
  }
}

bool opcodeMayRaise(Opcode opc) {
  switch (opc) {
  case IsTypeStruct:
  case IsTypeStructShallow:
    return Cfg::Eval::IsExprEnableUnresolvedWarning ||
           Cfg::Eval::IsVecNotices ||
           Cfg::Eval::WarnOnImplicitCoercionOfEnumValue;

  case StaticAnalysisError:
    return !Cfg::Eval::CrashOnStaticAnalysisError;

  case AFWHPrepareChild:
  case AKExistsObj:
  case AddNewElemKeyset:
  case AllocObj:
  case ArrayMarkLegacyRecursive:
  case ArrayMarkLegacyShallow:
  case ArrayUnmarkLegacyRecursive:
  case ArrayUnmarkLegacyShallow:
  case BespokeAppend:
  case BespokeElem:
  case BespokeGetThrow:
  case BespokeSet:
  case BespokeUnset:
  case CGetElem:
  case CGetProp:
  case CGetPropQ:
  case Call:
  case CallBuiltin:
  case CallFuncEntry:
  case CheckClsMethFunc:
  case CheckClsRGSoft:
  case CheckClsReifiedGenericMismatch:
  case CheckFunReifiedGenericMismatch:
  case CheckFunReifiedGenericsWithNamedArgs:
  case CheckFunNamedArgsMismatch:
  case CheckInOutMismatch:
  case CheckReadonlyMismatch:
  case CheckStackOverflow:
  case Clone:
  case CmpArrLike:
  case CmpObj:
  case CmpRes:
  case ConcatIntStr:
  case ConcatStr3:
  case ConcatStr4:
  case ConcatStrInt:
  case ConcatStrStr:
  case ConstructInstance:
  case ContEnter:
  case ConvArrLikeToDict:
  case ConvArrLikeToKeyset:
  case ConvArrLikeToVec:
  case ConvObjToBool:
  case ConvObjToDbl:
  case ConvObjToDict:
  case ConvObjToInt:
  case ConvObjToKeyset:
  case ConvObjToStr:
  case ConvObjToVec:
  case ConvTVToBool:
  case ConvTVToDbl:
  case ConvTVToInt:
  case ConvTVToStr:
  case Count:
  case CreateCCWH:
  case DictGet:
  case DictSet:
  case ElemDX:
  case ElemDictD:
  case ElemDictU:
  case ElemUX:
  case ElemX:
  case EqArrLike:
  case EqObj:
  case GetClsRGProp:
  case GetMemoKey:
  case GtArrLike:
  case GtObj:
  case GtRes:
  case GteArrLike:
  case GteObj:
  case GteRes:
  case HandleRequestSurprise:
  case HandleSurpriseEnter:
  case IncDecElem:
  case IncDecProp:
  case InitClsCns:
  case InitProps:
  case InitSProps:
  case InitSubClsCns:
  case InlineSideExit:
  case InterpOne:
  case IssetElem:
  case IssetProp:
  case IterExtractBase:
  case IterInitObj:
  case IterNextObj:
  case KeysetGet:
  case LdCls:
  case LdClsCached:
  case LdClsCtor:
  case LdClsPropAddrOrNull:
  case LdClsPropAddrOrRaise:
  case LdCoeffectFunParamNaive:
  case LdFunc:
  case LdFuncCached:
  case LdGblAddr:
  case LdGblAddrDef:
  case LdObjMethodD:
  case LdObjMethodS:
  case LdTypeCns:
  case LdTypeCnsClsName:
  case LdTypeCnsNoThrow:
  case LookupClsCns:
  case LookupClsCtxCns:
  case LookupClsMethod:
  case LookupClsMethodCache:
  case LookupClsMethodFCache:
  case LookupCnsE:
  case LookupFuncCached:
  case LtArrLike:
  case LtObj:
  case LtRes:
  case LteArrLike:
  case LteObj:
  case LteRes:
  case MapGet:
  case MapSet:
  case NSameArrLike:
  case NativeImpl:
  case NeqArrLike:
  case NeqObj:
  case NewKeysetArray:
  case OODeclExists:
  case OrdStrIdx:
  case OutlineSetOp:
  case PrintBool:
  case PrintInt:
  case PrintStr:
  case ProfileSubClsCns:
  case PropDX:
  case PropQ:
  case PropTypeRedefineCheck:
  case PropTypeValid:
  case PropX:
  case CheckStrictPackageDynamicReference:
  case RaiseCoeffectsCallViolation:
  case RaiseCoeffectsFunParamCoeffectRulesViolation:
  case RaiseCoeffectsFunParamTypeViolation:
  case RaiseError:
  case RaiseErrorOnInvalidIsAsExpressionType:
  case RaiseForbiddenDynCall:
  case RaiseForbiddenDynConstruct:
  case RaiseMissingDynamicallyReferenced:
  case RaiseModuleBoundaryViolation:
  case RaiseModulePropertyViolation:
  case ThrowMissingNamedParam:
  case ThrowNamedArgumentNameMismatch:
  case ThrowUnexpectedNamedArguments:
  case RaiseNotice:
  case RaiseStrToClassNotice:
  case RaiseTooManyArg:
  case RaiseWarning:
  case RecordReifiedGenericsAndGetTSList:
  case ReifiedInit:
  case ResolveTypeStruct:
  case ReturnHook:
  case SameArrLike:
  case SetElem:
  case SetNewElem:
  case SetOpElem:
  case SetOpProp:
  case SetOpTV:
  case SetProp:
  case SetRange:
  case SetRangeRev:
  case StringGet:
  case SuspendHookAwaitEF:
  case SuspendHookAwaitEG:
  case SuspendHookAwaitR:
  case SuspendHookCreateCont:
  case SuspendHookYield:
  case ThrowArrayIndexException:
  case ThrowArrayKeyException:
  case ThrowAsTypeStructError:
  case ThrowAsTypeStructException:
  case ThrowCallReifiedFunctionWithoutGenerics:
  case ThrowCannotModifyReadonlyCollection:
  case ThrowDivisionByZeroException:
  case ThrowHasThisNeedStatic:
  case ThrowInOutMismatch:
  case ThrowInvalidArrayKey:
  case ThrowInvalidOperation:
  case ThrowLateInitPropError:
  case ThrowLocalMustBeValueTypeException:
  case ThrowMissingArg:
  case ThrowMissingThis:
  case ThrowMustBeEnclosedInReadonly:
  case ThrowMustBeMutableException:
  case ThrowMustBeReadonlyException:
  case ThrowMustBeValueTypeException:
  case ThrowOutOfBounds:
  case ThrowReadonlyMismatch:
  case ThrowUndefPropException:
  case ThrowUninitLoc:
  case UnsetElem:
  case VectorSet:
  case VerifyParam:
  case VerifyParamCallable:
  case VerifyParamCls:
  case VerifyParamCoerce:
  case VerifyParamFail:
  case VerifyParamFailHard:
  case VerifyProp:
  case VerifyPropAll:
  case VerifyPropCls:
  case VerifyPropCoerce:
  case VerifyPropCoerceAll:
  case VerifyPropFail:
  case VerifyPropFailHard:
  case VerifyReifiedLocalType:
  case VerifyReifiedReturnType:
  case VerifyType:
  case VerifyRet:
  case VerifyRetCallable:
  case VerifyRetCls:
  case VerifyRetCoerce:
  case VerifyRetFail:
  case VerifyRetFailHard:
    return true;

  case AFWHBlockOn:
  case AFWHPushTailFrame:
  case AKExistsDict:
  case AKExistsKeyset:
  case AbsDbl:
  case AddDbl:
  case AddInt:
  case AddNewElemVec:
  case AddOffset:
  case AdvanceDictPtrIter:
  case AdvanceKeysetPtrIter:
  case AdvanceVecPtrIter:
  case AllocBespokeStructDict:
  case AllocInitROM:
  case AllocStructDict:
  case AllocVec:
  case AndInt:
  case AssertLoc:
  case AssertMBase:
  case AssertNonNull:
  case AssertStk:
  case AssertType:
  case AsyncFuncRet:
  case AsyncFuncRetPrefetch:
  case AsyncFuncRetSlow:
  case AsyncGenRetR:
  case AsyncGenYieldR:
  case AsyncSwitchFast:
  case BeginCatch:
  case DefCalleeFP:
  case BespokeEscalateToVanilla:
  case BespokeGet:
  case BespokeIterEnd:
  case BespokeIterFirstPos:
  case BespokeIterGetKey:
  case BespokeIterGetVal:
  case BespokeIterLastPos:
  case BespokeSetPos:
  case CallViolatesModuleBoundary:
  case Ceil:
  case CheckArrayCOW:
  case CheckCold:
  case CheckDictKeys:
  case CheckDictOffset:
  case CheckFuncNeedsCoverage:
  case CheckKeysetOffset:
  case CheckMROProp:
  case CheckMissingKeyInArrLike:
  case CheckNonNull:
  case CheckNullptr:
  case CheckPtrIterTombstone:
  case CheckRDSInitialized:
  case CheckRange:
  case CheckSmashableClass:
  case CheckSubClsCns:
  case CheckSurpriseFlags:
  case CheckSurpriseFlagsEnter:
  case CheckType:
  case CheckVecBounds:
  case ChrInt:
  case ClassHasAttr:
  case ClassHasReifiedGenerics:
  case CmpBool:
  case CmpDbl:
  case CmpInt:
  case CmpStr:
  case ColIsEmpty:
  case ColIsNEmpty:
  case Conjure:
  case ConstructClosure:
  case ContArIncIdx:
  case ContArIncKey:
  case ContArUpdateIdx:
  case ConvBoolToDbl:
  case ConvBoolToInt:
  case ConvDblToBool:
  case ConvDblToInt:
  case ConvDblToStr:
  case ConvFuncPrologueFlagsToARFlags:
  case ConvIntToBool:
  case ConvIntToDbl:
  case ConvIntToStr:
  case ConvPtrToLval:
  case ConvResToDbl:
  case ConvResToInt:
  case ConvStrToBool:
  case ConvStrToDbl:
  case ConvStrToInt:
  case CopyArray:
  case CountCollection:
  case CountDict:
  case CountKeyset:
  case CountVec:
  case CountWHNotDone:
  case CreateAFWH:
  case CreateAFWHL:
  case CreateAGWH:
  case CreateAGen:
  case CreateFSWH:
  case CreateGen:
  case CreateSSWH:
  case DbgAssertFunc:
  case DbgAssertRefCount:
  case DbgCheckLocalsDecRefd:
  case DbgTraceCall:
  case DbgTrashFrame:
  case DbgTrashMem:
  case DbgTrap:
  case DbgTrashStk:
  case DblAsBits:
  case DebugBacktrace:
  case DecRef:
  case DecRefNZ:
  case DecReleaseCheck:
  case DefConst:
  case DefFP:
  case DefFrameRelSP:
  case DefFuncEntryArFlags:
  case DefFuncEntryCalleeId:
  case DefFuncEntryCtx:
  case DefFuncEntryFP:
  case DefFuncEntryPrevFP:
  case DefFuncPrologueCallee:
  case DefFuncPrologueCtx:
  case DefFuncPrologueFlags:
  case DefFuncPrologueNumArgs:
  case DefFuncPrologueNamedArgs:
  case DefLabel:
  case DefRegSP:
  case DeserializeLazyProp:
  case DictFirst:
  case DictFirstKey:
  case DictGetK:
  case DictGetQuiet:
  case DictIdx:
  case DictIsset:
  case DictIterEnd:
  case DictLast:
  case DictLastKey:
  case DirFromFilepath:
  case DivDbl:
  case DivInt:
  case ElemDictK:
  case EndBlock:
  case EndCatch:
  case LeaveInlineFrame:
  case EnterFrame:
  case EnterInlineFrame:
  case EnterPrologue:
  case EnterTranslation:
  case EqArrayDataPtr:
  case EqBool:
  case EqClassId:
  case EqCls:
  case EqDbl:
  case EqFunc:
  case EqFuncId:
  case EqInt:
  case EqLazyCls:
  case EqPtrIter:
  case EqRes:
  case EqStr:
  case EqStrPtr:
  case ExitPrologue:
  case ExtendsClass:
  case FinishMemberOp:
  case Floor:
  case FuncCred:
  case FuncHasAttr:
  case FuncHasReifiedGenerics:
  case GenericRetDecRefs:
  case GetDictPtrIter:
  case GetKeysetPtrIter:
  case GetMemoKeyScalar:
  case GetTime:
  case GetTimeNs:
  case GetVecPtrIter:
  case GtBool:
  case GtDbl:
  case GtInt:
  case GtStr:
  case GteBool:
  case GteDbl:
  case GteInt:
  case GteStr:
  case HasReifiedParent:
  case HasToString:
  case IncCallCounter:
  case IncProfCounter:
  case IncStatCounter:
  case IncRef:
  case IncStat:
  case InitDictElem:
  case InitObjMemoSlots:
  case InitObjProps:
  case InitStructElem:
  case InitStructPositions:
  case InitThrowableFileAndLine:
  case InitVecElem:
  case InitVecElemLoop:
  case InlineCall:
  case InlineSideExitSyncStack:
  case InstanceOf:
  case InstanceOfBitmask:
  case InstanceOfIface:
  case InstanceOfIfaceVtable:
  case IntAsPtrToElem:
  case InterfaceSupportsArrLike:
  case InterfaceSupportsDbl:
  case InterfaceSupportsInt:
  case InterfaceSupportsStr:
  case InterpOneCF:
  case IsCol:
  case IsFunReifiedGenericsMatched:
  case IsLegacyArrLike:
  case IsTypeStructCached:
  case IsWaitHandle:
  case IterGetKeyArr:
  case IterGetValArr:
  case IterInitArr:
  case IterNextArr:
  case Jmp:
  case JmpExit:
  case JmpNZero:
  case JmpZero:
  case KeysetFirst:
  case KeysetGetK:
  case KeysetGetQuiet:
  case KeysetIdx:
  case KeysetIsset:
  case KeysetIterEnd:
  case KeysetLast:
  case KillActRec:
  case KillIter:
  case KillLoc:
  case LdAFWHActRec:
  case LdARFlags:
  case LdARFunc:
  case LdBindAddr:
  case LdClosureArg:
  case LdClosureCls:
  case LdClosureThis:
  case LdClsCns:
  case LdClsCnsVecLen:
  case LdClsCtxCns:
  case LdClsFromClsMeth:
  case LdClsFromRClsMeth:
  case LdClsInitData:
  case LdClsInitElem:
  case LdClsMethod:
  case LdClsMethodCacheCls:
  case LdClsMethodCacheFunc:
  case LdClsMethodFCacheFunc:
  case LdClsName:
  case LdCns:
  case LdColDict:
  case LdColVec:
  case LdContActRec:
  case LdContArKey:
  case LdContArValue:
  case LdContResumeAddr:
  case LdContState:
  case LdEnumClassLabelName:
  case LdFrameCls:
  case LdFrameThis:
  case LdFuncCls:
  case LdFuncFromClsMeth:
  case LdFuncFromRClsMeth:
  case LdFuncFromRFunc:
  case LdFuncInOutBits:
  case LdFuncName:
  case LdFuncNumParams:
  case LdFuncRequiredCoeffects:
  case LdFuncVecLen:
  case LdGenericsFromRClsMeth:
  case LdGenericsFromRFunc:
  case LdIfaceMethod:
  case LdImplicitContext:
  case LdImplicitContextMemoKey:
  case LdMemoAgnosticIC:
  case LdInitPropAddr:
  case LdInitRDSAddr:
  case LdIterEnd:
  case LdIterPos:
  case LdLazyClsName:
  case LdLoc:
  case LdLocAddr:
  case LdLocForeign:
  case LdMBase:
  case LdMIStateTempBaseAddr:
  case LdMem:
  case LdMethCallerName:
  case LdMonotypeDictKey:
  case LdMonotypeDictTombstones:
  case LdMonotypeDictVal:
  case LdMonotypeVecElem:
  case LdObjClass:
  case LdObjInvoke:
  case LdOutAddr:
  case LdPairElem:
  case LdPropAddr:
  case LdPtrIterKey:
  case LdPtrIterVal:
  case LdPublicFunc:
  case LdRDSAddr:
  case LdResolvedTypeCns:
  case LdResolvedTypeCnsClsName:
  case LdResolvedTypeCnsNoCheck:
  case LdRetVal:
  case LdSSwitchDest:
  case LdSmashable:
  case LdSmashableFunc:
  case LdStk:
  case LdStkAddr:
  case LdStrLen:
  case LdStructDictKey:
  case LdStructDictVal:
  case LdSubClsCns:
  case LdSwitchDest:
  case LdTVAux:
  case LdTVFromRDS:
  case LdTypeStructureVal:
  case LdTypeStructureValCns:
  case LdUnitPerRequestFilepath:
  case LdUnwinderValue:
  case LdVecElem:
  case LdVecElemAddr:
  case LdVectorSize:
  case LdWHNotDone:
  case LdWHResult:
  case LdWHState:
  case LoadBCSP:
  case LockObj:
  case LogArrayReach:
  case LogClsSpeculation:
  case LogGuardFailure:
  case LookupCls:
  case LookupClsCached:
  case LookupSPropSlot:
  case Lshr:
  case LtBool:
  case LtDbl:
  case LtInt:
  case LtStr:
  case LteBool:
  case LteDbl:
  case LteInt:
  case LteStr:
  case MapIsset:
  case MarkRDSAccess:
  case MarkRDSInitialized:
  case MemoGetInstanceCache:
  case MemoGetInstanceValue:
  case MemoGetLSBCache:
  case MemoGetLSBValue:
  case MemoGetStaticCache:
  case MemoGetStaticValue:
  case MemoSetInstanceCache:
  case MemoSetInstanceValue:
  case MemoSetLSBCache:
  case MemoSetLSBValue:
  case MemoSetStaticCache:
  case MemoSetStaticValue:
  case MethodExists:
  case Mod:
  case Mov:
  case MulDbl:
  case MulInt:
  case MulIntO:
  case NInstanceOfBitmask:
  case NSameObj:
  case NSameStr:
  case NeqBool:
  case NeqDbl:
  case NeqInt:
  case NeqRes:
  case NeqStr:
  case NewBespokeStructDict:
  case NewClsMeth:
  case NewCol:
  case NewColFromArray:
  case NewDictArray:
  case NewInstanceRaw:
  case NewLoggingArray:
  case NewPair:
  case NewRClsMeth:
  case NewRFunc:
  case NewStructDict:
  case Nop:
  case OrInt:
  case OrdStr:
  case PairIsset:
  case ProfileArrLikeProps:
  case ProfileArrayCOW:
  case ProfileCall:
  case ProfileCoeffectFunParam:
  case ProfileDecRef:
  case ProfileDictAccess:
  case ProfileGlobal:
  case ProfileInstanceCheck:
  case ProfileIsTypeStruct:
  case ProfileIterInit:
  case ProfileKeysetAccess:
  case ProfileMethod:
  case ProfileProp:
  case ProfileSwitchDest:
  case ProfileType:
  case PseudoRandomInt:
  case PtrToElemAsInt:
  case PushEmptyReifiedGenericsWithNamedArgs:
  case RBTraceEntry:
  case RBTraceMsg:
  case RecordFuncCall:
  case ReleaseShallow:
  case ReqBindJmp:
  case ReqInterpBBNoTranslate:
  case ReqRetranslate:
  case ReqRetranslateOpt:
  case ReserveVecNewElem:
  case RetranslateOptAsync:
  case RetCtrl:
  case SameObj:
  case SameStr:
  case Select:
  case Shl:
  case Shr:
  case Sqrt:
  case StArResumeAddr:
  case StClosureArg:
  case StClsInitElem:
  case StContArKey:
  case StContArState:
  case StContArValue:
  case StFrameCtx:
  case StFrameFunc:
  case StFrameMeta:
  case StImplicitContext:
  case StIterEnd:
  case StIterPos:
  case StLoc:
  case StLocMeta:
  case StLocRange:
  case StMBase:
  case StMROProp:
  case StMem:
  case StMemMeta:
  case StOutValue:
  case StPtrAt:
  case StPtrIterVal:
  case StStk:
  case StStkMeta:
  case StStkRange:
  case StTVInRDS:
  case StTypeAt:
  case StUnwinderExn:
  case StVMFP:
  case StVMPC:
  case StVMRegState:
  case StVMReturnAddr:
  case StVMSP:
  case StrictlyIntegerConv:
  case StringIsset:
  case StructDictAddNextSlot:
  case StructDictElemAddr:
  case StructDictSlot:
  case StructDictSlotInPos:
  case StructDictTypeBoundCheck:
  case StructDictUnset:
  case SubDbl:
  case SubInt:
  case Unreachable:
  case UnwindCheckSideExit:
  case VecFirst:
  case VecLast:
  case VectorIsset:
  case VoidPtrAsDataType:
  case XorBool:
  case XorInt:
    return false;
  }
  not_reached();
}

///////////////////////////////////////////////////////////////////////////////
}
