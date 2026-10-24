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

#include "hphp/runtime/vm/async-flow-stepper.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/ext/asio/ext_asio.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/vm-regs.h"

namespace HPHP {

TRACE_SET_MOD(debuggerflow)

namespace {
c_WaitableWaitHandle *objToWaitableWaitHandle(const Object& o) {
  assertx(o->instanceof(c_WaitableWaitHandle::classof()));
  return static_cast<c_WaitableWaitHandle*>(o.get());
}
}

bool AsyncFlowStepper::isActRecOnAsyncStack(const ActRec* target) {
  auto currentWaitHandle = HHVM_FN(asio_get_running)();
  if (currentWaitHandle.isNull()) return false;

  hphp_hash_set<c_WaitableWaitHandle*> visited;
  auto wh = objToWaitableWaitHandle(currentWaitHandle);

  // Skip the current wait handle
  wh = wh->getParentChain().firstInContext(wh->getContextStateIndex());

  while (wh != nullptr && !visited.contains(wh)) {
    visited.insert(wh);

    if (wh->getKind() == c_Awaitable::Kind::AsyncFunction &&
        wh->asAsyncFunction()->actRec() == target) {
      return true;
    }
    wh = wh->getParentChain().firstInContext(wh->getContextStateIndex());
  }
  return false;
}

// Setup async stepping if needed.
void AsyncFlowStepper::setup() {
  auto const func = vmfp()->func();
  auto const unit = func != nullptr ? func->unit() : nullptr;
  if (unit == nullptr) {
    return;
  }
  if (!func->isAsyncFunction()) {
    // Only perform async stepping inside async function.
    return;
  }

  auto const pc = vmpc();
  SourceLoc source_loc;
  if (!func->getSourceLoc(func->offsetOf(pc), source_loc)) {
    TRACE(5, "Could not grab the current line number\n");
    return;
  }
  auto const line = source_loc.line1;

  OffsetFuncRangeVec curLineRanges;
  if (!unit->getOffsetRanges(line, curLineRanges)) {
    curLineRanges.clear();
  }

  auto const awaitOpcodeFilter = [](Op op) { return op == OpAwait; };
  m_stepRangeFlowFilter.addRanges(
    curLineRanges
  );
  m_awaitOpcodeBreakpointFilter.addRanges(
    curLineRanges,
    awaitOpcodeFilter
  );
  // So that we can get notified when hitting await instruction.
  auto bpFilter = getBreakPointFilter();
  bpFilter->addRanges(
    curLineRanges,
    awaitOpcodeFilter
  );
  updateStepStartStackDepth();
  m_stage = AsyncStepperStage::StepOver;
}

AsyncStepHandleOpcodeResult AsyncFlowStepper::handleOpcode(PC pc) {
  auto ret = AsyncStepHandleOpcodeResult::Unhandled;
  switch (m_stage) {
    case AsyncStepperStage::Disabled:
    break;

    case AsyncStepperStage::StepOver:
    {
      // Check if we are executing "await" instruction.
      if (m_awaitOpcodeBreakpointFilter.checkPC(pc)) {
        auto wh = c_Awaitable::fromTV(*vmsp());
        // Is "await" blocked?
        if (wh && !wh->isFinished()) {
          handleBlockedAwaitOpcode(pc);
        }
        // else { no special action for non-blocked await. }
        ret = AsyncStepHandleOpcodeResult::Handled;
      } else if (isCompleted(pc)) {
        reset();
        ret = AsyncStepHandleOpcodeResult::Completed;
      }
      // else { let normal step-over logic to handle. }
    }
    break;

    case AsyncStepperStage::StepOverAwait:
    {
      captureResumeIdAfterAwait();
      // We have set internal breakpoint in resume block
      // and captured the resumeId,
      // do not need interrupt anymore.
      RID().setDebuggerIntr(false);
      m_stage = AsyncStepperStage::WaitResume;
      ret = AsyncStepHandleOpcodeResult::Handled;
    }
    break;

    case AsyncStepperStage::WaitResume:
    {
      // We are waiting for resume internal breakpoint to
      // trigger do not let other steppers handle.
      ret = AsyncStepHandleOpcodeResult::Handled;
      if (didResumeBreakpointTrigger(pc)) {
        // Finished await resume, start normal step-over again.
        updateStepStartStackDepth();
        m_stage = AsyncStepperStage::StepOver;
      }
    }
    break;

    default:
      not_reached();
  }
  return ret;
}

void AsyncFlowStepper::handleExceptionThrown() {
  m_isCurrentAsyncStepException = isActRecOnAsyncStack(m_asyncResumableId);
}

bool AsyncFlowStepper::handleExceptionHandler() {
  // Check if the throwing exception will break out of
  // current async stepping or not; if true we need
  // finish the async stepping in the exception handler.
  if (m_stage == AsyncStepperStage::WaitResume &&
    m_isCurrentAsyncStepException &&
    !isActRecOnAsyncStack(m_asyncResumableId)) {
    reset();
    return true;
  }
  return false;
}

// Called when hitting a blocked "await" opcode.
// Async stepping resume internal breakpoint needs
// two parts to check triggering:
//  1. m_asyncResumableId matches.
//  2. breakpoint pc matches.
// #1 is required because this async function may be called multiple times
// and we need m_asyncResumableId to distinguish if the triggered resume
// breakpoint is the one we are stepping.
// m_asyncResumableId uses ActRec address as id so for eager execution mode
// we need step over "await" opcode to get the final ActRec address on heap.
void AsyncFlowStepper::handleBlockedAwaitOpcode(PC pc) {
  TRACE(2, "AsyncFlowStepper: encountered blocking await\n");
  setResumeInternalBreakpoint(pc);
  auto fp = vmfp();
  if (isResumed(fp)) {
    // Already in resumed execution mode.
    m_asyncResumableId = fp;
    m_stage = AsyncStepperStage::WaitResume;
  } else {
    // In eager execution non-resumed mode we need to step over "await" opcode
    // then grab async function's migrated ActRec on heap as m_asyncResumableId.
    stepOverAwaitOpcode();
    m_stage = AsyncStepperStage::StepOverAwait;
  }
}

// Used by eager execution mode to step over "await" opcode
// so that the async frame's ActRec is migrated to heap.
// This is required because async frame ActRec's address
// is used for m_asyncResumableId.
void AsyncFlowStepper::stepOverAwaitOpcode() {
  assertx(vmfp()->func()->isAsyncFunction());
  m_stage = AsyncStepperStage::StepOverAwait;

  // Request interrupt opcode callback after "await" instruction
  // so that we can increase to next stage.
  RID().setDebuggerIntr(true);
}

// Should only be called immediately after "await" opcode
// to capture m_asyncResumableId.
void AsyncFlowStepper::captureResumeIdAfterAwait() {
  auto topObj = vmsp()->m_data.pobj;
  auto wh = static_cast<c_AsyncFunctionWaitHandle*>(topObj);
  m_asyncResumableId = wh->actRec();
}

// Set guard internal breakpoint at async operation
// resume point to continue stepping.
void AsyncFlowStepper::setResumeInternalBreakpoint(PC pc) {
  assertx(decode_op(pc) == Op::Await);

  auto resumeInstPc = pc + instrLen(pc);
  assertx(vmfp()->func()->offsetOf(resumeInstPc) != kInvalidOffset);

  TRACE(2, "Setup internal breakpoint after await at '%s' offset %d\n",
    vmfp()->func()->fullName()->data(),
    vmfp()->func()->offsetOf(resumeInstPc));
  m_resumeBreakpointFilter.addPC(resumeInstPc);

  auto bpFilter = getBreakPointFilter();
  bpFilter->addPC(resumeInstPc);

  m_stage = AsyncStepperStage::WaitResume;
}

bool AsyncFlowStepper::didResumeBreakpointTrigger(PC pc) {
  if (!UNLIKELY(m_resumeBreakpointFilter.checkPC(pc))) {
    return false;
  }
  return m_asyncResumableId == getAsyncResumableId(vmfp());
}

const ActRec* AsyncFlowStepper::getAsyncResumableId(const ActRec* fp) {
  assertx(isResumed(fp));
  assertx(fp->func()->isResumable());
  return fp;
}

void AsyncFlowStepper::updateStepStartStackDepth() {
  auto& req_data = RID();
  int stackDepth = req_data.getDebuggerStackDepth();
  if (stackDepth == 0) {
    return;
  }
  m_stepStartStackDepth = stackDepth;
}

// Return 'true' if stepping has completed.
bool AsyncFlowStepper::isCompleted(PC pc) {
  const auto stackDisp = getStackDisposition(m_stepStartStackDepth);
  // Step completes when pc is outside of step range and
  // is not in deeper frames(e.g. step-into).
  return stackDisp != StackDepthDisposition::Deeper &&
    !m_stepRangeFlowFilter.checkPC(pc);
}

void AsyncFlowStepper::reset() {
  m_stage = AsyncStepperStage::Disabled;
  m_asyncResumableId = nullptr;
  m_stepRangeFlowFilter.clear();
  m_awaitOpcodeBreakpointFilter.clear();
  m_resumeBreakpointFilter.clear();
}

} // namespace HPHP
