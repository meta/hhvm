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
#include "hphp/runtime/debugger/debugger_proxy.h"

#include <exception>
#include <map>
#include <stack>
#include <vector>

#include <fmt/format.h>
#include <folly/Conv.h>

#include "hphp/runtime/debugger/cmd/cmd_auth.h"
#include "hphp/runtime/debugger/cmd/cmd_interrupt.h"
#include "hphp/runtime/debugger/cmd/cmd_flow_control.h"
#include "hphp/runtime/debugger/cmd/cmd_signal.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/runtime/debugger/debugger_hook_handler.h"
#include "hphp/runtime/debugger/dummy_sandbox.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/ext/sockets/ext_sockets.h"
#include "hphp/runtime/vm/vm-regs.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/logger.h"
#include "hphp/util/process.h"

namespace HPHP::Eval {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(debugger)

DebuggerProxy::DebuggerProxy(req::ptr<Socket> socket, bool local)
    : m_local(local),
      m_signalThread(this, &DebuggerProxy::pollSignal),
      m_signum(CmdSignal::SignalNone) {
  TRACE(2, "DebuggerProxy::DebuggerProxy\n");
  m_thrift.create(socket);
  m_dummyInfo = DSandboxInfo::CreateDummyInfo((int64_t)this);
  Variant address;
  Variant port;
  std::string clientDetails;
  if (!local) {
    if (getClientConnectionInfo(address, port)) {
      clientDetails = fmt::format("From {}:{}",
                                          address.toString().data(),
                                          (int)port.toInt64());
    } else {
      clientDetails = "Failed to get client connection details";
    }
  } else {
    clientDetails = "Local script connection";
  }
  Debugger::UsageLog("server", m_dummyInfo.id(), "connect", clientDetails);
}

// Cleanup all resources owned by this proxy, including any threads it
// owns. If a thread doesn't stop, return false so we can try again
// later. This can be called multiple times, and this function leaves
// the proxy usable by request threads which may still be handling an
// interrupt.
bool DebuggerProxy::cleanup(int timeout) {
  TRACE_RB(2, "DebuggerProxy::cleanup starting\n");
  // If we're not already marked as stopping then there may be other
  // threads still attempting to use this object!
  assertx(m_stopped);
  // No more client operation is possible, so drop the connection.
  m_thrift.close();
  TRACE(2, "Stopping signal thread...\n");
  if (!m_signalThread.waitForEnd(timeout)) return false;
  TRACE(2, "Stopping dummy sandbox...\n");
  if (m_dummySandbox) {
    if (!m_dummySandbox->stop(timeout)) return false;
    delete m_dummySandbox;
    m_dummySandbox = nullptr;
  }
  TRACE_RB(2, "DebuggerProxy::cleanup complete\n");
  return true;
}

// Stop the proxy and ensure that no new uses of it can
// occur. Schedule it for final cleanup on another thread. This may be
// called from any thread wishing to stop the proxy, for any reason.
void DebuggerProxy::stop() {
  // Shared ref to keep us alive. Often the only reference to a proxy
  // is in the debugger's proxy map, which we're about to alter below.
  auto this_ = shared_from_this();
  Debugger::UsageLog("server", getSandboxId(), "disconnect");

  // After this we'll no longer be able to get this_ from
  // Debugger::findProxy(), which means no new interrupts for this
  // proxy. There may still be threads in existing interrupts, though.
  Debugger::RemoveProxy(this_);

  // Pop the signal polling thread, and any interrupting threads, out
  // of their event loops and cause them to exit.
  DSandboxInfo invalid;
  {
    Lock lock(this);
    m_sandbox = invalid;
    m_stopped = true;
  }

  // Retire the proxy, which will schedule it for cleanup and
  // destruction on another thread. Threads for the dummy sandbox and
  // signal polling can't destroy the proxy directly, since the proxy
  // owns them and will want to destroy them.
  Debugger::RetireProxy(this_);
}

// Stop the proxy, and stop execution of the current request.
void DebuggerProxy::stopAndThrow() {
  stop();
  throw DebuggerClientExitException();
}

const char *DebuggerProxy::getThreadType() const {
  TRACE(2, "DebuggerProxy::getThreadType\n");
  return isLocal() ? "Command Line Script" : "Dummy Sandbox";
}

DSandboxInfo DebuggerProxy::getSandbox() {
  TRACE(2, "DebuggerProxy::getSandbox\n");
  Lock lock(this);
  return m_sandbox;
}

std::string DebuggerProxy::getSandboxId() {
  TRACE(2, "DebuggerProxy::getSandboxId\n");
  Lock lock(this);
  return m_sandbox.id();
}

// Forms a list of all threads which are currently blocked within this
// proxy. There is the thread currently processing an interrupt, plus
// any other threads stacked up in blockUntilOwn() (represented in the
// m_threads set).
void DebuggerProxy::getThreads(std::vector<DThreadInfoPtr> &threads) {
  TRACE(2, "DebuggerProxy::getThreads\n");
  Lock lock(this);
  auto& interrupts = RID().interrupts;
  assertx(!interrupts.empty());
  if (!interrupts.empty()) {
    CmdInterrupt *tint = (CmdInterrupt*)interrupts.top();
    assertx(tint);
    if (tint) {
      threads.push_back(createThreadInfo(tint->desc()));
    }
  }
  for (std::map<int64_t, DThreadInfoPtr>::const_iterator iter =
         m_threads.begin(); iter != m_threads.end(); ++iter) {
    DThreadInfoPtr thread(new DThreadInfo());
    *thread = *iter->second;
    threads.push_back(thread);
  }
}

// Switch this proxy to debug the given sandbox.
bool DebuggerProxy::switchSandbox(const std::string &newId, bool force) {
  TRACE(2, "DebuggerProxy::switchSandbox\n");
  return Debugger::SwitchSandbox(shared_from_this(), newId, force);
}

// Callback made by Debugger::SwitchSandbox() when the switch is successful.
// NB: this is called with a read lock on the corresponding entry in the sandbox
// map.
void DebuggerProxy::updateSandbox(DSandboxInfoPtr sandbox) {
  TRACE(2, "DebuggerProxy::updateSandbox\n");
  Lock lock(this);
  if (sandbox) {
    if (m_sandbox.id() != sandbox->id()) {
      m_sandbox = *sandbox;
    } else {
      m_sandbox.update(*sandbox);
    }
  }
}

// Cause the proxy to debug the given thread. Other threads will stack
// up in blockUntilOwn() (depending on the thread mode).
bool DebuggerProxy::switchThread(DThreadInfoPtr thread) {
  TRACE(2, "DebuggerProxy::switchThread\n");
  Lock lock(this);
  if (m_threads.find(thread->m_id) != m_threads.end()) {
    m_newThread = thread;
    return true;
  }
  return false;
}

// Change the thread mode, and mark the given (or calling) thread as
// the current thread for this proxy, depending on the thread mode.
//
// We have three thread modes: Normal, Sticky, and Exclusive. For more
// details, see the help for CmdThread. In short, in Normal mode any
// thread may hit and process an interrupt. The first thread to hit an
// interrupt becomes the current thread, and all others block until
// the current thread is done with its interrupt.
//
// In Exclusive mode, only the current thread may process interrupts;
// All others will ignore interrupts. In Sticky mode, the current
// thread continues to block others (even while running) until the
// mode is switched back to Normal.
void DebuggerProxy::switchThreadMode(ThreadMode mode,
                                     int64_t threadId /* = 0 */) {
  TRACE(2, "DebuggerProxy::switchThreadMode\n");
  Lock lock(this);
  m_threadMode = mode;
  if (threadId) {
    m_thread = threadId;
    notifyAll(); // since threadId != 0, we're still waking up just one thread
  } else if (mode == Normal) {
    m_thread = 0;
    notify();
  } else {
    m_thread = (int64_t)Process::GetThreadId();
  }
  TRACE(2, "Current thread is now %" PRIx64 "\n", m_thread);
}

void DebuggerProxy::startDummySandbox() {
  Lock lock(this);
  if (m_stopped) return;

  TRACE(2, "DebuggerProxy::startDummySandbox\n");
  m_dummySandbox =
    new DummySandbox(this, Cfg::Debugger::DefaultSandboxPath,
                     Cfg::Debugger::StartupDocument);
  m_dummySandbox->start();
}

void DebuggerProxy::notifyDummySandbox() {
  TRACE(2, "DebuggerProxy::notifyDummySandbox\n");
  if (m_dummySandbox) m_dummySandbox->notifySignal(CmdSignal::SignalBreak);
}

void DebuggerProxy::setBreakPoints(
  std::vector<BreakPointInfoPtr>& breakpoints
) {
  TRACE(2, "DebuggerProxy::setBreakPoints\n");
  // Hold the break mutex while we update the proxy's state. There's no need to
  // hold it over the longer operation to set breakpoints in each file later.
  {
    WriteLock lock(m_breakMutex);
    // breakpoints holds a list of fresh new BreakPointInfo objects that
    // have been deserialized from the client's list of breakpoints.
    // The existing BreakPointInfo objects may include non empty values
    // for m_stack. If these get thrown away, breakpoints that are temporarily
    // disabled will suddenly fire again, which is not what we want to
    // happen if we create a new breakpoint or just even list breakpoints.
    auto it = m_breakpoints.begin();
    for (auto it1 = breakpoints.begin(); it1 != breakpoints.end(); ++it1)  {
      BreakPointInfoPtr newBreakPoint = *it1;
      do {
        for (auto it2 = it; it2 != m_breakpoints.end(); ) {
          BreakPointInfoPtr oldBreakPoint = *it2++;
          if (oldBreakPoint->same(newBreakPoint)) {
            newBreakPoint->transferStack(oldBreakPoint);
            it = it2;
            goto next_breakpoint;
          }
        }
        if (it == m_breakpoints.begin()) goto next_breakpoint;
        // Only searched a part of m_breakpoints. Reset it and try again.
        it = m_breakpoints.begin();
      } while (true);
      next_breakpoint: continue;
    }
    m_breakpoints = breakpoints;
    m_hasBreakPoints = !m_breakpoints.empty();
  }
  proxySetBreakPoints(this);
}

void DebuggerProxy::getBreakPoints(
    std::vector<BreakPointInfoPtr> &breakpoints) {
  TRACE(7, "DebuggerProxy::getBreakPoints\n");
  ReadLock lock(m_breakMutex);
  breakpoints = m_breakpoints;
}

// Proxy needs to be interrupted because it has something setup which may need
// processing; breakpoints, flow control commands, a signal.
bool DebuggerProxy::needInterrupt() {
  TRACE(2, "DebuggerProxy::needInterrupt\n");
  return m_hasBreakPoints || m_flow || m_signum != CmdSignal::SignalNone;
}

// We need VM interrupts if we're in a state that requires the debugger to be
// interrupted for every opcode.
bool DebuggerProxy::needVMInterrupts() {
  TRACE(2, "DebuggerProxy::needVMInterrupts\n");
  bool flowNeedsInterrupt = (m_flow && m_flow->needsVMInterrupt());
  return flowNeedsInterrupt || m_signum != CmdSignal::SignalNone;
}

// Handle an interrupt from the VM.
void DebuggerProxy::interrupt(CmdInterrupt &cmd) {
  TRACE_RB(2, "DebuggerProxy::interrupt\n");
  // Make any breakpoints that have passed breakable again.
  setBreakableForBreakpointsNotMatching(cmd);

  // At this point we have an interrupt, but we don't know if we're on the
  // thread the proxy considers "current".
  // NB: BreakPointReached really means we've got control of a VM thread from
  // the opcode hook. This could be for a breakpoint, stepping, etc.

  // Wait until this thread is the one this proxy wants to debug.
  if (!blockUntilOwn(cmd, true)) return;

  // We know we're on the "current" thread, so we can process any active flow
  // command, stop if we're at a breakpoint, handle other interrupts, etc.
  if (checkFlowBreak(cmd)) {
    // We've hit a breakpoint and now need to make sure that breakpoints
    // won't be hit again for this site until control leaves this site.
    // (Breakpoints can still get hit if control reaches this site during
    // a call that is part of this site because the flags are stacked.)
    unsetBreakableForBreakpointsMatching(cmd);
    while (true) {
      try {
        // We're about to send the client an interrupt and start
        // waiting for commands back from it. Disable signal polling
        // during this time, since our protocol requires that only one
        // thread talk to the client at a time.
        disableSignalPolling();
        SCOPE_EXIT { enableSignalPolling(); };
        processInterrupt(cmd);
      } catch (const DebuggerException& ) {
        TRACE(2, "DebuggerException from processInterrupt!\n");
        switchThreadMode(Normal);
        throw;
      } catch (...) {
        TRACE(2, "Unknown exception from processInterrupt!\n");
        assertx(false); // no other exceptions should be seen here
        switchThreadMode(Normal);
        throw;
      }
      if (cmd.getInterruptType() == PSPEnded) break;
      if (!m_newThread) break; // we're not switching threads
      switchThreadMode(Normal, m_newThread->m_id);
      m_newThread.reset();
      blockUntilOwn(cmd, false);
    }
  }

  if ((m_threadMode == Normal) || (cmd.getInterruptType() == PSPEnded)) {
    // If the thread mode is Normal we let other threads with
    // interrupts go ahead and process them. We also do this when the
    // thread is at PSPEnded because the thread is done.
    switchThreadMode(Normal);
  }
}

// Sends a copy of the given command to the associated client
// using the buffer in m_thrift. Returns false if an exception
// occurs during the send (typically a socket error).
bool DebuggerProxy::sendToClient(DebuggerCommand *cmd) {
  TRACE(2, "DebuggerProxy::sendToClient\n");
  return cmd->send(m_thrift);
}

// Allow the signal polling thread to send CmdSignal messages to the
// client to see if it there is a signal to repond to.
void DebuggerProxy::enableSignalPolling() {
  Lock lock(m_signalMutex);
  m_okayToPoll = true;
}

// Prevent the signal polling thread from sending CmdSignal messages
// to the client. Polling is disabled while a thread is stopped at an
// interrupt and responding to messages from the client.
void DebuggerProxy::disableSignalPolling() {
  Lock lock(m_signalMutex);
  m_okayToPoll = false;
  // Drop any pending signal since we're about to start processing
  // interrupts again anyway.
  m_signum = CmdSignal::SignalNone;
}

void DebuggerProxy::startSignalThread() {
  Lock lock(this);
  if (m_stopped) return;

  TRACE(2, "DebuggerProxy::startSignalThread\n");
  m_signalThread.start();
}

// This gets it's own thread, and polls the client once per second to see if
// there is a signal, i.e., if the user has pressed Ctrl-C, etc. If there is a
// signal, it is passed as an interrupt to the proxy in an attempt to get other
// threads in the sandbox to stop.
//
// If another thread in the sandbox fails to stop and consume the signal then
// it will be passed to the dummy sandbox instead.
void DebuggerProxy::pollSignal() {
  TRACE_RB(2, "DebuggerProxy::pollSignal: starting\n");
  int signalTimeout = Cfg::Debugger::SignalTimeout;
  while (!m_stopped) {
    sleep(1);

    // Block any threads that might be interrupting from communicating with the
    // client until we're done with this poll.
    Lock lock(m_signalMutex);

    // After DebuggerSignalTimeout seconds that no active thread picks
    // up the signal, we send it to dummy sandbox.
    if ((m_signum != CmdSignal::SignalNone) && m_dummySandbox &&
        (--signalTimeout <= 0)) {
      TRACE_RB(2, "DebuggerProxy::pollSignal: sending to dummy sandbox\n");
      m_dummySandbox->notifySignal(m_signum);
      m_signum = CmdSignal::SignalNone;
    }

    // Don't actually poll if another thread is already in a command
    // processing loop with the client.
    if (!m_okayToPoll) continue;

    // Send CmdSignal over to the client and wait for a response.
    CmdSignal cmd;
    if (!cmd.onServer(*this)) {
      TRACE_RB(2, "DebuggerProxy::pollSignal: "
               "Failed to send CmdSignal to client\n");
      break;
    }

    // We've sent the client a command, and we expect an immediate
    // response. Wait 10 times to give it a chance on especially
    // overloaded computers.
    DebuggerCommandPtr res;
    for (int i = 0; i < 10; i++) {
      if (DebuggerCommand::Receive(m_thrift, res,
                                   "DebuggerProxy::pollSignal()", true, true)) break;
      if (m_stopped) {
        TRACE_RB(2, "DebuggerProxy::pollSignal: "
                 "signal thread asked to stop while waiting "
                 "for CmdSignal back from the client\n");
        break;
      }
    }
    if (!res) {
      if (!m_stopped) {
        TRACE_RB(2, "DebuggerProxy::pollSignal: "
                 "Failed to get CmdSignal back from client\n");
      }
      break;
    }

    auto sig = std::dynamic_pointer_cast<CmdSignal>(res);
    if (!sig) {
      TRACE_RB(2, "DebuggerProxy::pollSignal: "
               "bad response from signal polling: %d", res->getType());
      break;
    }

    auto newSignum = sig->getSignal();

    if (newSignum != CmdSignal::SignalNone) {
      TRACE_RB(2, "DebuggerProxy::pollSignal: "
               "got interrupt signal from client\n");
      m_signum = newSignum;
      signalTimeout = Cfg::Debugger::SignalTimeout;
      Debugger::RequestInterrupt(shared_from_this());
    }
  }
  if (!m_stopped) {
    // We've noticed that the socket has closed. Stop and destory this proxy.
    TRACE_RB(2, "DebuggerProxy::pollSignal: "
             "lost communication with the client, stopping proxy\n");
    Debugger::UsageLog("server", getSandboxId(), "ProxyError", "Signal poll");
    stop();
  }
  TRACE_RB(2, "DebuggerProxy::pollSignal: ended\n");
}

// Grab the ip address and port of the client that is connected to this proxy.
bool DebuggerProxy::getClientConnectionInfo(Variant& address,
                                            Variant& port) {
  OptResource s(m_thrift.getSocket().get());
  return HHVM_FN(socket_getpeername)(s, address, port);
}

///////////////////////////////////////////////////////////////////////////////
// helpers

namespace {

// Passed to the ExecutionContext during Eval to add writes to stdout
// to the output buffer string.
struct DebuggerStdoutHook final : ExecutionContext::StdoutHook {
  StringBuffer& sb;
  explicit DebuggerStdoutHook(StringBuffer& sb) : sb(sb) {}
  void operator()(const char* s, int len) override {
    TRACE(2, "DebuggerProxy::append_stdout\n");
    sb.append(s, len);
  }
};

// Passed to the ExecutionContext during Eval to stream writes to stdout
// to the output buffer string.
struct DebuggerStreamStdoutHook final : ExecutionContext::StdoutHook {
  DebuggerProxy& proxy;
  DebuggerCommand& cmd;
  OptString &output;
  StreamStatus& stream_status;
  explicit DebuggerStreamStdoutHook(DebuggerProxy& proxy,
                                    DebuggerCommand &cmd,
                                    OptString &output,
                                    StreamStatus& stream_status)
    : proxy(proxy), cmd(cmd), output(output), stream_status(stream_status) {}
  void operator()(const char* s, int len) override {
    TRACE(2, "DebuggerProxy::stream_stdout\n");
    StringBuffer sb;
    sb.append(s, len);
    output = sb.detach();
    stream_status = StreamStatus::ONGOING;
    proxy.sendToClient(&cmd);
  }
};

struct DebuggerLoggerHook final : LoggerHook {
  StringBuffer& sb;
  explicit DebuggerLoggerHook(StringBuffer& sb) : sb(sb) {}
  void operator()(const char* /*hdr*/, const char* msg, const char* ending)
       override {
    TRACE(2, "DebuggerProxy::append_stderr\n");
    if (s_stderr_color) {
      sb.append(s_stderr_color);
    }
    sb.append(msg);
    sb.append(ending);
    if (s_stderr_color) {
      sb.append(ANSI_COLOR_END);
    }
  }
};

}

std::string DebuggerProxy::MakePHP(const std::string &php) {
  TRACE(2, "DebuggerProxy::MakePHP\n");
  return "<?hh " + php + ";";
}

std::string DebuggerProxy::MakePHPReturn(const std::string &php) {
  TRACE(2, "DebuggerProxy::MakePHPReturn\n");
  return "<?hh return " + php + ";";
}

// Record info about the current thread for the debugger client to use
// when listing all threads which are interrupted.
DThreadInfoPtr DebuggerProxy::createThreadInfo(const std::string &desc) {
  TRACE(2, "DebuggerProxy::createThreadInfo\n");
  DThreadInfoPtr info(new DThreadInfo());
  info->m_id = (int64_t)Process::GetThreadId();
  info->m_desc = desc;
  Transport *transport = g_context->getTransport();
  if (transport) {
    info->m_type = transport->getThreadTypeName();
    info->m_url = transport->getCommand();
  } else {
    info->m_type = getThreadType();
  }
  return info;
}

// Waits until this thread is the one that the proxy considers the current
// thread.
// NB: if the thread is not the current thread, and we're asked to check
// breakpoints, then if there are no breakpoints which could effect this
// thread we simply return false and stop processing the current interrupt.
bool DebuggerProxy::blockUntilOwn(CmdInterrupt &cmd, bool check) {
  int64_t self = cmd.getThreadId();
  TRACE(2, "DebuggerProxy::blockUntilOwn for thread %" PRIx64 "\n", self);

  Lock lock(this);
  if (m_thread && m_thread != self) {
    if (check && (m_threadMode == Exclusive || !checkBreakPoints(cmd))) {
      // Flow control commands only belong to sticky thread
      TRACE(2, "No need to interrupt this thread\n");
      return false;
    }
    m_threads[self] = createThreadInfo(cmd.desc());
    while (!m_stopped && m_thread && m_thread != self) {
      TRACE(2, "Waiting...\n");
      wait(1);

      // If for whatever reason, m_thread isn't debugging anymore (for example,
      // it runs in Sticky mode, but it finishes running), kick it out.
      if (m_thread && !Debugger::IsThreadDebugging(m_thread)) {
        TRACE(2, "Old thread abandoned debugging, taking over!\n");
        m_threadMode = Normal;
        m_thread = 0;
        m_newThread.reset();
        m_flow.reset();
      }
    }
    TRACE(2, "Ready to become current thread for this proxy\n");
    m_threads.erase(self);
    if (m_stopped) return false;
    if (!checkBreakPoints(cmd)) {
      // The breakpoint might have been removed while I'm waiting
      return false;
    }
  }

  // This thread is now the one the proxy considers the current thread.
  if (m_thread == 0) {
    TRACE(2, "Updating current thread\n");
    m_thread = self;
  }
  TRACE(2, "Current thread for this proxy tid=%" PRIx64 "\n", self);
  return true;
}

// Checks whether the cmd has any breakpoints that match the current Site.
// Also returns true for cmds that should always break, like SessionStarted,
// and returns true when we have special modes setup for, say, breaking on
// RequestEnded, PSPEnded, etc.
bool DebuggerProxy::checkBreakPoints(CmdInterrupt &cmd) {
  TRACE(2, "DebuggerProxy::checkBreakPoints\n");
  ReadLock lock(m_breakMutex);
  return cmd.shouldBreak(*this, m_breakpoints, getRealStackDepth());
}

// Check if we should stop due to flow control, breakpoints, and signals.
bool DebuggerProxy::checkFlowBreak(CmdInterrupt &cmd) {
  TRACE(2, "DebuggerProxy::checkFlowBreak\n");
  // If there is an outstanding Ctrl-C from the client, go ahead and break now.
  // Note: this stops any flow control command we might have in-flight.
  if (m_signum == CmdSignal::SignalBreak) {
    Lock lock(m_signalMutex);
    if (m_signum == CmdSignal::SignalBreak) {
      m_signum = CmdSignal::SignalNone;
      m_flow.reset();
      return true;
    }
  }

  // Note that even if fcShouldBreak, we still need to test bpShouldBreak
  // so to correctly report breakpoint reached + evaluating watchpoints;
  // vice versa, so to give a flow cmd a chance to react.
  // Note: a Continue cmd is a bit special. We need to process it _only_ if
  // we decide to break.
  bool fcShouldBreak = false; // should I break according to flow control?
  bool bpShouldBreak = false; // should I break according to breakpoints?

  if ((cmd.getInterruptType() == BreakPointReached ||
       cmd.getInterruptType() == HardBreakPoint ||
       cmd.getInterruptType() == ExceptionHandler) && m_flow) {
    if (!m_flow->is(DebuggerCommand::KindOfContinue)) {
      m_flow->onBeginInterrupt(*this, cmd);
      if (m_flow->complete()) {
        fcShouldBreak = true;
        m_flow.reset();
      }
    }
  }

  // NB: this also checks whether we should be stopping at special interrupt
  // sites, like SessionStarted, RequestEnded, ExceptionThrown, etc.
  bpShouldBreak = checkBreakPoints(cmd);

  // This is done before KindOfContinue testing.
  if (!fcShouldBreak && !bpShouldBreak) {
    return false;
  }

  if ((cmd.getInterruptType() == BreakPointReached ||
       cmd.getInterruptType() == HardBreakPoint) && m_flow) {
    if (m_flow->is(DebuggerCommand::KindOfContinue)) {
      m_flow->onBeginInterrupt(*this, cmd);
      if (m_flow->complete()) m_flow.reset();
      return false;
    }
  }

  return true;
}

void DebuggerProxy::checkStop() {
  TRACE(5, "DebuggerProxy::checkStop\n");
  if (m_stopped) stopAndThrow();
}

void DebuggerProxy::processInterrupt(CmdInterrupt &cmd) {
  TRACE_RB(2, "DebuggerProxy::processInterrupt\n");
  // Do the server-side work for this interrupt, which just notifies the client.
  if (!cmd.onServer(*this)) {
    TRACE_RB(1, "Failed to send CmdInterrupt to client\n");
    Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                       "Send interrupt");
    stopAndThrow();
  }

  Debugger::UsageLogInterrupt("server", getSandboxId(), cmd);

  // Wait for commands from the debugger client and process them. We'll stay
  // here until we get a command that should cause the thread to continue.
  while (true) {
    DebuggerCommandPtr res;
    while (!DebuggerCommand::Receive(m_thrift, res,
                                     "DebuggerProxy::processInterrupt()", true, true)) {
      // we will wait forever until DebuggerClient sends us something
      checkStop();
    }
    checkStop();
    if (res) {
      TRACE_RB(2, "Proxy got cmd type %d\n", res->getType());
      Debugger::UsageLog("server", getSandboxId(),
                         folly::to<std::string>(res->getType()));
      // Any control flow command gets installed here and we continue execution.
      m_flow = std::dynamic_pointer_cast<CmdFlowControl>(res);
      if (m_flow) {
        m_flow->onSetup(*this, cmd);
        if (!m_flow->complete()) {
          TRACE_RB(2, "Incomplete flow command %d remaining on proxy for "
                   "further processing\n", m_flow->getType());
          if (m_threadMode == Normal) {
            // We want the flow command to complete on the thread that
            // starts it.
            switchThreadMode(Sticky);
          }
        } else {
          // The flow cmd has determined that it is done with its work and
          // doesn't need to remain for later processing.
          TRACE_RB(2, "Flow command %d completed\n", m_flow->getType());
          m_flow.reset();
        }
        return;
      }
      if (res->is(DebuggerCommand::KindOfQuit)) {
        TRACE_RB(2, "Received quit command\n");
        res->onServer(*this); // acknowledge receipt so that client can quit.
        stopAndThrow();
      }
    }
    bool cmdFailure = false;
    try {
      // Perform the server-side work for this command.
      if (res) {
        if (!res->onServer(*this)) {
          TRACE_RB(1, "Failed to execute cmd %d from client\n", res->getType());
          Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                             "Command failed");
          cmdFailure = true;
        }
      } else {
        TRACE_RB(1, "Failed to receive cmd from client\n");
        Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                           "Command receive failed");
        cmdFailure = true;
      }
    } catch (const DebuggerException& ) {
      throw;
    } catch (const Object &o) {
      Logger::Warning(DEBUGGER_LOG_TAG
                      "Cmd type %d onServer() threw a php exception %s",
                      res->getType(), o->getVMClass()->name()->data());
      Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                         "Command exception");
      cmdFailure = true;
    } catch (const std::exception& e) {
      Logger::Warning(DEBUGGER_LOG_TAG
       "Cmd type %d onServer() threw exception %s", res->getType(), e.what());
      Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                         "Command exception");
      cmdFailure = true;
    } catch (...) {
      Logger::Warning(DEBUGGER_LOG_TAG
       "Cmd type %d onServer() threw non standard exception", res->getType());
      Debugger::UsageLog("server", getSandboxId(), "ProxyError",
                         "Command exception");
      cmdFailure = true;
    }
    if (cmdFailure) stopAndThrow();
    if (res->shouldExitInterrupt()) return;
  }
}

///////////////////////////////////////////////////////////////////////////////

std::pair<bool,Variant>
DebuggerProxy::ExecutePHPWithStreaming(const std::string &php, OptString &output, DebuggerCommand &cmd,
                          int frame, StreamStatus& stream_status, int flags) {
  TRACE(2, "DebuggerProxy::ExecutePHPWithStreaming\n");
  // Wire up stdout and stderr to our own string buffer so we can pass
  // any output back to the client.
  StringBuffer sb;
  StringBuffer *save = g_context->swapOutputBuffer(nullptr);
  DebuggerStreamStdoutHook stdout_hook(*this, cmd, output, stream_status);
  DebuggerLoggerHook stderr_hook(sb);

  auto const previousEvalOutputHook = m_evalOutputHook;
  if (previousEvalOutputHook != nullptr) {
    g_context->removeStdoutHook(previousEvalOutputHook);
  }

  m_evalOutputHook = &stdout_hook;
  g_context->addStdoutHook(&stdout_hook);

  if (flags & ExecutePHPFlagsLog) {
    Logger::SetThreadHook(&stderr_hook);
  }
  SCOPE_EXIT {
    g_context->removeStdoutHook(&stdout_hook);
    g_context->swapOutputBuffer(save);
    if (flags & ExecutePHPFlagsLog) {
      Logger::SetThreadHook(nullptr);
    }

    if (previousEvalOutputHook != nullptr) {
      g_context->addStdoutHook(previousEvalOutputHook);
    }

    m_evalOutputHook = previousEvalOutputHook;
  };
  stream_status = StreamStatus::INITIALIZED;
  OptString code(php.c_str(), php.size(), CopyString);
  // We're about to start executing more PHP. This is typically done
  // in response to commands from the client, and the client expects
  // those commands to send more interrupts since, of course, the
  // user might want to debug the code we're about to run. If we're
  // already processing an interrupt, enable signal polling around
  // the execution of the new PHP to ensure that we can handle
  // signals while doing so.
  //
  // Note: we must switch the thread mode to Sticky so we block
  // other threads which may hit interrupts while we're running,
  // since nested processInterrupt() calls would normally release
  // other threads on the way out.
  assertx(m_thread == (int64_t)Process::GetThreadId());
  ThreadMode origThreadMode = m_threadMode;
  switchThreadMode(Sticky, m_thread);
  if (flags & ExecutePHPFlagsAtInterrupt) enableSignalPolling();
  SCOPE_EXIT {
    if (flags & ExecutePHPFlagsAtInterrupt) disableSignalPolling();
    switchThreadMode(origThreadMode, m_thread);
  };
  auto const ret = g_context->evalPHPDebugger(code.get(), frame);
  output = sb.detach();
  stream_status = StreamStatus::COMPLETED;
  return {ret.failed, ret.result};
}


std::pair<bool,Variant>
DebuggerProxy::ExecutePHP(const std::string &php, OptString &output,
                          int frame, int flags) {
  TRACE(2, "DebuggerProxy::ExecutePHP\n");
  // Wire up stdout and stderr to our own string buffer so we can pass
  // any output back to the client.
  StringBuffer sb;
  StringBuffer *save = g_context->swapOutputBuffer(nullptr);
  DebuggerStdoutHook stdout_hook(sb);
  DebuggerLoggerHook stderr_hook(sb);

  auto const previousEvalOutputHook = m_evalOutputHook;
  if (previousEvalOutputHook != nullptr) {
    g_context->removeStdoutHook(previousEvalOutputHook);
  }

  m_evalOutputHook = &stdout_hook;
  g_context->addStdoutHook(&stdout_hook);

  if (flags & ExecutePHPFlagsLog) {
    Logger::SetThreadHook(&stderr_hook);
  }
  SCOPE_EXIT {
    g_context->removeStdoutHook(&stdout_hook);
    g_context->swapOutputBuffer(save);
    if (flags & ExecutePHPFlagsLog) {
      Logger::SetThreadHook(nullptr);
    }

    if (previousEvalOutputHook != nullptr) {
      g_context->addStdoutHook(previousEvalOutputHook);
    }

    m_evalOutputHook = previousEvalOutputHook;
  };
  OptString code(php.c_str(), php.size(), CopyString);
  // We're about to start executing more PHP. This is typically done
  // in response to commands from the client, and the client expects
  // those commands to send more interrupts since, of course, the
  // user might want to debug the code we're about to run. If we're
  // already processing an interrupt, enable signal polling around
  // the execution of the new PHP to ensure that we can handle
  // signals while doing so.
  //
  // Note: we must switch the thread mode to Sticky so we block
  // other threads which may hit interrupts while we're running,
  // since nested processInterrupt() calls would normally release
  // other threads on the way out.
  assertx(m_thread == (int64_t)Process::GetThreadId());
  ThreadMode origThreadMode = m_threadMode;
  switchThreadMode(Sticky, m_thread);
  if (flags & ExecutePHPFlagsAtInterrupt) enableSignalPolling();
  SCOPE_EXIT {
    if (flags & ExecutePHPFlagsAtInterrupt) disableSignalPolling();
    switchThreadMode(origThreadMode, m_thread);
  };
  auto const ret = g_context->evalPHPDebugger(code.get(), frame);
  output = sb.detach();
  return {ret.failed, ret.result};
}

std::string DebuggerProxy::requestAuthToken() {
  Lock lock(m_signalMutex);
  TRACE_RB(2, "DebuggerProxy::requestauthToken: sending auth request\n");

  // Try to use the current sandbox's path, defaulting to the path from
  // DebuggerDefaultSandboxPath if the current sandbox path is empty.
  auto sandboxPath = getSandbox().m_path;
  if (sandboxPath.empty()) {
    sandboxPath = Cfg::Debugger::DefaultSandboxPath;
  }

  CmdAuth cmd;
  cmd.setSandboxPath(sandboxPath);
  if (!cmd.onServer(*this)) {
    TRACE_RB(2, "DebuggerProxy::requestAuthToken: "
             "Failed to send CmdAuth to client\n");
    return "";
  }

  DebuggerCommandPtr res;
  while (!DebuggerCommand::Receive(m_thrift, res,
                                   "DebuggerProxy::requestAuthToken()", true, true)) {
    checkStop();
  }
  if (!res) {
    TRACE_RB(2, "DebuggerProxy::requestAuthToken: "
             "Failed to get CmdAuth back from client\n");
    return "";
  }

  auto token = std::dynamic_pointer_cast<CmdAuth>(res);
  if (!token) {
    TRACE_RB(2, "DebuggerProxy::requestAuthToken: "
             "bad response from token request: %d", res->getType());
    return "";
  }

  return token->getToken();
}

std::string DebuggerProxy::requestSessionAuth() {
  Lock lock(m_signalMutex);
  TRACE_RB(2, "DebuggerProxy::requestSessionAuth: sending auth request\n");

  // Try to use the current sandbox's path, defaulting to the path from
  // DebuggerDefaultSandboxPath if the current sandbox path is empty.
  auto sandboxPath = getSandbox().m_path;
  if (sandboxPath.empty()) {
    sandboxPath = Cfg::Debugger::DefaultSandboxPath;
  }

  CmdAuth cmd;
  cmd.setSandboxPath(sandboxPath);
  if (!cmd.onServer(*this)) {
    TRACE_RB(2, "DebuggerProxy::requestSessionAuth: "
             "Failed to send CmdAuth to client\n");
    return "";
  }

  DebuggerCommandPtr res;
  while (!DebuggerCommand::Receive(m_thrift, res,
                                   "DebuggerProxy::requestSessionAuth()", true, true)) {
    checkStop();
  }
  if (!res) {
    TRACE_RB(2, "DebuggerProxy::requestSessionAuth: "
             "Failed to get CmdAuth back from client\n");
    return "";
  }

  auto auth = std::dynamic_pointer_cast<CmdAuth>(res);
  if (!auth) {
    TRACE_RB(2, "DebuggerProxy::requestSessionAuth: "
             "bad response from auth request: %d", res->getType());
    return "";
  }

  return auth->getSession();
}

int DebuggerProxy::getRealStackDepth() {
  TRACE(2, "DebuggerProxy::getRealStackDepth\n");
  int depth = 0;
  auto const context = g_context.getNoCheck();
  auto fp = vmfp();
  if (!fp) return 0;

  while (fp != nullptr) {
    fp = context->getPrevVMState(fp);
    depth++;
  }
  return depth;
}

int DebuggerProxy::getStackDepth() {
  TRACE(2, "DebuggerProxy::getStackDepth\n");
  int depth = 0;
  auto fp = vmfp();
  if (!fp) return 0;
  fp = fp->sfp();
  while (fp) {
    fp = fp->sfp();
    depth++;
  }
  return depth;
}

// Allow breaks for previously disabled breakpoints that do not match the site
// of cmd. (Call this when processing an interrupt since this probably means
// that execution has moved away from the previous interrupt site.)
void DebuggerProxy::setBreakableForBreakpointsNotMatching(CmdInterrupt& cmd) {
  TRACE(2, "DebuggerProxy::setBreakableForBreakpointsNotMatching\n");
  auto site = cmd.getSite();
  if (site != nullptr) {
    auto stackDepth = getRealStackDepth();
    for (unsigned int i = 0; i < m_breakpoints.size(); ++i) {
      BreakPointInfoPtr bp = m_breakpoints[i];
      if (bp != nullptr && bp->m_state != BreakPointInfo::Disabled &&
          !bp->match(*this, cmd.getInterruptType(), *site)) {
        bp->setBreakable(stackDepth);
      }
    }
  }
}

// Do not allow further breaks on the site of cmd, except during
// calls made from the current site.
void DebuggerProxy::unsetBreakableForBreakpointsMatching(CmdInterrupt& cmd) {
  TRACE(2, "DebuggerProxy::unsetBreakableForBreakpointsMatching\n");
  auto site = cmd.getSite();
  if (site != nullptr) {
    auto offset = site->getCurOffset();
    auto stackDepth = getRealStackDepth();
    for (unsigned int i = 0; i < m_breakpoints.size(); ++i) {
      BreakPointInfoPtr bp = m_breakpoints[i];
      if (bp != nullptr && bp->m_state != BreakPointInfo::Disabled &&
          bp->match(*this, cmd.getInterruptType(), *site)) {
        bp->unsetBreakable(stackDepth, offset);
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
}
