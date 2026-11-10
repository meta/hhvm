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

#include "hphp/runtime/base/perf-mem-event.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/header-kind.h"
#include "hphp/runtime/base/member-reflection.h"
#include "hphp/runtime/base/memory-manager-defs.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/reverse-data-map.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/tc-internal.h"

#include "hphp/util/alloc.h"
#include "hphp/util/perf-event.h"
#include "hphp/util/stack-trace.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/trace.h"

#include <boost/algorithm/string/predicate.hpp>

#include <folly/Overload.h>
#include <folly/ScopeGuard.h>
#ifdef HHVM_FACEBOOK
#include <folly/Demangle.h>
#include <folly/debugging/symbolizer/Symbolizer.h>
#endif

namespace HPHP {

TRACE_SET_MOD(perf_mem_event)

using namespace jit;

///////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * Version number for the entries.
 *
 * Bump this whenever the log format changes, so that it's easy to filter out
 * old, incompatible results.
 */
constexpr auto kVersion = 3;

/*
 * Update `record' with the data member that `internal' is in, relative to
 * `base', returning whether or not a corresponding member is found.
 */
template<class T>
bool try_member(const T* base, const void* internal,
                StructuredLogEntry& record) {
  if (auto const memb = nameof_member(base, internal)) {
    record.setStr("member", memb);
    return true;
  }
  return false;
}

/*
 * Per-type log entry updaters.
 *
 * These all assume that `addr' is a pointer that is logically internal to the
 * base object.
 */

void fill_record(const Class* cls, const void* addr,
                 StructuredLogEntry& record) {
  record.setStr("name", cls->name()->data());

  if (cls->classVec() <= addr && addr < cls->mallocEnd()) {
    auto const idx = (reinterpret_cast<uintptr_t>(addr)
                     - reinterpret_cast<uintptr_t>(cls->classVec()))
                     / sizeof(*cls->classVec());
    record.setStr("member", "m_classVec");
    record.setInt("index", idx);
  }
  // Introspect members after dealing with the variable-length terminal
  // array member.
  if (try_member(cls, addr, record)) return;

  if (cls->funcVec() <= addr && addr < cls) {
    auto const idx = (reinterpret_cast<uintptr_t>(cls)
                     - reinterpret_cast<uintptr_t>(addr))
                     / sizeof(*cls->funcVec());
    record.setStr("member", "funcVec");
    record.setInt("index", idx);
  }
}

void fill_record(const Func* func, const void* addr,
                 StructuredLogEntry& record) {
  record.setStr("name", func->fullName()->data());

  auto const func_end = reinterpret_cast<const char*>(func)
                        + Func::prologueTableOff();
  if (func_end <= addr && addr < func->mallocEnd()) {
    auto const idx = (reinterpret_cast<const char*>(addr) - func_end)
                     / sizeof(AtomicLowTCA);
    record.setStr("member", "m_prologueTable");
    record.setInt("index", idx);
  }
  // Introspect members after dealing with the variable-length terminal
  // array member.
  try_member(func, addr, record);
}

void fill_record(const Unit* unit, const void* addr,
                 StructuredLogEntry& record) {
  record.setStr("name", unit->origFilepath()->data());
  try_member(unit, addr, record);
}

void fill_record(const StringData* sd, const void* addr,
                 StructuredLogEntry& record) {
  record.setStr("data", sd->data());
  if (try_member(sd, addr, record)) return;

  auto const idx = uintptr_t(addr) - uintptr_t(sd->data());
  if (idx < 0) return;

  record.setInt("index", idx);
}

void fill_record(const ArrayData* arr, const void* addr,
                 StructuredLogEntry& record) {
  if (try_member(arr, addr, record)) return;
  auto const idx = VanillaVec::pointerToIndex(arr, addr);
  if (idx < 0) return;

  record.setInt("ikey", idx);

  if (idx < arr->size()) {
    auto const base = reinterpret_cast<const char*>(addr);
    auto const rval = VanillaVec::LvalInt(const_cast<ArrayData*>(arr), idx);
    auto const type_diff = reinterpret_cast<const char*>(&type(rval)) - base;
    auto const data_diff = reinterpret_cast<const char*>(&val(rval)) - base;
    if (0 <= type_diff && type_diff < sizeof(type(rval))) {
      record.setStr("value_memb", "m_type");
    } else if (0 <= data_diff && data_diff < sizeof(val(rval))) {
      record.setStr("value_memb", "m_data");
    }
    record.setStr("value_type", tname(type(rval)));
  }
}

void fill_record(const VanillaDict* arr, const void* addr,
                 StructuredLogEntry& record) {
  if (try_member(arr, addr, record)) return;

  auto const data = reinterpret_cast<const VanillaDict::Elm*>(arr + 1);
  auto const idx = (uintptr_t(addr) - uintptr_t(data))
                   / sizeof(VanillaDict::Elm);

  if (idx < arr->iterLimit()) {
    auto const elm = &data[idx];

    if (auto const memb = nameof_member(elm, addr)) {
      record.setStr("value_memb", memb);
    }
    if (elm->isTombstone()) {
      record.setStr("value_type", "<tombstone>");
    } else {
      record.setStr("value_type", tname(elm->data.m_type));
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Update `record' for `tca', known to point into the TC.
 */
bool record_tc_mem_event(TCA tca, StructuredLogEntry& record) {
  record.setStr("location", "jit_code");

  auto const ustub = tc::ustubs().describe(tca);
  if (!boost::starts_with(ustub, "0x")) {
    record.setStr("kind", "ustub");
    record.setStr("name", ustub.substr(0, ustub.find('+')));
  }

  return true;
}

/*
 * Update `record' for an `addr' known to be internal to the VM metadata object
 * given by `res'.
 */
bool record_vm_metadata_mem_event(data_map::result res, const void* addr,
                                  StructuredLogEntry& record) {
  auto const pos = reinterpret_cast<const char*>(addr);

  assertx(!res.empty());
  res.apply(folly::overload(
    [&](const ArrayData* arr) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(arr));
      record.setStr("kind", "Array");
    },
    [&](const Class* cls) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(cls));
      record.setStr("kind", "Class");
      fill_record(cls, addr, record);
    },
    [&](const Func* func) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(func));
      record.setStr("kind", "Class");
      record.setStr("kind", "Func");
      fill_record(func, addr, record);
    },
    [&](const NamedType* ne) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(ne));
      record.setStr("kind", "NamedType");
      try_member(ne, addr, record);
    },
    [&](const NamedFunc* ne) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(ne));
      record.setStr("kind", "NamedFunc");
      try_member(ne, addr, record);
    },
    [&](const StringData* sd) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(sd));
      record.setStr("kind", "StringData");
      fill_record(sd, addr, record);
    },
    [&](const Unit* unit) {
      record.setInt("offset", pos - reinterpret_cast<const char*>(unit));
      record.setStr("kind", "Unit");
      fill_record(unit, addr, record);
    }
  ));
  return true;
}

/*
 * Update `record' for an `addr' in low memory.
 */
bool record_low_mem_event(const void* addr, StructuredLogEntry& record) {
  record.setStr("location", "low_mem");

  // See if `addr' refers to some VM metadata object.
  auto const res = data_map::find_containing(addr);
  if (!res.empty()) {
    return record_vm_metadata_mem_event(res, addr, record);
  }

  // Try to symbolize `addr' if possible.
#ifdef HHVM_FACEBOOK
  using namespace folly::symbolizer;
  Symbolizer symbolizer;
  SymbolizedFrame frame;

  auto const iddr = reinterpret_cast<uintptr_t>(addr);
  symbolizer.symbolize(iddr, frame);

  auto const name = folly::demangle(frame.name);
  if (!name.empty()) record.setStr("symbol", name);
#endif

  return true;
}

/*
 * Update `record' for an `addr' known to be in the request heap object given
 * by `hdr'.
 */
bool record_request_heap_mem_event(const void* addr,
                                   const HeapObject* hdr,
                                   StructuredLogEntry& record) {
  record.setStr("location", "request_heap");
  record.setStr("kind", header_names[uint8_t(hdr->kind())]);
  record.setInt("offset", reinterpret_cast<const char*>(addr) -
                          reinterpret_cast<const char*>(hdr));

  switch (hdr->kind()) {
    case HeaderKind::String:
      fill_record(static_cast<const StringData*>(hdr), addr, record);
      break;

    case HeaderKind::Vec:
      fill_record(static_cast<const ArrayData*>(hdr), addr, record);
      break;

    case HeaderKind::Dict:
      fill_record(static_cast<const VanillaDict*>(hdr), addr, record);
      break;

    case HeaderKind::Keyset:
      try_member(static_cast<const VanillaKeyset*>(hdr), addr, record);
      break;

    case HeaderKind::BespokeVec:
    case HeaderKind::BespokeDict:
    case HeaderKind::BespokeKeyset:
      // TODO(kshaunak): Expose an address -> element API for bespokes.
      break;

    case HeaderKind::Object:
    case HeaderKind::NativeObject:
    case HeaderKind::Closure:
    case HeaderKind::WaitHandle:
    case HeaderKind::AsyncFuncWH:
    case HeaderKind::AwaitAllWH:
    case HeaderKind::ConcurrentWH:
    case HeaderKind::Resource:
    case HeaderKind::ClsMeth:
    case HeaderKind::RClsMeth:

    case HeaderKind::Vector:
    case HeaderKind::Map:
    case HeaderKind::Set:
    case HeaderKind::Pair:
    case HeaderKind::ImmVector:
    case HeaderKind::ImmMap:
    case HeaderKind::ImmSet:

    case HeaderKind::AsyncFuncFrame:
    case HeaderKind::NativeData:
    case HeaderKind::ClosureHdr:
    case HeaderKind::MemoData:

    case HeaderKind::RFunc:
      break;

    case HeaderKind::Cpp:
    case HeaderKind::SmallMalloc:
    case HeaderKind::BigMalloc:
    case HeaderKind::Slab:
    case HeaderKind::Free:
    case HeaderKind::Hole:
      break;
  }
  return true;
}

/*
 * Update `record' for an `addr' known to be on the native C++ or the VM eval
 * stack.
 *
 * All our stacks are black boxes, so we can't do much categorization.
 */
bool record_cpp_stack_mem_event(const void* /*addr*/,
                                StructuredLogEntry& record) {
  record.setStr("location", "cpp_stack");
  return true;
}
bool record_vm_stack_mem_event(const void* /*addr*/,
                               StructuredLogEntry& record) {
  record.setStr("location", "vm_stack");
  return true;
}

}

///////////////////////////////////////////////////////////////////////////////

void record_perf_mem_event(PerfEvent kind, const perf_event_sample* sample) {
  perf_event_pause();
  SCOPE_EXIT { perf_event_resume(); };

  auto const addr = reinterpret_cast<const void*>(sample->addr);

  auto record = StructuredLogEntry{};
  record.setInt("version", kVersion);
  record.setStr("event", [&] {
    switch (kind) {
      case PerfEvent::Load:  return "load";
      case PerfEvent::Store: return "store";
    }
    not_reached();
  }());
  record.setInt("addr", uintptr_t(addr));

  auto const data_src = sample->tail()->data_src;
  auto const info = perf_event_data_src(kind, data_src);
  record.setInt("data_src", data_src);
  record.setStr("mem_lvl", info.mem_lvl);
  record.setStr("tlb", info.tlb);
  record.setInt("mem_hit", info.mem_hit);
  record.setInt("snoop", info.snoop);
  record.setInt("snoop_hit", info.snoop_hit);
  record.setInt("snoop_hitm", info.snoop_hitm);
  record.setInt("locked", info.locked);
  record.setInt("tlb_hit", info.tlb_hit);

  auto const should_log = [&] {
    auto const tca = reinterpret_cast<TCA>(const_cast<void*>(addr));

    if (addr == nullptr) {
      return false;
    }
    if (jit::mcgen::initialized() && jit::tc::code().isValidCodeAddress(tca)) {
      return record_tc_mem_event(tca, record);
    }
    if (uintptr_t(addr) <= kLowArenaMaxAddr) {
      return record_low_mem_event(addr, record);
    }
    if (uintptr_t(addr) - s_stackLimit < s_stackSize) {
      return record_cpp_stack_mem_event(addr, record);
    }
    if (isValidVMStackAddress(addr)) {
      return record_vm_stack_mem_event(addr, record);
    }

    if (auto const obj = tl_heap->find(addr)) {
      return record_request_heap_mem_event(addr, obj, record);
    }
    if (tl_heap->contains(const_cast<void*>(addr))) {
      record.setStr("location", "request_heap");
      return true;
    }

    record.setStr("location", "(unknown)");
    return true;
  }();

  if (should_log) {
    // Symbolize the callchain for the event.
    auto const st = StackTrace(
      reinterpret_cast<void* const*>(sample->ips),
      sample->nr
    );
    auto frames = std::vector<folly::StringPiece>{};
    folly::split('\n', st.toString(), frames);
    record.setVec("stacktrace", frames);

    FTRACE(1, "perf_mem_event: {}\n", show(record).c_str());
    StructuredLog::log("hhvm_mem_access", record);
  }
}

///////////////////////////////////////////////////////////////////////////////

}
