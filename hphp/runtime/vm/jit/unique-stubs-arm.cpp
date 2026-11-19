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

#include "hphp/runtime/vm/jit/unique-stubs-arm.h"

#include "hphp/runtime/vm/jit/abi-arm.h"
#include "hphp/runtime/vm/jit/align-arm.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/code-gen-tls.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"

#include "hphp/util/address-range.h"

namespace HPHP::jit {

TRACE_SET_MOD(ustubs)

///////////////////////////////////////////////////////////////////////////////

namespace arm {

///////////////////////////////////////////////////////////////////////////////

static void alignJmpTarget(CodeBlock& cb) {
  align(cb, nullptr, Alignment::JmpTarget, AlignContext::Dead);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Helper for the freeLocalsHelpers which does the actual work of decrementing
 * a value's refcount or releasing it.
 *
 * This helper is reached via call from the various freeLocalHelpers.
 * It expects `dataVal' to be a counted Value, and `typeVal' to be the
 * associated DataType.
 *
 * The `live' registers must be preserved across any native calls (and
 * generally left untouched).
 */
static TCA emitDecRefHelper(CodeBlock& cb, DataBlock& data, CGMeta& fixups,
                            PhysReg dataVal, PhysReg typeVal, RegSet live) {
  return vwrap(cb, data, fixups, [&] (Vout& v) {
    // Set up frame linkage to avoid an indirect fixup.
    v << stublogue{true};
    v << copy{rsp(), rfp()};

    auto destroy = [&](Vout& v) {
      // Note that the stack is aligned since we called to this helper from an
      // stack-unaligned stub.
      PhysRegSaver prs{v, live};

      // The refcount is exactly 1; release the value.
      // Avoid 'this' pointer overwriting by reserving it as an argument.
      // There's no need for a fixup, because we setup a frame on the c++
      // stack.
      assertx(dataVal == rarg(0));
      v << callm{lookupDestructor(v, typeVal, true), arg_regs(1)};
      // fallthru
    };

    auto const sf = emitCmpRefCount(v, OneReference, dataVal);

    ifThen(v, CC_NE, sf, [&] (Vout& v) {
      emitDecRefCount(v, dataVal);
      v << stubret{live, true};
    });

    destroy(v);
    v << stubret{live, true};
  });
}

TCA emitFreeLocalsHelpers(CodeBlock& cb, DataBlock& data, UniqueStubs& us) {
  // The address of the first local's type is passed in the second
  // argument register. The address of the first local's value is
  // passed in the third argument register. We use the first to store
  // the loaded value (so its already in the right place when calling
  // the release function).  We use the fourth as a scratch register
  // for loading the type and the fifth as a scratch register for
  // storing the end pointer.
  auto const dataVal = rarg(0);
  auto const typePtr = rarg(1);
  auto const dataPtr = rarg(2);
  auto const typeVal = rarg(3);
  auto const end = rarg(4);

  CGMeta fixups;
  TCA freeLocalsHelpers[kNumFreeLocalsHelpers];
  TCA freeManyLocalsHelper;

  // This stub is very hot; keep it cache-aligned.
  align(cb, &fixups, Alignment::CacheLine, AlignContext::Dead);
  auto const release = emitDecRefHelper(
    cb, data, fixups, dataVal, typeVal,
    dataPtr | typePtr | end
  );

  auto const decref_local = [&] (Vout& v, Vptr d, Vptr t) {
    auto const sf = v.makeReg();

    // We can't do a byte load here---we have to sign-extend since we use
    // `type' as a 64-bit array index to the destructor table.
    v << loadsbq{t, typeVal};
    auto const cc = emitIsTVTypeRefCounted(v, sf, typeVal);

    ifThen(v, cc, sf, [&] (Vout& v) {
      v << load{d, dataVal};
      auto const counted = v.makeReg();
      v << shrqi{int(kSharedMaxShift), dataVal, v.makeReg(), counted};
      ifThen(v, CC_NZ, counted, [&] (Vout& v) {
        v << call{release, dataVal | typeVal};
      });
    });
  };

  alignJmpTarget(cb);

  freeManyLocalsHelper = vwrap(cb, data, [&] (Vout& v) {
    // Calculate end before modifying the frame pointer
    v << lea{ptrToLocalType(rvmfp(), kNumFreeLocalsHelpers - 1), end};

    // Set up frame linkage to avoid an indirect fixup.
    v << copy{rsp(), rfp()};

    // We always unroll the final `kNumFreeLocalsHelpers' decrefs, so only loop
    // until we hit that point.
    doWhile(v, CC_NZ, {}, [&](const VregList&, const VregList&) {
      decref_local(v, *dataPtr, *typePtr);
      auto const sf = v.makeReg();
      prevLocal(v, typePtr, dataPtr, typePtr, dataPtr);
      v << cmpq{typePtr, end, sf};
      return sf;
    });
  });

  for (auto i = kNumFreeLocalsHelpers - 1; i >= 0; --i) {
    freeLocalsHelpers[i] = vwrap(cb, data, [&] (Vout& v) {
      decref_local(v, *dataPtr, *typePtr);
      if (i != 0) {
        prevLocal(v, typePtr, dataPtr, typePtr, dataPtr);
        v << fallthru{typePtr | dataPtr};
      }
    });
  }

  // All the stub entrypoints share the same ret.
  vwrap(cb, data, fixups, [] (Vout& v) {
    v << stubret{RegSet{}, true};
  });

  // Create a table of branches
  us.freeManyLocalsHelper = vwrap(cb, data, [&] (Vout& v) {
    v << stublogue{true};

    // rvmfp() is needed by the freeManyLocalsHelper stub above, so frame
    // linkage setup is deferred until after its use in freeManyLocalsHelper.
    v << jmpi{freeManyLocalsHelper};
  });
  for (auto i = kNumFreeLocalsHelpers - 1; i >= 0; --i) {
    us.freeLocalsHelpers[i] = vwrap(cb, data, [&] (Vout& v) {
      // We set up frame linkage to avoid an indirect fixup.
      v << stublogue{true};
      v << copy{rsp(), rfp()};
      v << jmpi{freeLocalsHelpers[i]};
    });
  }

  // FIXME: This stub is hot, so make sure to keep it small.
#if 0
  always_assert(Stats::enabled() ||
                (cb.frontier() - release <= 4 * x64::cache_line_size()));
#endif

  fixups.process(nullptr);
  return release;
}

///////////////////////////////////////////////////////////////////////////////

TCA emitCallToExit(CodeBlock& cb, DataBlock& data, const UniqueStubs& us) {
  return vwrap(cb, data, [&] (Vout& v) {
    v << jmpi{us.enterTCExit};
  });
}

///////////////////////////////////////////////////////////////////////////////

}}
