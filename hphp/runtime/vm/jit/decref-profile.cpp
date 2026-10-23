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

#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/base/array-iterator.h"

#include <fmt/format.h>

namespace {
bool isTypedValueRefcounted(const HPHP::TypedValue& tv) {
  return isRefcountedType(tv.type()) && tv.val().pcnt->isRefCounted();
}
}
namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

void DecRefProfile::reduce(DecRefProfile& a, const DecRefProfile& b) {
  auto const total = static_cast<uint64_t>(a.total) +
                     static_cast<uint64_t>(b.total);
  auto constexpr limit = std::numeric_limits<decltype(a.total)>::max();
  if (total > limit) {
    auto const scale = [&] (uint32_t& x, uint64_t y) {
      x = ((x + y) * limit + total - 1) / total;
    };
    a.total = limit;
    scale(a.refcounted, b.refcounted);
    scale(a.released, b.released);
    scale(a.decremented, b.decremented);
    scale(a.arrayOfUncountedReleaseCount, b.arrayOfUncountedReleaseCount);
  } else {
    a.total       = total;
    a.refcounted  += b.refcounted;
    a.released    += b.released;
    a.decremented += b.decremented;
    a.arrayOfUncountedReleaseCount += b.arrayOfUncountedReleaseCount;
  }
  a.updateDataType(b.datatype);
}

void DecRefProfile::update(TypedValue tv) {
  auto constexpr max = std::numeric_limits<decltype(total)>::max();
  if (total == max) return;

  total++;
  updateDataType(dt_modulo_persistence(tv.type()));
  if (!isRefcountedType(tv.type())) return;
  refcounted++;
  auto const countable = tv.val().pcnt;
  if (countable->decWillRelease()) {
    released++;
    if (isRealType(datatype) && isArrayLikeType(datatype) &&
        tv.val().parr->isVanilla()) {
      size_t numProfiledElements = 0;
      bool hasRefcountedElement = false;
      IterateKV(tv.val().parr, [&](TypedValue key, TypedValue value){
        numProfiledElements++;
        hasRefcountedElement = isTypedValueRefcounted(key) ||
                               isTypedValueRefcounted(value);
        return hasRefcountedElement ||
               (numProfiledElements == kMaxNumProfiledElements);
      });
      if (!hasRefcountedElement) {
        arrayOfUncountedReleaseCount++;
      }
    }
  } else if (countable->isRefCounted()) {
    decremented++;
  }
}

void DecRefProfile::updateDataType(HPHP::DataType newDT) {
  if (datatype == kNoDataTypesSeen) {
    datatype = newDT;
  } else if (datatype != newDT && newDT != kNoDataTypesSeen) {
    datatype = kMultipleDataTypesSeen;
  }
}

folly::dynamic DecRefProfile::toDynamic() const {
  return folly::dynamic::object("total", total)
                               ("uncounted", uncounted())
                               ("percentUncounted", percent(uncounted()))
                               ("persistent", persistent())
                               ("percentPersistent", percent(persistent()))
                               ("destroyed", destroyed())
                               ("percentDestroyed", percent(destroyed()))
                               ("survived", survived())
                               ("percentSurvived", percent(survived()))
                               ("datatype", (data_type_t)datatype)
                               ("arrayOfUncountedReleaseCount", percent(arrayOfUncountedReleasedCount()))
                               ("profileType", "DecRefProfile");
}

std::string DecRefProfile::toString() const {
  return fmt::format(
    "total: {:4}\n uncounted: {:4} ({:.1f}%),\n persistent: {:4} ({:.1f}%),\n"
    " destroyed: {:4} ({:.1f}%),\n survived: {:4} ({:.1f}%),\n datatype: {:4}\n"
    "arrayOfUncountedReleasedCount: {:4} ({:.1f}%) \n",
    total,
    uncounted(),  percent(uncounted()),
    persistent(), percent(persistent()),
    destroyed(),  percent(destroyed()),
    survived(),   percent(survived()),
    datatype,
    arrayOfUncountedReleasedCount(), percent(arrayOfUncountedReleasedCount())
  );
}

const StringData* decRefProfileKey(int locId) {
  return makeStaticString(folly::to<std::string>("DecRefProfile-", locId));
}

const StringData* decRefProfileKey(const IRInstruction* inst) {
  auto const local = inst->extra<DecRefData>()->locId;
  return decRefProfileKey(local);
}

TargetProfile<DecRefProfile> decRefProfile(
    const TransContext& context, const IRInstruction* inst) {
  auto const profileKey = decRefProfileKey(inst);
  return TargetProfile<DecRefProfile>(context, inst->marker(), profileKey);
}

TargetProfile<DecRefProfile> decRefProfile(const TransContext& context,
                                           const BCMarker& marker,
                                           int locId) {
  auto const profileKey = decRefProfileKey(locId);
  return TargetProfile<DecRefProfile>(context, marker, profileKey);
}

///////////////////////////////////////////////////////////////////////////////

}
