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
#include "hphp/runtime/vm/jit/alias-class.h"

#include <limits>
#include <algorithm>

#include <fmt/format.h>

#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/analysis.h"

namespace HPHP::jit {

namespace {

//////////////////////////////////////////////////////////////////////

std::string bit_str(AliasClass::rep bits, AliasClass::rep skip) {
  using A = AliasClass;

  switch (bits) {
  case A::BEmpty:          return "Empty";
  case A::BHeap:           return "Heap";
  case A::BUnknownTV:      return "UnkTV";
  case A::BUnknown:        return "Unk";
  case A::BElem:           return "Elem";
  case A::BMIState:        return "Mis";
  case A::BActRec:         return "ActRec";
  case A::BVMReg:          return "VmRegAny";
  case A::BLocal:          break;
  case A::BIter:           break;
  case A::BClosureArg:     break;
  case A::BProp:           break;
  case A::BElemI:          break;
  case A::BElemS:          break;
  case A::BStack:          break;
  case A::BMITempBase:     break;
  case A::BMIBase:         break;
  case A::BMIROProp:       break;
  case A::BRds:            break;
  case A::BFContext:       break;
  case A::BFFunc:          break;
  case A::BFMeta:          break;
  case A::BFBasePtr:       break;
  case A::BVMRegState:     break;
  case A::BVMFP:           break;
  case A::BVMSP:           break;
  case A::BVMPC:           break;
  case A::BVMRetAddr:      break;
  case A::BOther:          break;
  }

  auto ret = std::string{};
  auto const bset = std::bitset<32>{bits};
  for (auto i = 0; i < 32; ++i) {
    if (!bset.test(i)) continue;
    if ((1ul << i) & skip) continue;
    switch (1ul << i) {
    case A::BEmpty:
    case A::BHeap:
    case A::BUnknown:
    case A::BUnknownTV:
    case A::BElem:
    case A::BMIState:
    case A::BActRec:
      always_assert(0);
    case A::BLocal:          ret += "Lv"; break;
    case A::BIter:           ret += "It"; break;
    case A::BClosureArg:     ret += "ClArg"; break;
    case A::BProp:           ret += "Pr"; break;
    case A::BElemI:          ret += "Ei"; break;
    case A::BElemS:          ret += "Es"; break;
    case A::BStack:          ret += "St"; break;
    case A::BMITempBase:     ret += "MiTB"; break;
    case A::BMIBase:         ret += "MiB"; break;
    case A::BMIROProp:       ret += "MiROP"; break;
    case A::BRds:            ret += "Rds"; break;
    case A::BFContext:       ret += "Fc"; break;
    case A::BFFunc:          ret += "Ff"; break;
    case A::BFMeta:          ret += "Fm"; break;
    case A::BFBasePtr:       ret += "FrBase"; break;
    case A::BVMRegState:     ret += "VmState"; break;
    case A::BVMFP:           ret += "Vmfp"; break;
    case A::BVMSP:           ret += "Vmsp"; break;
    case A::BVMPC:           ret += "Vmpc"; break;
    case A::BVMRetAddr:      ret += "VmRetAddr"; break;
    case A::BOther:          ret += "Other"; break;
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

template<typename T>
size_t framelike_hash(size_t hash, T t) {
  return folly::hash::hash_combine(hash, t.frameIdx, t.ids.raw());
}

template<typename T>
void framelike_checkInvariants(T t) {
  assertx(!t.ids.empty());
}

template<typename T>
bool framelike_equal(T a, T b) {
  return a.frameIdx == b.frameIdx && a.ids == b.ids;
}

template<typename T>
bool framelike_subclass(T a, T b) {
  return a.frameIdx == b.frameIdx && a.ids <= b.ids;
}

template<typename T>
bool framelike_maybe(T a, T b) {
  return a.frameIdx == b.frameIdx && a.ids.maybe(b.ids);
}

//////////////////////////////////////////////////////////////////////

}

template<typename T>
AliasClass AliasClass::framelike_union(rep newBits, rep frm, T a, T b) {
  auto ret = AliasClass{newBits};
  auto const frmA = a;
  auto const frmB = b;
  if (frmA.frameIdx != frmB.frameIdx) return ret;

  auto const newIds = frmA.ids | frmB.ids;
  // Even when newIds.isAny(), we still know it won't alias things in
  // other frames, so keep the specialization tag.
  ret.framelike_union_set(T { frmA.frameIdx, newIds });
  ret.m_stagBits = static_cast<rep>(ret.m_stagBits | frm);
  assertx(ret.checkInvariants());
  assertx(AliasClass{ a } <= ret && AliasClass { b } <= ret);
  return ret;
}

void AliasClass::framelike_union_set(ALocal cls) {
  m_stagBits = BLocal;
  m_local = cls;
}
void AliasClass::framelike_union_set(AIter cls) {
  m_stagBits = BIter;
  m_iter = cls;
}

//////////////////////////////////////////////////////////////////////

size_t AliasClass::Hash::operator()(AliasClass acls) const {
  auto const hash = folly::hash::twang_mix64(acls.m_bits | acls.m_stagBits);
  switch (stagFor(acls.m_stagBits)) {
  case STag::None:
    return hash;

  case STag::Iter:  return framelike_hash(hash, acls.m_iter);
  case STag::Local: return framelike_hash(hash, acls.m_local);

  case STag::ClosureArg:
    return folly::hash::hash_combine(hash,
                                     acls.m_closureArg.closure,
                                     acls.m_closureArg.offset); 
  case STag::Prop:
    return folly::hash::hash_combine(hash,
                                     acls.m_prop.obj,
                                     acls.m_prop.offset);
  case STag::ElemI:
    return folly::hash::hash_combine(hash,
                                     acls.m_elemI.arr,
                                     acls.m_elemI.idx);
  case STag::ElemS:
    return folly::hash::hash_combine(hash,
                                     acls.m_elemS.arr,
                                     acls.m_elemS.key->hash());
  case STag::Stack:
    return folly::hash::hash_combine(hash,
                                     acls.m_stack.low.offset,
                                     acls.m_stack.high.offset);
  case STag::FrameAll:
    return folly::hash::hash_combine(hash, acls.m_frameAll.frameIdx);

  case STag::Rds:
    return folly::hash::hash_combine(hash, acls.m_rds.handle);
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

#define X(What, what)                                       \
  AliasClass::AliasClass(A##What x)                         \
    : m_bits(B##What)                                       \
    , m_stagBits(B##What)                                   \
    , m_##what(x)                                           \
  {                                                         \
    assertx(checkInvariants());                             \
  }                                                         \
                                                            \
  Optional<A##What> AliasClass::is_##what() const {  \
    if (*this <= A##What##Any) return what();               \
    return std::nullopt;                                     \
  }

X(Local, local)
X(Iter, iter)
X(ClosureArg, closureArg)
X(Prop, prop)
X(ElemI, elemI)
X(ElemS, elemS)
X(Stack, stack)
X(Rds, rds)

#undef X

#define X(What, what)                                       \
  Optional<A##What> AliasClass::what() const {       \
    if (m_stagBits & B##What) return m_##what;              \
    return std::nullopt;                                     \
  }

X(Local, local)
X(Iter, iter)
X(ClosureArg, closureArg)
X(Prop, prop)
X(ElemI, elemI)
X(ElemS, elemS)
X(Stack, stack)
X(Rds, rds)

#undef X

#define X(What, what)                                       \
  AliasClass::AliasClass(A##What x)                         \
    : m_bits(B##What)                                       \
    , m_stagBits(B##What)                                   \
    , m_frameAll(x)                                         \
  {                                                         \
    assertx(checkInvariants());                             \
  }                                                         \
                                                            \
  Optional<A##What> AliasClass::is_##what() const {  \
    if (*this <= A##What##Any) return what();               \
    return std::nullopt;                                     \
  }                                                         \
  Optional<A##What> AliasClass::what() const {       \
    if (m_stagBits & B##What) {                             \
      return A##What { m_frameAll.frameIdx };               \
    }                                                       \
    return std::nullopt;                                     \
  }

X(FContext, fcontext)
X(FFunc, ffunc)
X(FMeta, fmeta)
X(ActRec, actrec)

#undef X
#undef Y

Optional<AliasClass> AliasClass::mis() const {
  auto const bits = static_cast<rep>(m_bits & BMIState);

  if (bits != BEmpty) return AliasClass{bits};
  return std::nullopt;
}

Optional<AliasClass> AliasClass::is_mis() const {
  if (*this <= AMIStateAny) return mis();
  return std::nullopt;
}

Optional<AliasClass> AliasClass::frame_base() const {
  auto const bits = static_cast<rep>(m_bits & BFBasePtr);

  if (bits != BEmpty) return AliasClass{bits};
  return std::nullopt;
}

Optional<AliasClass> AliasClass::is_frame_base() const {
  if (*this <= AFBasePtr) return frame_base();
  return std::nullopt;
}

Optional<AliasClass> AliasClass::vm_reg_state() const {
  auto const bits = static_cast<rep>(m_bits & BVMRegState);

  if (bits != BEmpty) return AliasClass{bits};
  return std::nullopt;
}

Optional<AliasClass> AliasClass::is_vm_reg_state() const {
  if (*this <= AVMRegState) return vm_reg_state();
  return std::nullopt;
}

Optional<AliasClass> AliasClass::vm_reg() const {
  auto const bits = static_cast<rep>(m_bits & BVMReg);

  if (bits != BEmpty) return AliasClass{bits};
  return std::nullopt;
}

Optional<AliasClass> AliasClass::is_vm_reg() const {
  if (*this <= AVMRegAny) return vm_reg();
  return std::nullopt;
}

Optional<AliasClass> AliasClass::exclude_vm_reg() const {
  auto const bits = static_cast<rep>(m_bits & ~BVMReg);

  if (bits != BEmpty) {
    auto ret = AliasClass{bits};
    ret.m_stagBits = m_stagBits;

    switch (stagFor(m_stagBits)) {
    case STag::None:
      break;
    case STag::Iter:       new (&ret.m_iter) AIter(m_iter); break;
    case STag::FrameAll:   new (&ret.m_frameAll) UFrameBase(m_frameAll);break;
    case STag::Local:      new (&ret.m_local) ALocal(m_local); break;
    case STag::ClosureArg: new (&ret.m_closureArg) AClosureArg(m_closureArg); break;
    case STag::Prop:       new (&ret.m_prop) AProp(m_prop); break;
    case STag::ElemI:      new (&ret.m_elemI) AElemI(m_elemI); break;
    case STag::ElemS:      new (&ret.m_elemS) AElemS(m_elemS); break;
    case STag::Stack:      new (&ret.m_stack) AStack(m_stack); break;
    case STag::Rds:        new (&ret.m_rds) ARds(m_rds); break;
    }

    return ret;
  }
  return std::nullopt;
}

AliasClass::STag AliasClass::stagFor(rep bits) {
  if (bits == BEmpty) return STag::None;
  STag ret{STag::None};
#define O(tag, mask) if ((bits & mask) == bits) ret = STag::tag;
  ALIAS_CLASS_SPEC
#undef O
  //assertx(ret != STag::None);
  always_assert_flog(ret != STag::None, "Bad bits: {}", (uint32_t)bits);
  return ret;
}

bool AliasClass::checkInvariants() const {
  switch (stagFor(m_stagBits)) {
  case STag::None:           break;
  case STag::Iter:           framelike_checkInvariants(m_iter);  break;
  case STag::Local:          framelike_checkInvariants(m_local); break;
  case STag::FrameAll:       break;
  case STag::ClosureArg:     break;
  case STag::Prop:           break;
  case STag::ElemI:          break;
  case STag::Stack:
    assertx(m_stack.size() > 0);          // use AEmpty if you want that
    break;
  case STag::ElemS:
    assertx(m_elemS.key->isStatic());
    break;
  case STag::Rds:
    assertx(rds::isValidHandle(m_rds.handle));
    break;
  }

  assertx((m_bits & m_stagBits) == m_stagBits);

  return true;
}

bool AliasClass::equivData(AliasClass o) const {
  assertx(stagFor(m_stagBits) == stagFor(o.m_stagBits));
  switch (stagFor(m_stagBits)) {
  case STag::None:       return true;
  case STag::Local:      return framelike_equal(m_local, o.m_local);
  case STag::Iter:       return framelike_equal(m_iter, o.m_iter);
  case STag::FrameAll:   return m_frameAll.frameIdx == o.m_frameAll.frameIdx;
  case STag::ClosureArg: return m_closureArg.closure == o.m_closureArg.closure &&
                               m_closureArg.offset == o.m_closureArg.offset;
  case STag::Prop:       return m_prop.obj == o.m_prop.obj &&
                              m_prop.offset == o.m_prop.offset;
  case STag::ElemI:      return m_elemI.arr == o.m_elemI.arr &&
                              m_elemI.idx == o.m_elemI.idx;
  case STag::ElemS:      return m_elemS.arr == o.m_elemS.arr &&
                              m_elemS.key == o.m_elemS.key;
  case STag::Stack:      return m_stack.low == o.m_stack.low &&
                              m_stack.high == o.m_stack.high;
  case STag::Rds:        return m_rds.handle == o.m_rds.handle;
  }
  not_reached();
}

bool AliasClass::operator==(AliasClass o) const {
  return m_bits == o.m_bits &&
         m_stagBits == o.m_stagBits &&
         equivData(o);
}

/*
 * A category can stay specialized only if every operand containing it
 * specializes it. Otherwise the union would narrow that operand's generic
 * effects, even if the operands' specialized frame indices agree. Callers
 * separately check that the specialization data can be combined.
 */
AliasClass::rep AliasClass::unionSTagBits(AliasClass o) const {
  auto const generic = (m_bits & ~m_stagBits) | (o.m_bits & ~o.m_stagBits);
  return static_cast<rep>((m_stagBits | o.m_stagBits) & ~generic);
}

AliasClass AliasClass::unionData(rep newBits, AliasClass a, AliasClass b) {
  assertx(stagFor(a.m_stagBits) == stagFor(b.m_stagBits));
  auto const frm = static_cast<rep>(a.unionSTagBits(b) & BActRec);
  switch (stagFor(a.m_stagBits)) {
  case STag::None:
    break;
  case STag::ClosureArg:
  case STag::Prop:
  case STag::ElemI:
  case STag::ElemS:
  case STag::Rds:
  case STag::FrameAll:
    assertx(!a.equivData(b));
    break;
  case STag::Iter:  return framelike_union(newBits, frm, a.m_iter, b.m_iter);
  case STag::Local: return framelike_union(newBits, frm, a.m_local, b.m_local);

  case STag::Stack:
    {
      auto const stkA = a.m_stack;
      auto const stkB = b.m_stack;

      // Make a stack range big enough to contain both of them.
      auto const highest = std::max(stkA.high, stkB.high);
      auto const lowest = std::min(stkA.low, stkB.low);
      auto const newStack = AStack::range(lowest, highest);
      auto ret = AliasClass{newBits};
      new (&ret.m_stack) AStack(newStack);
      ret.m_stagBits = BStack;
      assertx(ret.checkInvariants());
      assertx(a <= ret && b <= ret);
      return ret;
    }
  }

  return AliasClass{newBits};
}

Optional<AliasClass> AliasClass::precise_union(AliasClass o) const {
  if (o <= *this) return *this;
  if (*this <= o) return o;

  auto const unioned = static_cast<rep>(m_bits | o.m_bits);

  // For a precise union, we need to make sure the returned class is not any
  // bigger than it should be.  This means we can't deal with situations where
  // we have different stags, and right now we also don't try to deal with
  // situations that have the same stag in a combinable way.  (E.g. two
  // adjacent AStack ranges, multiple ALocal locals.)
  auto const stag1 = stagFor(m_stagBits);
  auto const stag2 = stagFor(o.m_stagBits);
  if (stag1 == STag::None && stag2 == STag::None) {
    return AliasClass{unioned};
  }
  if (stag1 == STag::None && stag2 != STag::None) {
    return o.precise_union(*this); // flip args
  }
  if (stag1 == STag::FrameAll || stag2 == STag::FrameAll) {
    auto const uf1 = asUFrameBase();
    auto const uf2 = o.asUFrameBase();
    if (uf1 && uf2 && uf1->frameIdx == uf2->frameIdx) {
      auto ret = stag1 == STag::FrameAll ? o : *this;
      ret.m_bits = unioned;
      ret.m_stagBits = unionSTagBits(o);
      return ret;
    }
  }
  assertx(stag1 != STag::None);
  if (stag2 != STag::None) return std::nullopt;
  if (o.m_bits & m_stagBits) return std::nullopt;

  // Keep the data and stag from this, but change its bits.
  auto ret = *this;
  ret.m_bits = unioned;
  assertx(stagFor(ret.m_stagBits) == stag1);
  return ret;
}

AliasClass AliasClass::operator|(AliasClass o) const {
  if (auto const c = precise_union(o)) return *c;

  auto const unioned = static_cast<rep>(m_bits | o.m_bits);

  // If they have the same stag, try to merge them with unionData.
  auto stag1 = stagFor(m_stagBits);
  auto stag2 = stagFor(o.m_stagBits);
  if (stag1 == stag2) return unionData(unioned, *this, o);

  auto const frame1 = asUFrameBase();
  auto const frame2 = o.asUFrameBase();

  // If one of the alias classes have a non-None stag, we can only keep it if
  // the other doesn't have any of the corresponding bits set.
  if (stag1 != STag::None && (o.m_bits & m_stagBits)) stag1 = STag::None;
  if (stag2 != STag::None && (m_bits & o.m_stagBits)) stag2 = STag::None;

  auto ret = AliasClass{unioned};
  if (stag1 == stag2) return ret;       // both None.

  /*
   * Union operations are guaranteed to be commutative, so if there are two
   * non-None stags, we have to consistently choose between them if we're going
   * to keep one.  For now we keep the one with a smaller `rep' value, instead
   * of discarding both.
   *
   * We can also assume we're not in any of the situations that
   * precise_diffSTag_unionData supported, and that neither *this nor `o' are
   * subtypes of each other, because we already tried both of those things.
   *
   * Note also that we might be in a situation where one of the STags
   * is representing a union of more primitive STags. But for this
   * case, we've already thrown away the overlap by setting stags to
   * None above.
   */
  const AliasClass* best = &o;
  auto const stag = [&] () -> STag {
    if (stag1 != STag::None) {
      if (stag2 == STag::None || m_stagBits < o.m_stagBits) {
        best = this;
        ret.m_stagBits = m_stagBits;
        return stag1;
      }
    }
    ret.m_stagBits = o.m_stagBits;
    return stag2;
  }();

  switch (stag) {
  case STag::None:
    break;
  case STag::Iter:       new (&ret.m_iter) AIter(best->m_iter); break;
  case STag::FrameAll:   new (&ret.m_frameAll) UFrameBase(best->m_frameAll);break;
  case STag::Local:      new (&ret.m_local) ALocal(best->m_local); break;
  case STag::ClosureArg: new (&ret.m_closureArg) AClosureArg(best->m_closureArg); break;
  case STag::Prop:       new (&ret.m_prop) AProp(best->m_prop); break;
  case STag::ElemI:      new (&ret.m_elemI) AElemI(best->m_elemI); break;
  case STag::ElemS:      new (&ret.m_elemS) AElemS(best->m_elemS); break;
  case STag::Stack:      new (&ret.m_stack) AStack(best->m_stack); break;
  case STag::Rds:        new (&ret.m_rds) ARds(best->m_rds); break;
  }

  // If both alias classes have compatible frames we can merge their frame
  // specific specializations.
  if (frame1 && frame2 && frame1->frameIdx == frame2->frameIdx) {
    auto const frameBits = unionSTagBits(o) & BActRec;
    ret.m_stagBits = static_cast<rep>(ret.m_stagBits | frameBits);
  }

  return ret;
}

bool AliasClass::subclassData(AliasClass o) const {
  assertx(stagFor(m_stagBits) == stagFor(o.m_stagBits));
  switch (stagFor(m_stagBits)) {
  case STag::None:
  case STag::ClosureArg:
  case STag::Prop:
  case STag::ElemI:
  case STag::ElemS:
  case STag::Rds:
    return equivData(o);
  case STag::Iter:
    return framelike_subclass(m_iter, o.m_iter);
  case STag::Local:
    return framelike_subclass(m_local, o.m_local);
  case STag::Stack:
    return m_stack.high <= o.m_stack.high && m_stack.low >= o.m_stack.low;
  case STag::FrameAll: return m_frameAll.frameIdx == o.m_frameAll.frameIdx;
  }
  not_reached();
}

Optional<UFrameBase> AliasClass::asUFrameBase() const {
  switch (stagFor(m_stagBits)) {
  case STag::None:
  case STag::ClosureArg:
  case STag::Prop:
  case STag::ElemI:
  case STag::ElemS:
  case STag::Rds:
  case STag::Stack:
    return std::nullopt;
  case STag::Local:     return m_local;
  case STag::Iter:      return m_iter;
  case STag::FrameAll:  return m_frameAll;
  }
  not_reached();
}

/*
 * Should return true if this's specialized data is entirely contained in o's
 * specialized data, for only the portion of the data relevant for
 * "relevant_bits", with the precondition that m_stagBits != o.m_stagBits.
 *
 * We know the only case this can happen is if at least one of them is an
 * iterator, and they are only going to have a subclass relationship inside of
 * relevant_bits only if they refer to the same iterator.
 */
bool AliasClass::diffSTagSubclassData(rep relevant_bits, AliasClass o) const {
  if ((relevant_bits & BActRec) == relevant_bits) {
    if ((o.m_stagBits & m_bits) == relevant_bits) {
      return asUFrameBase()->frameIdx == o.asUFrameBase()->frameIdx;
    }
    return false;
  } else {
    return false;
  }
}

/*
 * This function conceptually should check that the portions of the specialized
 * information on the intersection of the stagBits for the two classes may
 * overlap.
 *
 * Again since we only deal with iterators, the intersection must be iterator
 * information, and we know it only intersects if they are the same iterator
 * id.
 */
bool AliasClass::diffSTagMaybeData(rep relevant_bits, AliasClass o) const {
  if (relevant_bits & BActRec) {
    return asUFrameBase()->frameIdx == o.asUFrameBase()->frameIdx;
  } else {
    return false;
  }
}

bool AliasClass::operator<=(AliasClass o) const {
  if (m_bits == BEmpty) return true;

  auto const isect = static_cast<rep>(m_bits & o.m_bits);
  if (isect != m_bits) return false;
  if (!(o.m_stagBits & isect)) return true;

  // If they have the same specialized tag, then since isect is equal to
  // m_bits, the stagBits must be part of the intersection or be BEmpty.  This
  // means they can only be in a subclass relationship if that specialized data
  // is.
  if (stagFor(m_stagBits) == stagFor(o.m_stagBits)) {
    auto const frame = m_stagBits & BActRec;
    auto const oframe = o.m_stagBits & BActRec;
    // If they have any frame specializations we should have those bits as well.
    if ((frame & oframe) == (oframe & isect)) {
      return subclassData(o);
    }
    return false;
  }

  auto const sbits  = m_stagBits;
  auto const osbits = o.m_stagBits;

  /*
   * If the stag bits for the two classes overlap, but the stags were
   * different, we're dealing with a union-style STag (like IterAll).
   */
  if (auto const inner_bits = (sbits & osbits /*& isect is redundant*/)) {
    return diffSTagSubclassData(static_cast<rep>(inner_bits), o);
  }

  /*
   * Different stags, with non-overlapping bits.  The sbits must be part of the
   * intersection (or be BEmpty), since isect == m_bits above, but osbits may
   * or may not be.  So this breaks down into the following cases:
   *
   * If the osbits are part of the intersection, then this can't be a subclass
   * of `o', because this has only generic information for that bit but it is
   * set in isect.  If the osbits was BEmpty, osbits & isect is zero, which
   * avoids this case.
   *
   * The remaining situations are that stag is STag::None, in which case it
   * is a subclass since osbits wasn't in the intersection.  Or that stag has
   * a bit that is in the isect (since m_bits == isect), and that bit is set in
   * o.m_bits.  In either case this is a subclass, so we can just return true.
   */
  if (osbits & isect) return false;
  return true;
}

bool AliasClass::maybeData(AliasClass o) const {
  assertx(stagFor(m_stagBits) == stagFor(o.m_stagBits));
  if (m_stagBits & o.m_stagBits & BActRec) {
    auto const uframe1 = asUFrameBase();
    auto const uframe2 = o.asUFrameBase();
    if (uframe1 && uframe2 && uframe1->frameIdx == uframe2->frameIdx) {
      return true;
    }
  }
  switch (stagFor(m_stagBits)) {
  case STag::None:
    not_reached();  // handled outside
  case STag::Iter:   return framelike_maybe(m_iter, o.m_iter);
  case STag::Local:  return framelike_maybe(m_local, o.m_local);
  case STag::FrameAll: return m_frameAll.frameIdx == o.m_frameAll.frameIdx;
  case STag::ClosureArg:
    if (m_closureArg.offset != o.m_closureArg.offset) return false;
    if (!m_closureArg.closure->type().maybe(o.m_closureArg.closure->type())) return false;
    return true;

  case STag::Prop:
    /*
     * We can't tell if two objects could be the same from here in general, but
     * we can rule out simple cases based on type.  The props can't be the same
     * if they are at different offsets, though.
     */
    if (m_prop.offset != o.m_prop.offset) return false;
    if (!m_prop.obj->type().maybe(o.m_prop.obj->type())) return false;
    return true;

  /*
   * Two arrays can generally be the same even if they aren't the same SSATmp,
   * because we might have loaded it from more than one place, and we have
   * linear chains in array modification instructions.
   */
  case STag::ElemI:
    if (m_elemI.idx != o.m_elemI.idx) return false;
    if (!m_elemI.arr->type().maybe(o.m_elemI.arr->type())) return false;
    return true;
  case STag::ElemS:
    if (m_elemS.key != o.m_elemS.key) return false;
    if (!m_elemS.arr->type().maybe(o.m_elemS.arr->type())) return false;
    return true;

  case STag::Stack:
    {
      // True if there's a non-empty intersection of the two stack slot
      // intervals.
      auto const lowest_upper = std::min(m_stack.high, o.m_stack.high);
      auto const highest_lower = std::max(m_stack.low, o.m_stack.low);
      return lowest_upper > highest_lower;
    }

  case STag::Rds:
    return m_rds.handle == o.m_rds.handle;
  }

  not_reached();
}

/*
 * TODO(#2884927): we probably need to be aware of possibly-integer-like string
 * keys here before we can start using ElemS for anything.  (Or else ensure
 * that we never use ElemS with an integer-like string.)
 */
bool AliasClass::maybe(AliasClass o) const {
  auto const isect = static_cast<rep>(m_bits & o.m_bits);
  if (isect == 0) return false;

  auto const sbits = m_stagBits;
  auto const osbits = o.m_stagBits;

  /*
   * If a shared portion of the specialized information is in the intersection,
   * and the intersection is otherwise empty, we can just check if the data
   * associated with the intersecting bits is in a maybe relationship.
   */
  if (auto const inner_bits = (sbits & osbits & isect)) {
    if ((inner_bits & isect) == isect) {
      if (stagFor(sbits) == stagFor(osbits)) return maybeData(o);
      return diffSTagMaybeData(static_cast<rep>(inner_bits), o);
    }
  }

  // Otherwise, the intersection is non-empty and has no specialized
  // information inside the intersecting portion to consult.  So we know they
  // overlap.
  return true;
}

bool AliasClass::isSingleLocation() const {
  // Either BEmpty or more than one bit set in rep.
  if (m_bits == 0 || (m_bits & (m_bits - 1))) return false;

  // Only MIState, FBasePtr, VMReg, and VMRegState are allowed to have no
  // specialization and be a valid single location.
  if (m_stagBits == BEmpty) {
    return *this <= (AMIStateAny | AFBasePtr | AVMRegAny | AVMRegState);
  }

  // ALocal, AStack, and AIter can contain multiple locations.
  if (auto const frame = is_local()) {
    return frame->ids.hasSingleValue();
  }
  if (auto const iter = is_iter()) {
    return iter->ids.hasSingleValue();
  }
  if (auto const stk = is_stack()) {
    return stk->size() == 1;
  }
  // All other specializations currently have exactly one location.
  return true;
}

//////////////////////////////////////////////////////////////////////

AliasClass canonicalize(AliasClass a) {
  using T = AliasClass::STag;
  switch (AliasClass::stagFor(a.m_stagBits)) {
  case T::None:           return a;
  case T::Local:          return a;
  case T::Iter:           return a;
  case T::Stack:          return a;
  case T::Rds:            return a;
  case T::FrameAll:       return a;
  case T::ClosureArg:     a.m_closureArg.closure = canonical(a.m_closureArg.closure); return a;
  case T::Prop:           a.m_prop.obj = canonical(a.m_prop.obj);                     return a;
  case T::ElemI:          a.m_elemI.arr = canonical(a.m_elemI.arr);                   return a;
  case T::ElemS:          a.m_elemS.arr = canonical(a.m_elemS.arr);                   return a;
  }
  not_reached();
}

jit::vector<AliasClass> canonicalize(jit::vector<AliasClass> as) {
  for (auto& a : as) a = canonicalize(a);
  return as;
}

//////////////////////////////////////////////////////////////////////

std::string show(AliasClass acls) {
  using A  = AliasClass;

  auto ret = bit_str(acls.m_bits, acls.m_stagBits);
  if (!ret.empty() && acls.m_stagBits != A::BEmpty) {
    ret += ' ';
  }

  switch (A::stagFor(acls.m_stagBits)) {
  case A::STag::None:
    break;
  case A::STag::Local:
    fmt::format_to(std::back_inserter(ret), "Lv {}:{}", acls.m_local.frameIdx,
                  show(acls.m_local.ids));
    break;
  case A::STag::Iter:
    fmt::format_to(std::back_inserter(ret), "It {}:{}", acls.m_iter.frameIdx,
                  show(acls.m_iter.ids));
    break;
  case A::STag::FrameAll:
    break;
  case A::STag::ClosureArg:
    fmt::format_to(std::back_inserter(ret), "ClArg t{}:{}", acls.m_closureArg.closure->id(), acls.m_closureArg.offset);
    break;
  case A::STag::Prop:
    fmt::format_to(std::back_inserter(ret), "Pr t{}:{}", acls.m_prop.obj->id(), acls.m_prop.offset);
    break;
  case A::STag::ElemI:
    fmt::format_to(std::back_inserter(ret), "Ei t{}:{}", acls.m_elemI.arr->id(), acls.m_elemI.idx);
    break;
  case A::STag::ElemS:
    fmt::format_to(std::back_inserter(ret), "Es t{}:{}", acls.m_elemS.arr->id(),
      acls.m_elemS.key->slice());
    break;
  case A::STag::Stack:
    fmt::format_to(std::back_inserter(ret), "St {}{}",
      acls.m_stack.low.offset == std::numeric_limits<int32_t>::min()
        ? "<"
        : fmt::format("{}:", acls.m_stack.low.offset),
      acls.m_stack.high.offset
    );
    break;
  case A::STag::Rds:
    fmt::format_to(std::back_inserter(ret), "Rds {}", acls.m_rds.handle);
    break;
  }

  if (acls.m_stagBits & A::BActRec) {
    if (A::stagFor(acls.m_stagBits) != A::STag::FrameAll) ret += "+";
    if (acls.m_stagBits & A::BFMeta)    ret += "Fm";
    if (acls.m_stagBits & A::BFContext) ret += "Fc";
    if (acls.m_stagBits & A::BFFunc)    ret += "Ff";
    if (A::stagFor(acls.m_stagBits) == A::STag::FrameAll) {
      fmt::format_to(std::back_inserter(ret), "@{}", acls.asUFrameBase()->frameIdx);
    }
  }

  return ret;
}

std::string show(const jit::vector<AliasClass>& as) {
  std::string ret = "[";
  auto first = true;
  for (auto const& a : as) {
    if (!first) ret += ", ";
    first = false;
    ret += show(a);
  }
  ret += "]";
  return ret;
}

//////////////////////////////////////////////////////////////////////

}
