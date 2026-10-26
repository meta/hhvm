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
#include "hphp/runtime/base/execution-context.h"

#define __STDC_LIMIT_MACROS

#include <cstdint>
#include <algorithm>
#include <list>
#include <utility>

#include <fmt/format.h>
#include <folly/MapUtil.h>
#include <folly/Format.h>
#include <folly/Likely.h>

#include "hphp/util/configs/log.h"
#include "hphp/util/logger.h"
#include "hphp/util/process.h"
#include "hphp/util/text-color.h"
#include "hphp/util/service-data.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/request-event-handler.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/debuggable.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/request-fanout-limit-wrapper.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/container-functions.h"
#include "hphp/runtime/base/apc-stats.h"
#include "hphp/runtime/base/apc-typed-value.h"
#include "hphp/runtime/base/extended-logger.h"
#include "hphp/runtime/base/implicit-context.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/runtime/debugger/debugger_base.h"
#include "hphp/runtime/ext/asio/ext_asio.h"
#include "hphp/runtime/ext/std/ext_std_output.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/ext/reflection/ext_reflection.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/server-note.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/runtime/server/thread-hint.h"
#include "hphp/runtime/server/source-root-info.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/jit/enter-tc.h"
#include "hphp/runtime/vm/jit/jit-resume-addr-defs.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/unwind-itanium.h"
#include "hphp/runtime/vm/act-rec-defs.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/event-hook.h"
#include "hphp/runtime/vm/hh-utils.h"
#include "hphp/runtime/vm/interp-helpers.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/unwind.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/timer.h"
#include "hphp/zend/zend-math.h"

#include "hphp/runtime/ext/hh/ext_implicit_context.h"
#include "hphp/runtime/ext/vsdebug/ext_vsdebug.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
TRACE_SET_MOD(bcinterp)

rds::local::AliasedRDSLocal<ExecutionContext,
                            rds::local::Initialize::Explicitly,
                            &rds::local::detail::HotRDSLocals::g_context
                           > g_context;

static std::atomic<bool> s_GloballySuppressNonFatals{false};

static auto s_pspEntryCounter = ServiceData::createTimeSeries(
    "psp_entry", {ServiceData::StatsType::COUNT});
static auto s_pspSuccessCounter = ServiceData::createTimeSeries(
    "psp_success", {ServiceData::StatsType::COUNT});

ExecutionContext::ExecutionContext()
  : m_transport(nullptr)
  , m_sb(nullptr)
  , m_implicitFlush(false)
  , m_protectedLevel(0)
  , m_stdoutBytesWritten(0)
  , m_errorState(ExecutionContext::ErrorState::NoError)
  , m_lastErrorNum(0)
  , m_deferredErrors(nullptr)
  , m_throwAllErrors(false)
  , m_vhost(nullptr)
  , m_globalNVTable(nullptr)
  , m_lambdaCounter(0)
  , m_nesting(0)
  , m_dbgNoBreak(false)
  , m_inlineInterpState(InlineInterpState::NONE)
  , m_lastErrorPath(staticEmptyString())
  , m_lastErrorLine(0)
  , m_xenonRequestOutputFile(staticEmptyString())
  , m_executingSetprofileCallback(false)
  , m_logger_hook(*this)
{
  m_deferredErrors = staticEmptyVec();
  resetCoverageCounters();
  // We don't want a new execution context to cause any request-heap
  // allocations (because it will cause us to hold a slab, even while idle).
  static auto s_cwd = makeStaticString(Process::CurrentWorkingDirectory);
  m_cwd = s_cwd;
  RID().setMemoryLimit(std::to_string(Cfg::Server::RequestMemoryMaxBytes));
  RID().setErrorReportingLevel(RuntimeOption::RuntimeErrorReportingLevel);

  VariableSerializer::serializationSizeLimit->value =
    RuntimeOption::SerializationSizeLimit;
  tvWriteUninit(m_headerCallback);

  m_shutdowns[ShutdownType::ShutDown] = empty_vec_array();
  m_shutdowns[ShutdownType::PostSend] = empty_vec_array();
  m_shutdownsBackup[ShutdownType::ShutDown] = empty_vec_array();
  m_shutdownsBackup[ShutdownType::PostSend] = empty_vec_array();
}

namespace rds::local {
template<>
void GContextType::Base::destroy() {
  if (!isNull()) {
    getNoCheck()->sweep();
    nullOut();
  }
}
}


void ExecutionContext::cleanup() {
  manageAPCHandle();
  auto dead = std::move(m_stdoutHooks);
  m_debuggerStdoutHook = nullptr;
}

void ExecutionContext::sweep() {
  cleanup();
}

ExecutionContext::~ExecutionContext() {
  cleanup();
}

void ExecutionContext::backupSession() {
  m_shutdownsBackup = m_shutdowns;
  m_userErrorHandlersBackup = m_userErrorHandlers;
  m_userExceptionHandlersBackup = m_userExceptionHandlers;
}

void ExecutionContext::restoreSession() {
  m_shutdowns = m_shutdownsBackup;
  m_userErrorHandlers = m_userErrorHandlersBackup;
  m_userExceptionHandlers = m_userExceptionHandlersBackup;
}

///////////////////////////////////////////////////////////////////////////////
// system functions

OptString ExecutionContext::getMimeType() const {
  OptString mimetype;
  if (m_transport) {
    mimetype = m_transport->getMimeType();
  }

  if (strncasecmp(mimetype.data(), "text/", 5) == 0) {
    int pos = mimetype.find(';');
    if (pos != OptString::npos) {
      mimetype = mimetype.substr(0, pos);
    }
  } else if (m_transport && m_transport->getUseDefaultContentType()) {
    mimetype = RID().getDefaultMimeType();
  }
  return mimetype;
}

std::string ExecutionContext::getRequestUrl(size_t szLimit) {
  Transport* t = getTransport();
  std::string ret = t ? t->getUrl() : "";
  if (szLimit != std::string::npos) {
    ret = ret.substr(0, szLimit);
  }
  return ret;
}

void ExecutionContext::setContentType(const OptString& mimetype,
                                          const OptString& charset) {
  if (m_transport) {
    OptString contentType = mimetype;
    contentType += "; ";
    contentType += "charset=";
    contentType += charset;
    m_transport->addHeader("Content-Type", contentType.c_str());
    m_transport->setUseDefaultContentType(false);
  }
}

///////////////////////////////////////////////////////////////////////////////
// write()

void ExecutionContext::write(const OptString& s) {
  write(s.data(), s.size());
}

void ExecutionContext::addStdoutHook(StdoutHook* hook) {
  if (hook != nullptr) {
    m_stdoutHooks.insert(hook);
  }
}

bool ExecutionContext::removeStdoutHook(StdoutHook* hook) {
  if (hook == nullptr) {
    return false;
  }

  return m_stdoutHooks.erase(hook) != 0;
}

std::size_t ExecutionContext::numStdoutHooks() const {
  return m_stdoutHooks.size();
}

VSDEBUG::DebuggerStdoutHook* ExecutionContext::debuggerStdoutHook() const {
  return m_debuggerStdoutHook;
}

void ExecutionContext::addDebuggerStdoutHook(
  VSDEBUG::DebuggerStdoutHook* hook) {
  m_debuggerStdoutHook = hook;
}

void ExecutionContext::removeDebuggerStdoutHook() {
  m_debuggerStdoutHook = nullptr;
}

static void safe_stdout(const  void  *ptr,  size_t  size) {
  write(fileno(stdout), ptr, size);
}

void ExecutionContext::writeStdout(const char *s, int len, bool skipHooks) {
  fflush(stdout);
  if (skipHooks || m_stdoutHooks.empty()) {
    if (s_stdout_color) {
      safe_stdout(s_stdout_color, strlen(s_stdout_color));
      safe_stdout(s, len);
      safe_stdout(ANSI_COLOR_END, strlen(ANSI_COLOR_END));
    } else {
      safe_stdout(s, len);
    }
    m_stdoutBytesWritten += len;
  } else {
    for (auto const hook : m_stdoutHooks) {
      assertx(hook != nullptr);
      (*hook)(s, len);
    }
  }
}

void ExecutionContext::writeTransport(const char *s, int len) {
  if (m_transport) {
    m_transport->sendRaw(s, len, 200, false, true);
  } else {
    writeStdout(s, len);
  }
}

size_t ExecutionContext::getStdoutBytesWritten() const {
  return m_stdoutBytesWritten;
}

void ExecutionContext::write(const char *s, int len) {
  if (m_debuggerStdoutHook) (*m_debuggerStdoutHook)(s, len);
  if (m_sb) {
    m_sb->append(s, len);
    if (m_out && m_out->chunk_size > 0) {
      if (m_sb->size() >= m_out->chunk_size) {
        obFlush();
      }
    }
    if (m_implicitFlush) flush();
  } else {
    writeTransport(s, len);
  }
}

///////////////////////////////////////////////////////////////////////////////
// output buffers

void ExecutionContext::obProtect(bool on) {
  m_protectedLevel = on ? m_buffers.size() : 0;
}

void ExecutionContext::obStart(const Variant& handler /* = null */,
                               int chunk_size /* = 0 */,
                               OBFlags flags /* = OBFlags::Default */) {
  if (m_insideOBHandler) {
    raise_error("ob_start(): Cannot use output buffering "
                "in output buffering display handlers");
  }
  m_buffers.emplace_back(Variant(handler), chunk_size, flags);
  resetCurrentBuffer();
}

OptString ExecutionContext::obCopyContents() {
  if (!m_buffers.empty()) {
    StringBuffer &oss = m_buffers.back().oss;
    if (!oss.empty()) {
      return oss.copy();
    }
  }
  return String::Empty();
}

OptString ExecutionContext::obDetachContents() {
  if (!m_buffers.empty()) {
    StringBuffer &oss = m_buffers.back().oss;
    if (!oss.empty()) {
      return oss.detach();
    }
  }
  return String::Empty();
}

int ExecutionContext::obGetContentLength() {
  if (m_buffers.empty()) {
    return 0;
  }
  return m_buffers.back().oss.size();
}

void ExecutionContext::obClean(int handler_flag) {
  if (!m_buffers.empty()) {
    OutputBuffer *last = &m_buffers.back();
    if (!last->handler.isNull()) {
      m_insideOBHandler = true;
      SCOPE_EXIT { m_insideOBHandler = false; };
      vm_call_user_func(last->handler,
                        make_vec_array(last->oss.detach(), handler_flag));
    }
    last->oss.clear();
  }
}

bool ExecutionContext::obFlush(bool force /*= false*/) {
  assertx(m_protectedLevel >= 0);

  if ((int)m_buffers.size() <= m_protectedLevel) {
    return false;
  }

  auto iter = m_buffers.end();
  OutputBuffer& last = *(--iter);
  if (!force && !(last.flags & OBFlags::Flushable)) {
    return false;
  }
  if (any(last.flags & OBFlags::OutputDisabled)) {
    return false;
  }

  const int flag = k_PHP_OUTPUT_HANDLER_START | k_PHP_OUTPUT_HANDLER_END;

  if (iter != m_buffers.begin()) {
    OutputBuffer& prev = *(--iter);
    if (last.handler.isNull()) {
      prev.oss.absorb(last.oss);
    } else {
      auto str = last.oss.detach();
      try {
        Variant tout;
        {
          m_insideOBHandler = true;
          SCOPE_EXIT { m_insideOBHandler = false; };
          tout = vm_call_user_func(
            last.handler, make_vec_array(str, flag)
          );
        }
        prev.oss.append(tout.toString());
      } catch (...) {
        prev.oss.append(str);
        throw;
      }
    }
    return true;
  }

  auto str = last.oss.detach();
  if (!last.handler.isNull()) {
    try {
      Variant tout;
      {
        m_insideOBHandler = true;
        SCOPE_EXIT { m_insideOBHandler = false; };
        tout = vm_call_user_func(
          last.handler, make_vec_array(str, flag)
        );
      }
      str = tout.toString();
    } catch (...) {
      writeTransport(str.data(), str.size());
      throw;
    }
  }

  writeTransport(str.data(), str.size());
  return true;
}

void ExecutionContext::obFlushAll() {
  do {
    obFlush(true);
  } while (obEnd());
}

bool ExecutionContext::obEnd() {
  assertx(m_protectedLevel >= 0);
  if ((int)m_buffers.size() > m_protectedLevel) {
    m_buffers.pop_back();
    resetCurrentBuffer();
    if (m_implicitFlush) flush();
    return true;
  }
  if (m_implicitFlush) flush();
  return false;
}

void ExecutionContext::obEndAll() {
  while (obEnd()) {}
}

int ExecutionContext::obGetLevel() {
  assertx((int)m_buffers.size() >= m_protectedLevel);
  return m_buffers.size() - m_protectedLevel;
}

const StaticString
  s_level("level"),
  s_type("type"),
  s_flags("flags"),
  s_name("name"),
  s_args("args"),
  s_chunk_size("chunk_size"),
  s_buffer_used("buffer_used"),
  s_default_output_handler("default output handler");

Array ExecutionContext::obGetStatus(bool full) {
  Array ret = empty_vec_array();
  int level = 0;
  for (auto& buffer : m_buffers) {
    Array status = empty_dict_array();
    if (level < m_protectedLevel || buffer.handler.isNull()) {
      status.set(s_name, s_default_output_handler);
      status.set(s_type, 0);
    } else {
      status.set(s_name, buffer.handler);
      status.set(s_type, 1);
    }

    int flags = 0;
    if (any(buffer.flags & OBFlags::Cleanable)) {
      flags |= k_PHP_OUTPUT_HANDLER_CLEANABLE;
    }
    if (any(buffer.flags & OBFlags::Flushable)) {
      flags |= k_PHP_OUTPUT_HANDLER_FLUSHABLE;
    }
    if (any(buffer.flags & OBFlags::Removable)) {
      flags |= k_PHP_OUTPUT_HANDLER_REMOVABLE;
    }
    status.set(s_flags, flags);

    status.set(s_level, level);
    status.set(s_chunk_size, buffer.chunk_size);
    status.set(s_buffer_used, static_cast<uint64_t>(buffer.oss.size()));

    if (full) {
      ret.append(status);
    } else {
      ret = std::move(status);
    }
    level++;
  }
  return ret;
}

OptString ExecutionContext::obGetBufferName() {
  if (m_buffers.empty()) {
    return OptString();
  } else if (m_buffers.size() <= m_protectedLevel) {
    return s_default_output_handler;
  } else {
    auto iter = m_buffers.end();
    OutputBuffer& buffer = *(--iter);
    if (buffer.handler.isNull()) {
      return s_default_output_handler;
    } else {
      return buffer.handler.toString();
    }
  }
}

void ExecutionContext::obSetImplicitFlush(bool on) {
  m_implicitFlush = on;
}

Array ExecutionContext::obGetHandlers() {
  Array ret = empty_vec_array();
  for (auto& ob : m_buffers) {
    auto& handler = ob.handler;
    ret.append(handler.isNull() ? s_default_output_handler : handler);
  }
  return ret;
}

void ExecutionContext::flush() {
  if (!m_buffers.empty() &&
      Cfg::Server::EnableEarlyFlush && m_protectedLevel &&
      !(m_buffers.front().flags & OBFlags::OutputDisabled)) {
    OutputBuffer &buffer = m_buffers.front();
    StringBuffer &oss = buffer.oss;
    if (!oss.empty()) {
      if (any(buffer.flags & OBFlags::WriteToStdout)) {
        writeStdout(oss.data(), oss.size());
      } else {
        writeTransport(oss.data(), oss.size());
      }
      oss.clear();
    }
  }
}

void ExecutionContext::resetCurrentBuffer() {
  if (m_buffers.empty()) {
    m_sb = nullptr;
    m_out = nullptr;
  } else {
    m_sb = &m_buffers.back().oss;
    m_out = &m_buffers.back();
  }
}

///////////////////////////////////////////////////////////////////////////////
// program executions

void ExecutionContext::registerShutdownFunction(const Variant& function,
                                                ShutdownType type) {
  auto& funcs = m_shutdowns[type];
  assertx(funcs.isVec());
  funcs.append(function);
}

Variant ExecutionContext::pushUserErrorHandler(const Variant& function,
                                               int error_types) {
  Variant ret{Variant::NullInit{}};
  if (!m_userErrorHandlers.empty()) {
    ret = m_userErrorHandlers.back().first;
  }
  m_userErrorHandlers.push_back(std::pair<Variant,int>(function, error_types));
  return ret;
}

Variant ExecutionContext::pushUserExceptionHandler(const Variant& function) {
  Variant ret{Variant::NullInit{}};
  if (!m_userExceptionHandlers.empty()) {
    ret = m_userExceptionHandlers.back();
  }
  m_userExceptionHandlers.push_back(function);
  return ret;
}

void ExecutionContext::popUserErrorHandler() {
  if (!m_userErrorHandlers.empty()) {
    m_userErrorHandlers.pop_back();
  }
}

void ExecutionContext::clearUserErrorHandlers() {
  while (!m_userErrorHandlers.empty()) m_userErrorHandlers.pop_back();
}

void ExecutionContext::popUserExceptionHandler() {
  if (!m_userExceptionHandlers.empty()) {
    m_userExceptionHandlers.pop_back();
  }
}

void ExecutionContext::acceptRequestEventHandlers(bool enable) {
  m_acceptRequestEventHandlers = enable;
}

std::size_t ExecutionContext::registerRequestEventHandler(
  RequestEventHandler *handler) {
  assertx(handler && handler->getInited());
  assertx(m_acceptRequestEventHandlers);
  m_requestEventHandlers.push_back(handler);
  return m_requestEventHandlers.size()-1;
}

void ExecutionContext::unregisterRequestEventHandler(
  RequestEventHandler* handler,
  std::size_t index) {
  assertx(index < m_requestEventHandlers.size() &&
         m_requestEventHandlers[index] == handler);
  assertx(!handler->getInited());
  if (index == m_requestEventHandlers.size()-1) {
    m_requestEventHandlers.pop_back();
  } else {
    m_requestEventHandlers[index] = nullptr;
  }
}

static bool requestEventHandlerPriorityComp(RequestEventHandler *a,
                                            RequestEventHandler *b) {
  if (!a) return b;
  else if (!b) return false;
  else return a->priority() < b->priority();
}

void ExecutionContext::onRequestShutdown() {
  while (!m_requestEventHandlers.empty()) {
    // handlers could cause other handlers to be registered,
    // so need to repeat until done
    decltype(m_requestEventHandlers) tmp;
    tmp.swap(m_requestEventHandlers);

    // Sort handlers by priority so that lower priority values get shutdown
    // first
    sort(tmp.begin(), tmp.end(),
         requestEventHandlerPriorityComp);
    for (auto* handler : tmp) {
      if (!handler) continue;
      assertx(handler->getInited());
      handler->requestShutdown();
      handler->setInited(false);
    }
  }
}

void ExecutionContext::executeFunctions(ShutdownType type) {
  RID().resetTimers(
      Cfg::Server::PspTimeoutSeconds,
      Cfg::Server::PspCpuTimeoutSeconds
  );

  assertx((*ImplicitContext::activeCtx) == (*ImplicitContext::emptyCtx));

  // We mustn't destroy any callbacks until we're done with all
  // of them. So hold them in tmp.
  // XXX still true in a world without destructors?
  auto tmp = empty_vec_array();
  while (true) {
    Array funcs = m_shutdowns[type];
    if (funcs.empty()) break;
    m_shutdowns[type] = empty_vec_array();
    IterateV(
      funcs.get(),
      [](TypedValue v) {
        vm_call_user_func(VarNR{v}, init_null_variant,
                          RuntimeCoeffects::defaults());
        assertx((*ImplicitContext::activeCtx) == (*ImplicitContext::emptyCtx));
      }
    );
    tmp.append(funcs);
  }
}

void ExecutionContext::onShutdownPreSend() {
  // in case obStart was called without obFlush
  SCOPE_EXIT {
    try { obFlushAll(); } catch (...) {}
  };

  // When host is OOMing, abort abruptly.
  if (RID().shouldOOMAbort()) return;

  tracing::Block _{"shutdown-pre-send"};

  tl_heap->resetCouldOOM(isStandardRequest());
  executeFunctions(ShutDown);
}

void ExecutionContext::onShutdownPostSend() {
  ThreadHint::getInstance().updateThreadHint(
    ThreadHint::Priority::PostProcessing);

  // When host is OOMing, abort abruptly.
  if (RID().shouldOOMAbort()) return;

  tracing::Block _{"shutdown-post-send"};

  ServerStats::SetThreadMode(ServerStats::ThreadMode::PostProcessing);
  tl_heap->resetCouldOOM(isStandardRequest());
  try {
    try {
      ServerStatsHelper ssh("psp", ServerStatsHelper::TRACK_HWINST);
      s_pspEntryCounter->addValue(1);
      executeFunctions(PostSend);
      s_pspSuccessCounter->addValue(1);
    } catch (...) {
      try {
        bump_counter_and_rethrow(true /* isPsp */, this);
      } catch (const ExitException&) {
        // do nothing
      } catch (const Exception& e) {
        onFatalError(e);
      } catch (const Object& e) {
        onUnhandledException(e);
      }
    }
  } catch (...) {
    Logger::Error("unknown exception was thrown from psp");
  }

  ServerStats::SetThreadMode(ServerStats::ThreadMode::Idling);
}

///////////////////////////////////////////////////////////////////////////////
// error handling

bool ExecutionContext::errorNeedsHandling(int errnum,
                                          bool callUserHandler,
                                          ErrorThrowMode mode) {
  if (UNLIKELY(m_throwAllErrors)) {
    throw Exception(fmt::format("throwAllErrors: {}", errnum));
  }
  if (mode != ErrorThrowMode::Never || errorNeedsLogging(errnum)) {
    return true;
  }
  if (callUserHandler) {
    if (!m_userErrorHandlers.empty() &&
        (m_userErrorHandlers.back().second & errnum) != 0) {
      return true;
    }
  }
  return false;
}

bool ExecutionContext::errorNeedsLogging(int errnum) {
  auto level =
    RID().getErrorReportingLevel() |
    Cfg::Log::ForceErrorReportingLevel;
  return Cfg::Log::NoSilencer || (level & errnum) != 0;
}

struct ErrorStateHelper {
  ErrorStateHelper(ExecutionContext *context,
                   ExecutionContext::ErrorState state) {
    m_context = context;
    m_originalState = m_context->getErrorState();
    m_context->setErrorState(state);
  }
  ~ErrorStateHelper() {
    m_context->setErrorState(m_originalState);
  }
private:
  ExecutionContext *m_context;
  ExecutionContext::ErrorState m_originalState;
};

const StaticString
  s_class("class"),
  s_file("file"),
  s_function("function"),
  s_line("line"),
  s_error_num("error-num"),
  s_error_string("error-string"),
  s_error_file("error-file"),
  s_error_line("error-line"),
  s_error_backtrace("error-backtrace"),
  s_error_implicit_context_blame("error-implicit-context-blame"),
  s_overflow("overflow");

void ExecutionContext::handleError(const std::string& msg,
                                   int errnum,
                                   bool callUserHandler,
                                   ErrorThrowMode mode,
                                   const std::string& prefix,
                                   bool skipFrame /* = false */) {
  SYNC_VM_REGS_SCOPED();

  auto newErrorState = ErrorState::ErrorRaised;
  switch (getErrorState()) {
  case ErrorState::ErrorRaised:
  case ErrorState::ErrorRaisedByUserHandler:
    return;
  case ErrorState::ExecutingUserHandler:
    newErrorState = ErrorState::ErrorRaisedByUserHandler;
    break;
  default:
    break;
  }

  // Potentially upgrade the error to E_USER_ERROR
  if (errnum & Cfg::ErrorHandling::UpgradeLevel &
      static_cast<int>(ErrorMode::UPGRADEABLE_ERROR)) {
    errnum = static_cast<int>(ErrorMode::USER_ERROR);
    mode = ErrorThrowMode::IfUnhandled;
  }

  if (UNLIKELY(s_GloballySuppressNonFatals.load(std::memory_order_relaxed) &&
               mode == ErrorThrowMode::Never)) {
    Logger::FError("Suppressing non-Fatal Error({}): {}", errnum, msg);
    return;
  }

  auto const ee = skipFrame ?
    ExtendedException(ExtendedException::SkipFrame{}, msg) :
    ExtendedException(msg);
  bool handled = false;
  {
    ErrorStateHelper esh(this, newErrorState);
    if (callUserHandler) {
      handled = callUserErrorHandler(ee, errnum, false);
    }

    if (!handled) {
      recordLastError(ee, errnum);
    }
  }

  if (mode == ErrorThrowMode::Always ||
      (mode == ErrorThrowMode::IfUnhandled && !handled)) {
    DEBUGGER_ATTACHED_ONLY(phpDebuggerErrorHook(ee, errnum, msg));
    bool isRecoverable =
      errnum == static_cast<int>(ErrorMode::RECOVERABLE_ERROR);
    raise_fatal_error(msg.c_str(), ee.getBacktrace(), isRecoverable,
                      !errorNeedsLogging(errnum) /* silent */);
    not_reached();
  }
  if (!handled) {



    // If we're inside an error handler already, queue it up on the deferred
    // list.
    if (getErrorState() == ErrorState::ExecutingUserHandler) {
      auto& deferred = m_deferredErrors;
      if (deferred.size() < Cfg::Eval::MaxDeferredErrors) {
        auto fileAndLine = ee.getFileAndLine();
        deferred.append(
          make_dict_array(
            s_error_num, errnum,
            s_error_string, msg,
            s_error_file, std::move(fileAndLine.first),
            s_error_line, fileAndLine.second,
            s_error_backtrace, ee.getBacktrace(),
            s_error_implicit_context_blame, ImplicitContext::getBlameVectors()
          )
        );
      } else if (!deferred.empty()) {
        auto const last = deferred.lval(int64_t{deferred.size() - 1});
        if (isDictType(type(last))) {
          asArrRef(last).set(s_overflow, true);
        }
      }
    }

    if (errorNeedsLogging(errnum)) {
      DEBUGGER_ATTACHED_ONLY(phpDebuggerErrorHook(ee, errnum, ee.getMessage()));
      auto fileAndLine = ee.getFileAndLine();
      Logger::Log(Logger::LogError, prefix.c_str(), ee,
                  fileAndLine.first.c_str(), fileAndLine.second);
    }
  }
}

bool ExecutionContext::callUserErrorHandler(const Exception& e, int errnum,
                                                bool swallowExceptions) {
  // If we raise an error while in the process of loading a Func or Class we
  // need to re-allow autoloading while executing the user error handler.
  AutoloadHandler::Inhibit allowAutoload{false};

  switch (getErrorState()) {
  case ErrorState::ExecutingUserHandler:
  case ErrorState::ErrorRaisedByUserHandler:
    return false;
  default:
    break;
  }
  if (!m_userErrorHandlers.empty() &&
      (m_userErrorHandlers.back().second & errnum) != 0) {
    std::pair<OptString, int> fileAndLine{String::Empty(), 0};
    Variant backtrace;
    if (auto const ee = dynamic_cast<const ExtendedException*>(&e)) {
      fileAndLine = ee->getFileAndLine();
      backtrace = ee->getBacktrace();
    }
    try {
      ErrorStateHelper esh(this, ErrorState::ExecutingUserHandler);
      m_deferredErrors = empty_vec_array();
      SCOPE_EXIT { m_deferredErrors = empty_vec_array(); };
      if (!same(vm_call_user_func
                (m_userErrorHandlers.back().first,
                 make_vec_array(errnum, OptString(e.getMessage()),
                     fileAndLine.first, fileAndLine.second, empty_dict_array(),
                     backtrace, ImplicitContext::getBlameVectors()),
                 RuntimeCoeffects::defaults()),
                false)) {
        return true;
      }
    } catch (const RequestTimeoutException&) {
      static auto requestErrorHandlerTimeoutCounter =
          ServiceData::createTimeSeries("requests_timed_out_error_handler",
                                        {ServiceData::StatsType::COUNT});
      requestErrorHandlerTimeoutCounter->addValue(1);
      ServerStats::Log("request.timed_out.error_handler", 1);
      markTimedOut();
      if (!swallowExceptions) throw;
    } catch (const RequestCPUTimeoutException&) {
      static auto requestErrorHandlerCPUTimeoutCounter =
          ServiceData::createTimeSeries("requests_cpu_timed_out_error_handler",
                                        {ServiceData::StatsType::COUNT});
      requestErrorHandlerCPUTimeoutCounter->addValue(1);
      ServerStats::Log("request.cpu_timed_out.error_handler", 1);
      markTimedOut();
      if (!swallowExceptions) throw;
    } catch (const RequestMemoryExceededException&) {
      static auto requestErrorHandlerMemoryExceededCounter =
          ServiceData::createTimeSeries(
              "requests_memory_exceeded_error_handler",
              {ServiceData::StatsType::COUNT});
      requestErrorHandlerMemoryExceededCounter->addValue(1);
      ServerStats::Log("request.memory_exceeded.error_handler", 1);

      if (!swallowExceptions) throw;
    } catch (...) {
      static auto requestErrorHandlerOtherExceptionCounter =
          ServiceData::createTimeSeries(
              "requests_other_exception_error_handler",
              {ServiceData::StatsType::COUNT});
      requestErrorHandlerOtherExceptionCounter->addValue(1);
      ServerStats::Log("request.other_exception.error_handler", 1);

      if (!swallowExceptions) throw;
    }
  }
  return false;
}

bool ExecutionContext::onFatalError(const Exception& e) {
  tl_heap->resetCouldOOM(isStandardRequest());
  RID().resetTimers();
  // need to restore the error reporting level, because the fault
  // handler for silencers won't be run on fatals, and we might be
  // about to run a user error handler (and psp/shutdown code).
  RID().setErrorReportingLevel(RuntimeOption::RuntimeErrorReportingLevel);

  auto prefix = "\nFatal error: ";
  auto errnum = static_cast<int>(ErrorMode::FATAL_ERROR);
  auto const fatal = dynamic_cast<const FatalErrorException*>(&e);
  if (fatal && fatal->isRecoverable()) {
     prefix = "\nCatchable fatal error: ";
     errnum = static_cast<int>(ErrorMode::RECOVERABLE_ERROR);
  }

  recordLastError(e, errnum);

  bool silenced = false;
  std::pair<OptString, int> fileAndLine{String::Empty(), 0};
  if (auto const ee = dynamic_cast<const ExtendedException*>(&e)) {
    silenced = ee->isSilent();
    fileAndLine = ee->getFileAndLine();
  }
  // need to silence even with the AlwaysLogUnhandledExceptions flag set
  if (!silenced && Cfg::Log::AlwaysLogUnhandledExceptions) {
    Logger::Log(Logger::LogError, prefix, e, fileAndLine.first.c_str(),
                fileAndLine.second);
  }
  bool handled = false;
  if (Cfg::ErrorHandling::CallUserHandlerOnFatals) {
    handled = callUserErrorHandler(e, errnum, true);
  }
  if (!handled && !silenced && !Cfg::Log::AlwaysLogUnhandledExceptions) {
    Logger::Log(Logger::LogError, prefix, e, fileAndLine.first.c_str(),
                fileAndLine.second);
  }
  return handled;
}

bool ExecutionContext::onUnhandledException(Object e) {
  OptString err = throwable_to_string(e.get());
  if (Cfg::Log::AlwaysLogUnhandledExceptions) {
    Logger::Error("\nFatal error: Uncaught %s", err.data());
  }

  if (e.instanceof(SystemLib::getThrowableClass())) {
    // user thrown exception
    if (!m_userExceptionHandlers.empty()) {
      if (!same(vm_call_user_func
                (m_userExceptionHandlers.back(),
                 make_vec_array(e),
                 RuntimeCoeffects::defaults()),
                false)) {
        return true;
      }
    }
  } else {
    assertx(false);
  }
  m_lastError = err;

  if (!Cfg::Log::AlwaysLogUnhandledExceptions) {
    Logger::Error("\nFatal error: Uncaught %s", err.data());
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

void ExecutionContext::debuggerInfo(
    std::vector<std::pair<const char*,std::string>>& info) {
  static StaticString s_memoryLimit("memory_limit");
  int64_t newInt = convert_bytes_to_long(IniSetting::Get(s_memoryLimit));
  if (newInt <= 0) {
    newInt = std::numeric_limits<int64_t>::max();
  }
  if (newInt == std::numeric_limits<int64_t>::max()) {
    info.emplace_back("Max Memory", "(unlimited)");
  } else {
    info.emplace_back("Max Memory", IDebuggable::FormatSize(newInt));
  }
  info.emplace_back("Max Time",
                    IDebuggable::FormatTime(RID().getTimeout() * 1000));
}

void ExecutionContext::setenv(const OptString& name, const OptString& value) {
  m_envs.set(m_envs.convertKey<IntishCast::Cast>(name),
             make_tv<KindOfString>(value.get()));
}

void ExecutionContext::unsetenv(const OptString& name) {
  m_envs.remove(name);
}

OptString ExecutionContext::getenv(const OptString& name) const {
  if (m_envs.exists(name)) {
    return m_envs[name].toString();
  }
  if (is_cli_server_mode()) {
    auto envs = cli_env();
    if (envs.exists(name)) return envs[name].toString();
    return OptString();
  }
  if (auto value = ::getenv(name.data())) {
    return OptString(value, CopyString);
  }
  if (RuntimeOption::EnvVariables.find(name.c_str()) != RuntimeOption::EnvVariables.end()) {
    return OptString(RuntimeOption::EnvVariables[name.c_str()].data(), CopyString);
  }
  return OptString();
}

TypedValue ExecutionContext::lookupClsCns(const NamedType* ne,
                                      const StringData* cls,
                                      const StringData* cns) {
  Class* class_ = nullptr;
  try {
    // Use resolve() as opposed to load() since we want to make sure this
    // class is accessible to us.
    class_ = Class::resolve(ne, cls, vmfp()->func());
  } catch (Object& ex) {
    // For compatibility with php, throwing through a constant lookup has
    // different behavior inside a property initializer (86pinit/86sinit).
    auto ar = getStackFrame();
    if (ar && ar->func() && Func::isSpecial(ar->func()->name())) {
      raise_warning("Uncaught %s", throwable_to_string(ex.get()).data());
      raise_error("Couldn't find constant %s::%s", cls->data(), cns->data());
    }
    throw;
  }
  if (class_ == nullptr) {
    raise_error(Strings::UNKNOWN_CLASS, cls->data());
  }
  TypedValue clsCns = class_->clsCnsGet(cns);
  if (clsCns.m_type == KindOfUninit) {
    raise_error("Couldn't find constant %s::%s", cls->data(), cns->data());
  }
  return clsCns;
}

static Class* loadClass(StringData* clsName) {
  Class* class_ = Class::load(clsName);
  if (class_ == nullptr) {
    raise_error(Strings::UNKNOWN_CLASS, clsName->data());
  }
  return class_;
}

ObjectData* ExecutionContext::createObject(StringData* clsName,
                                           const Variant& params,
                                           const ArrayData* namedArgNames,
                                           bool init /* = true */) {
  return createObject(loadClass(clsName), params, namedArgNames, init);
}

ObjectData* ExecutionContext::createObject(const Class* class_,
                                           const Variant& params,
                                           const ArrayData* namedArgNames,
                                           bool init) {
  callerDynamicConstructChecks(class_);
  auto o = Object::attach(ObjectData::newInstance(const_cast<Class*>(class_)));
  if (init) {
    initObject(class_, params, namedArgNames, o.get());
  }

  return o.detach();
}

ObjectData* ExecutionContext::createObjectOnly(StringData* clsName) {
  return createObject(clsName, init_null_variant, nullptr, false);
}

ObjectData* ExecutionContext::initObject(StringData* clsName,
                                         const Variant& params,
                                         const ArrayData* namedArgNames,
                                         ObjectData* o) {
  return initObject(loadClass(clsName), params, namedArgNames, o);
}

ObjectData* ExecutionContext::initObject(const Class* class_,
                                         const Variant& params,
                                         const ArrayData* namedArgNames,
                                         ObjectData* o) {
  auto ctor = class_->getCtor();
  if (!(ctor->attrs() & AttrPublic)) {
    std::string msg = "Access to non-public constructor of class ";
    msg += class_->name()->data();
    Reflection::ThrowReflectionExceptionObject(msg);
  }
  // call constructor
  if (!isContainerOrNull(params)) {
    throw_param_is_not_container();
  }
  tvDecRefGen(invokeFunc(ctor, params, namedArgNames, o, nullptr,
                         RuntimeCoeffects::fixme(), true, false, true,
                         false, Array()));
  return o;
}

ActRec* ExecutionContext::getStackFrame() {
  VMRegAnchor _;
  return vmfp();
}

ObjectData* ExecutionContext::getThis() {
  VMRegAnchor _;
  ActRec* fp = vmfp();
  if (fp->skipFrame()) fp = getPrevVMStateSkipFrame(fp);
  if (fp && fp->func()->cls() && fp->hasThis()) {
    return fp->getThis();
  }
  return nullptr;
}

namespace {
const RepoOptions& getRepoOptionsForDebuggerEval() {
  const auto root = SourceRootInfo::GetCurrentSourceRoot();
  if (root.empty()) {
    return RepoOptions::defaults();
  }

  const auto dummyPath = fmt::format("{}/$$eval$$.php", root.native());
  return RepoOptions::forFile(dummyPath.data());
}
} // namespace

const RepoOptions& ExecutionContext::getRepoOptionsForCurrentFrame() const {
  return getRepoOptionsForFrame(0);
}

const RepoOptions& ExecutionContext::getRepoOptionsForFrame(int frame) const {
  VMRegAnchor _;

  const RepoOptions* ret{nullptr};

  walkStack([&] (const BTFrame& frm) {
    if (frame--) {
      return false;
    }

    auto const path = frm.func()->unit()->filepath();
    if (UNLIKELY(path->empty())) {
      // - Systemlib paths always start with `/:systemlib`
      // - we make up a bogus filename for eval()
      // - but let's assert out of paranoia as we're in a path that's not
      //   perf-sensitive
      assertx(!frm.func()->unit()->isSystemLib());
      ret = &getRepoOptionsForDebuggerEval();
      return true;
    }
    ret = &RepoOptions::forFile(path->data());
    return true;
  });
  return ret ? *ret : RepoOptions::defaults();
}

void ExecutionContext::onLoadWithOptions(
  const char* f, const RepoOptions& opts
) {
  if (!m_requestOptions) {
    m_requestOptions.emplace(opts);
    return;
  }
  if (m_requestOptions != opts) {
    // The data buffer has to stay alive for the call to raise_error.
    auto const path_str = opts.path();
    auto const path = path_str.empty() ? "{default options}" : path_str.c_str();
    raise_error(
      "Attempting to load file %s with incompatible parser settings from %s, "
      "this request is using parser settings from %s",
      f, path, m_requestOptions->path().c_str()
    );
  }
}

StringData* ExecutionContext::getContainingFileName() {
  VMRegAnchor _;
  ActRec* ar = vmfp();
  if (ar == nullptr) return staticEmptyString();
  if (ar->skipFrame()) ar = getPrevVMStateSkipFrame(ar);
  if (ar == nullptr) return staticEmptyString();
  Unit* unit = ar->func()->unit();
  auto const path =  ar->func()->originalUnit() ?
    ar->func()->originalUnit() : unit->filepath();
  return const_cast<StringData*>(path);
}

int ExecutionContext::getLine() {
  VMRegAnchor _;
  ActRec* ar = vmfp();
  auto func = ar ? ar->func() : nullptr;
  Offset pc = func ? pcOff() : 0;
  if (ar == nullptr) return -1;
  if (ar->skipFrame()) ar = getPrevVMStateSkipFrame(ar, &pc);
  if (ar == nullptr || (func = ar->func()) == nullptr) return -1;
  return func->getLineNumber(pc);
}

ActRec* ExecutionContext::getFrameAtDepthForDebuggerUnsafe(int frameDepth) const {
  ActRec* ret = nullptr;
  walkStack([&] (const BTFrame& frm) {
    if (frameDepth == 0) {
      if (!frm.isInlined() && frm.localsAvailable()) {
        ret = frm.fpInternal();
      }
      return true;
    }

    frameDepth--;
    return false;
  });
  return ret;
}

Array ExecutionContext::getLocalDefinedVariablesDebugger(int frame) {
  const auto fp = getFrameAtDepthForDebuggerUnsafe(frame);
  return getDefinedVariables(fp);
}

bool ExecutionContext::setHeaderCallback(const Variant& callback) {
  if (tvAsVariant(g_context->m_headerCallback).toBoolean()) {
    // return false if a callback has already been set.
    return false;
  }
  tvAsVariant(g_context->m_headerCallback) = callback;
  return true;
}

const static StaticString
  s_enter_async_entry_point("__SystemLib\\enter_async_entry_point");

TypedValue ExecutionContext::invokeUnit(const Unit* unit,
                                        bool callByHPHPInvoke) {
  checkHHConfig(unit);

  const_cast<Unit*>(unit)->merge();

  auto it = unit->getEntryPoint();
  if (callByHPHPInvoke && it != nullptr) {
    if (it->isAsync()) {
      invokeFunc(
        Func::lookup(s_enter_async_entry_point.get()),
        make_vec_array(Variant{it}),
        nullptr, nullptr, nullptr, RuntimeCoeffects::defaults(), false
      );
    } else {
      invokeFunc(it, init_null_variant, nullptr, nullptr, nullptr,
                 RuntimeCoeffects::defaults(), false);
    }
  }
  return make_tv<KindOfInt64>(1);
}

void ExecutionContext::syncGdbState() {
  if (Cfg::Jit::Enabled && !Cfg::Jit::NoGdb) {
    Debug::DebugInfo::Get()->debugSync();
  }
}

void ExecutionContext::pushVMState(TypedValue* savedSP) {
  if (UNLIKELY(!vmfp())) {
    // first entry
    assertx(m_nestedVMs.size() == 0);
    return;
  }

  TRACE(3, "savedVM: %p %p %p %p\n", vmpc(), vmfp(), vmFirstAR(), savedSP);

  auto& savedVM = m_nestedVMs.emplace_back(
    VMState {
      vmpc(),
      vmfp(),
      vmFirstAR(),
      savedSP,
      vmMInstrState(),
      vmJitCalledFrame(),
      vmJitReturnAddr(),
#ifdef HHVM_RECORD_JIT_CFA
      vmJitCfa(),
#endif
      jit::g_unwind_rds->exn,
    }
  );
  jit::g_unwind_rds->exn = nullptr;

  m_nesting++;

  if (debug && savedVM.fp &&
      savedVM.fp->func() &&
      savedVM.fp->func()->unit()) {
    // Some asserts and tracing.
    const Func* func = savedVM.fp->func();
    /* bound-check asserts in offsetOf */
    func->offsetOf(savedVM.pc);
    TRACE(3, "pushVMState: saving frame %s pc %p off %d fp %p\n",
          func->name()->data(),
          savedVM.pc,
          func->offsetOf(savedVM.pc),
          savedVM.fp);
  }
}

void ExecutionContext::popVMState() {
  if (UNLIKELY(m_nestedVMs.empty())) {
    // last exit
    vmfp() = nullptr;
    vmpc() = nullptr;
    vmFirstAR() = nullptr;
    return;
  }

  assertx(m_nestedVMs.size() >= 1);

  VMState &savedVM = m_nestedVMs.back();
  vmpc() = savedVM.pc;
  vmfp() = savedVM.fp;
  vmFirstAR() = savedVM.firstAR;
  vmStack().top() = savedVM.sp;
  vmMInstrState() = savedVM.mInstrState;
  vmJitCalledFrame() = savedVM.jitCalledFrame;
  vmJitReturnAddr() = savedVM.jitReturnAddr;
#ifdef HHVM_RECORD_JIT_CFA
  vmJitCfa() = savedVM.jitCfa;
#endif
  jit::g_unwind_rds->exn = savedVM.exn;

  if (debug) {
    if (savedVM.fp &&
        savedVM.fp->func() &&
        savedVM.fp->func()->unit()) {
      const Func* func = savedVM.fp->func();
      (void) /* bound-check asserts in offsetOf */
        func->offsetOf(savedVM.pc);
      TRACE(3, "popVMState: restoring frame %s pc %p off %d fp %p\n",
            func->name()->data(),
            savedVM.pc,
            func->offsetOf(savedVM.pc),
            savedVM.fp);
    }
  }

  m_nestedVMs.pop_back();
  m_nesting--;

  TRACE(1, "Reentry: exit fp %p pc %p\n", vmfp(), vmpc());
}

void ExecutionContext::ExcLoggerHook::operator()(
    const char* header, const char* msg, const char* ending
) {
  ec.write(header);
  ec.write(msg);
  ec.write(ending);
  ec.flush();
}

StaticString
  s_hh("<?hh "),
  s_namespace("namespace "),
  s_curly_start(" { "),
  s_curly_end(" }"),
  s_function_start("<<__DynamicallyCallable>> function "),
  s_evaluate_default_argument("evaluate_default_argument"),
  s_function_middle("() { return "),
  s_semicolon(";"),
  s_stdClass("stdClass");

void ExecutionContext::requestInit() {
  initBlackHole();
  createGlobalNVTable();
  vmStack().requestInit();
  ResourceHdr::resetMaxId();
  jit::tc::requestInit();

  *rl_num_coeffect_violations = 0;

  if (Cfg::Jit::EnableRenameFunction == 1) {
    assertx(SystemLib::s_anyNonPersistentBuiltins);
  }

  /*
   * The normal case for production mode is that all builtins are
   * persistent, and every systemlib unit is accordingly going to be
   * merge only.
   *
   * However, if we have builtins that are renamable, we'll actually
   * need to merge systemlib on every request because some of the
   * builtins will not be marked persistent.
   */
  if (UNLIKELY(SystemLib::s_anyNonPersistentBuiltins)) {
    SystemLib::mergePersistentUnits();
  }

  assertx(!ImplicitContext::emptyCtx.isInit());
  ImplicitContext::emptyCtx.initWith(initEmptyContext().detach());

  assertx(!ImplicitContext::activeCtx.isInit());
  ImplicitContext::activeCtx.initWith(*ImplicitContext::emptyCtx);
  (*ImplicitContext::activeCtx)->incRefCount();

  profileRequestStart();

#ifndef NDEBUG
  Class* cls = NamedType::getOrCreate(s_stdClass.get())->clsList();
  assertx(cls);
  assertx(cls == SystemLib::getstdClassClass());
#endif

  if (Logger::UseRequestLog) Logger::SetThreadHook(&m_logger_hook);

  // Needs to be last (or nearly last): might cause unit merging to call an
  // extension function in the VM; this is bad if systemlib itself hasn't been
  // merged.
  autoTypecheckRequestInit();

  if (!Cfg::Repo::Authoritative && Cfg::Eval::SampleRequestTearing) {
    if (StructuredLog::coinflip(Cfg::Eval::SampleRequestTearing)) {
      m_requestStartForTearing.emplace();
      Timer::GetRealtimeTime(*m_requestStartForTearing);
    }
  }
}

void ExecutionContext::requestExit() {
  RequestInfo *ti = RequestInfo::s_requestInfo.getNoCheck();
  RequestId rootReqId = ti->getRootRequestId();
  if (rootReqId != ti->m_id || m_xboxTasksStarted > 0) {
    // Set the script filename of request group when root request
    // is about to finish. It is important that setScriptFilename
    // is called before decrement.
    if (rootReqId == ti->m_id) {
      requestFanoutLimitSetScriptFilename(rootReqId, ServerNote::Get("DYNO_SCRIPT_FILENAME").c_str());
    }
    requestFanoutLimitDecrement(rootReqId);
  }

  apcExtension::purgeDeferred(std::move(m_apcDeferredExpire));

  autoTypecheckRequestExit();

  manageAPCHandle();
  syncGdbState();
  vmStack().requestExit();
  profileRequestEnd();
  EventHook::Disable();
  zend_rand_unseed();
  clearBlackHole();

  if (m_globalNVTable) {
    req::destroy_raw(m_globalNVTable);
    m_globalNVTable = nullptr;
  }

  if (!m_lastError.isNull()) {
    clearLastError();
  }

  if (!m_visitedFiles.isNull()) {
    m_visitedFiles = Array();
  }

  {
    m_deferredErrors = empty_vec_array();
  }

  if (Logger::UseRequestLog) Logger::SetThreadHook(nullptr);
  if (m_requestTrace) record_trace(std::move(*m_requestTrace));
  if (!Cfg::Repo::Authoritative) m_requestStartForTearing.reset();
  if (Cfg::Eval::LogDeclDeps) m_loadedRdepMap.clear();
}

/**
 * Enter VM by calling action(), which invokes a function or resumes
 * an async function. The 'ar' argument points to an ActRec of the
 * invoked/resumed function.
 */
template<class Action>
static inline void enterVM(ActRec* ar, Action action) {
  assertx(ar);
  assertx(!ar->sfp());
  assertx(isCallToExit(ar->m_savedRip));
  assertx(ar->callOffset() == 0);

  vmFirstAR() = ar;
  vmJitCalledFrame() = nullptr;
  vmJitReturnAddr() = 0;

  action();

  while (vmpc()) {
    exception_handler(enterVMAtCurPC);
  }
}

/*
 * Shared implementation for invokeFunc{,Few}().
 */
ALWAYS_INLINE
TypedValue ExecutionContext::invokeFuncImpl(const Func* f,
                                            ObjectData* thiz, Class* cls,
                                            uint32_t numPositionalArgs,
                                            const ArrayData* namedArgNames,
                                            RuntimeCoeffects providedCoeffects,
                                            bool hasGenerics, bool dynamic,
                                            bool allowDynCallNoPointer) {
  assertx(f);
  // If `f' is a regular function, `thiz' and `cls' must be null.
  assertx(IMPLIES(!f->implCls(), (!thiz && !cls)));
  // If `f' is a method, either `thiz' or `cls' must be non-null.
  assertx(IMPLIES(f->preClass(), thiz || cls));
  // If `f' is a static method, thiz must be null.
  assertx(IMPLIES(f->isStaticInPrologue(), !thiz));

  auto numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  ActRec* ar =
    vmStack().indA(numPositionalArgs + numNamedArgs + (hasGenerics ? 1 : 0));
  void* ctx = thiz ? (void*)thiz : (void*)cls;

  // Callee checks and input initialization.
  calleeGenericsChecks(f, hasGenerics);
  calleeNamedArgChecks(f, numPositionalArgs, namedArgNames);
  calleeArgumentArityChecks(f, numPositionalArgs);
  calleeArgumentTypeChecks(f, numPositionalArgs, namedArgNames, ctx);
  calleeDynamicCallChecks(f, dynamic, allowDynCallNoPointer);
  calleeCoeffectChecks(f, providedCoeffects, numPositionalArgs, ctx);
  f->recordCall();
  initFuncInputs(f, numPositionalArgs);

  ar->setReturnVMExit();
  ar->setFunc(f);
  if (thiz) {
    thiz->incRefCount();
    ar->setThis(thiz);
  } else if (cls) {
    ar->setClass(cls);
  } else {
    ar->trashThis();
  }

#ifdef HPHP_TRACE
  if (vmfp() == nullptr) {
    TRACE(1, "Reentry: enter %s(%p) from top-level\n",
          f->name()->data(), ar);
  } else {
    TRACE(1, "Reentry: enter %s(pc %p ar %p) from %s(%p)\n",
          f->name()->data(), vmpc(), ar,
          vmfp()->func() ? vmfp()->func()->name()->data()
                         : "unknownBuiltin",
          vmfp());
  }
#endif

  auto const reentrySP =
    reinterpret_cast<TypedValue*>(ar) + kNumActRecCells + f->numInOutParams();
  pushVMState(reentrySP);
  SCOPE_EXIT {
    assert_flog(
      vmStack().top() == reentrySP,
      "vmsp() mismatch around reentry: before @ {}, after @ {}",
      reentrySP, vmStack().top()
    );
    popVMState();
  };

  // If the request terminated with a C++ exception, we would not
  // have cleared the implicit context since that logic is
  // done in a PHP try-finally. Let's clear the implicit context here.
  auto const prev_ic = *ImplicitContext::activeCtx;
  try {
    enterVM(ar, [&] {
      exception_handler([&] {
        enterVMAtFunc(ar, numPositionalArgs,
                      numMatchedNamedArgs(f, namedArgNames) ==
                        f->numNamedParams());
      });
    });

    assertx(prev_ic == *ImplicitContext::activeCtx);

    if (UNLIKELY(f->takesInOutParams())) {
      VecInit vec(f->numInOutParams() + 1);
      for (uint32_t i = 0; i < f->numInOutParams() + 1; ++i) {
        vec.append(*vmStack().topTV());
        vmStack().popC();
      }
      return make_array_like_tv(vec.create());
    } else {
      auto const retval = *vmStack().topTV();
      vmStack().discard();
      return retval;
    }
  } catch (...) {
    // This is an explicit try-catch-rethrow rather than a SCOPE_EXIT
    // because std::uncaught_exceptions() is relatively expensive, and this
    // is very hot code.
    ImplicitContext::setActive(Object{prev_ic});
    throw;
  }
}

TypedValue ExecutionContext::invokeFunc(const Func* f,
                                        const Variant& args_,
                                        const ArrayData* namedArgNames /* = null */,
                                        ObjectData* thiz /* = NULL */,
                                        Class* cls /* = NULL */,
                                        RuntimeCoeffects providedCoeffects
                                              /* = RuntimeCoeffects::fixme() */,
                                        bool dynamic /* = true */,
                                        bool checkRefAnnot /* = false */,
                                        bool allowDynCallNoPointer
                                                              /* = false */,
                                        bool readonlyReturn /* = false */,
                                        Array&& generics /* = Array() */,
                                        const ArrayData* namedArgsToUnpack /* = null */) {
  VMRegAnchor _;

  // Check both the native stack and VM stack for overflow, numParams
  // is already included in f->maxStackCells().
  auto const unpackedNamedArgs = namedArgsToUnpack == nullptr
    ? 0
    : static_cast<int32_t>(namedArgsToUnpack->size());
  checkStack(vmStack(), f,
             f->numInOutParams() + kNumActRecCells + unpackedNamedArgs);

  // Reserve space for inout outputs and ActRec.
  for (auto i = f->numInOutParams(); i > 0; --i) vmStack().pushUninit();
  for (auto i = kNumActRecCells; i > 0; --i) vmStack().pushUninit();

  // Push arguments.
  auto const& args = *args_.asTypedValue();
  assertx(isContainerOrNull(args));
  if (namedArgsToUnpack != nullptr) {
    for (auto i = 0; i < namedArgsToUnpack->size(); ++i) {
      tvDup(namedArgsToUnpack->at(i), *vmStack().allocC());
    }
  }
  auto numPositionals = tvIsNull(args) ? 0 : getContainerSize(args);
  if (numPositionals > 0) {
    assertx(isContainer(args));
    tvDup(args, *vmStack().allocC());
    numPositionals = prepareUnpackArgs(f, 0, checkRefAnnot);
  }

  // Push generics.
  auto const hasGenerics = !generics.isNull();
  GenericsSaver::push(std::move(generics));

  // Caller checks.
  if (dynamic) callerDynamicCallChecks(f, allowDynCallNoPointer);
  if (f->attrs() & AttrReadonlyReturn && !readonlyReturn) {
    throwReadonlyMismatch(f, kReadonlyReturnId);
  }
  return invokeFuncImpl(f, thiz, cls, numPositionals,
                        namedArgNames, providedCoeffects, hasGenerics,
                        dynamic, allowDynCallNoPointer);
}

TypedValue ExecutionContext::invokeFuncFew(
  const Func* f,
  ExecutionContext::ThisOrClass thisOrCls,
  uint32_t numArgs,
  const ArrayData* namedArgNames /* = null */,
  const TypedValue* argv,
  RuntimeCoeffects providedCoeffects,
  bool dynamic /* = true */,
  bool allowDynCallNoPointer /* = false */) {
  VMRegAnchor _;
  auto& stack = vmStack();
  auto const numNamedArgs = static_cast<uint32_t>(
    namedArgNames ? namedArgNames->size() : 0
  );
  assertx(numArgs >= numNamedArgs);
  auto numPositionalArgs = numArgs - numNamedArgs;

  // See comments in invokeFunc().
  checkStack(stack, f,
             f->numInOutParams() + kNumActRecCells +
               static_cast<int32_t>(numNamedArgs));

  // Reserve space for inout outputs and ActRec.
  for (auto i = f->numInOutParams(); i > 0; --i) stack.pushUninit();
  for (auto i = kNumActRecCells; i > 0; --i) stack.pushUninit();

  // Named values precede positional values in argv and on the VM stack.
  for (auto i = 0; i < numNamedArgs; ++i) {
    const TypedValue *from = &argv[i];
    TypedValue* to = stack.allocTV();
    tvDup(*from, *to);
  }

  // Push non-variadic positional arguments.
  auto const numPositionalParams = f->numPositionalParams();
  auto const numNonVariadicPositionals =
    std::min(numPositionalArgs, numPositionalParams);
  for (auto i = 0; i < numNonVariadicPositionals; ++i) {
    auto const from = &argv[numNamedArgs + i];
    auto const to = stack.allocTV();
    tvDup(*from, *to);
  }

  // Pack excess positional arguments.
  if (UNLIKELY(numPositionalParams < numPositionalArgs)) {
    VecInit ai{numPositionalArgs - numPositionalParams};
    for (auto i = numPositionalParams; i < numPositionalArgs; ++i) {
      ai.append(argv[numNamedArgs + i]);
    }
    stack.pushArrayLikeNoRc(ai.create());
    numPositionalArgs = numPositionalParams + 1;
  }

  // Caller checks.
  if (dynamic) callerDynamicCallChecks(f, allowDynCallNoPointer);
  if (f->attrs() & AttrReadonlyReturn) {
    throwReadonlyMismatch(f, kReadonlyReturnId);
  }

  return invokeFuncImpl(f, thisOrCls.left(), thisOrCls.right(),
                        numPositionalArgs, namedArgNames,
                        providedCoeffects, false /* hasGenerics */, dynamic,
                        false);
}

static void prepareAsyncFuncEntry(ActRec* enterFnAr,
                                  Resumable* resumable,
                                  bool willUnwind) {
  assertx(enterFnAr);
  assertx(enterFnAr->func()->isAsync());
  assertx(isResumed(enterFnAr));
  assertx(resumable);

  vmfp() = enterFnAr;
  // If we're going to unwind, we need to resume at the offset we
  // suspended at, so we look up the correct catch trace (not the one
  // for the next op).
  vmpc() = enterFnAr->func()->at(
    willUnwind
      ? resumable->suspendOffset()
      : resumable->resumeFromAwaitOffset()
  );
  EventHook::FunctionResumeAwait(enterFnAr, EventHook::Source::Asio);
}

void ExecutionContext::resumeAsyncFunc(Resumable* resumable,
                                       ObjectData* freeObj,
                                       const TypedValue awaitResult) {
  assertx(regState() == VMRegState::CLEAN);
  assertx(*ImplicitContext::activeCtx);
  SCOPE_EXIT { assertx(regState() == VMRegState::CLEAN); };

  auto fp = resumable->actRec();
  ImplicitContext::setActive(Object{[&] {
    if (!fp->func()->isGenerator()) return frame_afwh(fp)->m_implicitContext;
    auto gen = frame_async_generator(fp);
    return gen->getWaitHandle()->m_implicitContext;
  }()});

  // We don't need to check for space for the ActRec (unlike generally
  // in normal re-entry), because the ActRec isn't on the stack.
  checkStack(vmStack(), fp->func(), 0);

  TypedValue* savedSP = vmStack().top();
  tvDup(awaitResult, *vmStack().allocC());

  // decref after awaitResult is on the stack (null when no child on WH)
  if (freeObj) {
    decRefObj(freeObj);
  }

  pushVMState(savedSP);
  SCOPE_EXIT { popVMState(); };

  enterVM(fp, [&] {
    exception_handler([&] {
      prepareAsyncFuncEntry(fp, resumable, false);

      const bool useJit = RID().getJit();
      if (LIKELY(useJit && resumable->resumeAddr())) {
        Stats::inc(Stats::VMEnter);
        jit::enterTC(jit::JitResumeAddr::trans(resumable->resumeAddr()));
      } else {
        enterVMAtCurPC();
      }
    });
  });
}

void ExecutionContext::resumeAsyncFuncThrow(Resumable* resumable,
                                            ObjectData* freeObj,
                                            ObjectData* exception) {
  assertx(exception);
  assertx(exception->instanceof(SystemLib::getThrowableClass()));
  assertx(regState() == VMRegState::CLEAN);
  assertx(*ImplicitContext::activeCtx);
  SCOPE_EXIT { assertx(regState() == VMRegState::CLEAN); };

  auto fp = resumable->actRec();
  ImplicitContext::setActive(Object{[&] {
    if (!fp->func()->isGenerator()) return frame_afwh(fp)->m_implicitContext;
    auto gen = frame_async_generator(fp);
    return gen->getWaitHandle()->m_implicitContext;
  }()});

  checkStack(vmStack(), fp->func(), 0);

  // decref after we hold reference to the exception
  Object e(exception);
  decRefObj(freeObj);

  pushVMState(vmStack().top());
  SCOPE_EXIT { popVMState(); };

  enterVM(fp, [&] {
    DEBUG_ONLY auto const success = exception_handler([&] {
      prepareAsyncFuncEntry(fp, resumable, true);
    });
    // Function entry may fail only with a C++ exception thrown by the event
    // hook, which would be rethrown by the exception_handler() after unwinding
    // the current frame.
    assertx(success);

    unwindVM(exception);
    e.reset();
  });
}

ActRec* ExecutionContext::getPrevVMState(const ActRec* fp,
                                         Offset* prevPc /* = NULL */,
                                         TypedValue** prevSp /* = NULL */,
                                         bool* fromVMEntry /* = NULL */,
                                         jit::TCA* jitReturnAddr /* = NULL */) {
  if (fp == nullptr) {
    return nullptr;
  }
  ActRec* prevFp = fp->sfp();
  if (LIKELY(prevFp != nullptr)) {
    if (prevSp) {
      if (UNLIKELY(isResumed(fp))) {
        assertx(fp->func()->isGenerator());
        *prevSp = (TypedValue*)prevFp - prevFp->func()->numSlotsInFrame();
      } else {
        *prevSp = (TypedValue*)(fp + 1);
      }
    }
    if (prevPc) *prevPc = fp->callOffset();
    if (fromVMEntry) *fromVMEntry = false;
    if (jitReturnAddr) *jitReturnAddr = (jit::TCA)fp->m_savedRip;
    return prevFp;
  }
  // Linear search from end of m_nestedVMs. In practice, we're probably
  // looking for something recently pushed.
  int i = m_nestedVMs.size() - 1;
  ActRec* firstAR = vmFirstAR();
  while (i >= 0 && firstAR != fp) {
    firstAR = m_nestedVMs[i--].firstAR;
  }
  if (i == -1) return nullptr;
  const VMState& vmstate = m_nestedVMs[i];
  prevFp = vmstate.fp;
  assertx(prevFp);
  assertx(prevFp->func()->unit());
  if (prevSp) *prevSp = vmstate.sp;
  if (prevPc) {
    *prevPc = prevFp->func()->offsetOf(vmstate.pc);
  }
  if (fromVMEntry) *fromVMEntry = true;
  if (jitReturnAddr) *jitReturnAddr = vmstate.jitReturnAddr;
  return prevFp;
}

Variant ExecutionContext::getEvaledArg(const StringData* val,
                                       const OptString& namespacedName,
                                       const Unit* funcUnit) {
  auto key = StrNR(val);

  if (m_evaledArgs.get()) {
    auto const arg = m_evaledArgs.get()->get(key);
    if (arg.is_init()) return Variant::wrap(arg);
  }

  OptString code;
  OptString funcName;
  int pos = namespacedName.rfind('\\');
  if (pos != -1) {
    auto ns = namespacedName.substr(0, pos);
    code = s_hh + s_namespace + ns + s_curly_start + s_function_start +
      s_evaluate_default_argument + s_function_middle + key +
      s_semicolon + s_curly_end + s_curly_end;
    funcName = ns + "\\" + s_evaluate_default_argument;
  } else {
    code = s_hh + s_function_start + s_evaluate_default_argument +
      s_function_middle + key + s_semicolon + s_curly_end;
    funcName = s_evaluate_default_argument;
  }

  Unit* unit = compileEvalString(code.get());
  // This exception will be caught by the caller and proper error message
  // will be emitted
  if (unit->getFatalInfo()) throw Exception("Parse error");
  assertx(unit != nullptr);
  unit->setInterpretOnly();

  // The evaluate_default_argument function should be the last one
  assertx(unit->funcs().size() >= 1);
  auto func = *(unit->funcs().end() - 1);
  assertx(func->name()->equal(funcName.get()) &&
          "We expecting the evaluate_default_argument func");

  // Default arg values are not currently allowed to depend on class context.
  auto v = Variant::attach(
    // TODO(named_params): Support eval with named args.
    g_context->invokeFuncFew(func, nullptr, 0, nullptr, nullptr,
                             RuntimeCoeffects::defaults(), true, true)
  );
  m_evaledArgs.set(key, *v.asTypedValue());
  return Variant::wrap(m_evaledArgs.lookup(key));
}

void ExecutionContext::recordLastError(const Exception& e, int errnum) {
  m_lastError = OptString(e.getMessage());
  m_lastErrorNum = errnum;
  m_lastErrorPath = OptString::attach(getContainingFileName());
  m_lastErrorLine = getLine();
  if (auto const ee = dynamic_cast<const ExtendedException*>(&e)) {
    m_lastErrorPath = ee->getFileAndLine().first;
    m_lastErrorLine = ee->getFileAndLine().second;
  }
}

void ExecutionContext::clearLastError() {
  m_lastError = OptString();
  m_lastErrorNum = 0;
  m_lastErrorPath = staticEmptyString();
  m_lastErrorLine = 0;
}

void ExecutionContext::enqueueAPCDeferredExpire(const OptString& key) {
  auto keyStr = key.get();
  keyStr->incRefCount();
  m_apcDeferredExpire.push_back(keyStr);
}

void ExecutionContext::enqueueAPCHandle(APCHandle* handle, size_t size) {
  assertx(handle->isShared());
  m_apcHandles.push_back(handle);
  m_apcMemSize += size;
}

// Treadmill solution for the SharedVariant memory management
namespace {
struct FreedAPCHandle {
  explicit FreedAPCHandle(std::vector<APCHandle*>&& shandles, size_t size)
    : m_memSize(size), m_apcHandles(std::move(shandles))
  {}
  void operator()() {
    for (auto handle : m_apcHandles) {
      APCTypedValue::fromHandle(handle)->deleteShared();
    }
    APCStats::getAPCStats().removePendingDelete(m_memSize);
  }
private:
  size_t m_memSize;
  std::vector<APCHandle*> m_apcHandles;
};
}

void ExecutionContext::manageAPCHandle() {
  assertx(apcExtension::UseUncounted || m_apcHandles.size() == 0);
  if (m_apcHandles.size() > 0) {
    Treadmill::enqueue(
      FreedAPCHandle(std::move(m_apcHandles), m_apcMemSize)
    );
    APCStats::getAPCStats().addPendingDelete(m_apcMemSize);
  }
}

ExecutionContext::EvaluationResult
ExecutionContext::evalPHPDebugger(StringData* code, int frame) {
  // The code has "<?hh" prepended already
  auto unit = compile_debugger_string(code->slice(),
    getRepoOptionsForFrame(frame));
  if (unit == nullptr) {
    raise_error("Syntax error");
    return {true, init_null_variant, "Syntax error"};
  }
  auto const file = unit->filepath()->data();
  if (!m_requestOptions) {
    onLoadWithOptions(file, RepoOptions::forFile(file));
  }
  auto const debugger = HPHP::VSDEBUG::VSDebugExtension::getDebugger();
  HPHP::VSDEBUG::DebuggerEvaluationContext debuggerContext(debugger);
  return evalPHPDebugger(unit, frame);
}

const StaticString
  s_DebuggerMainAttr("__DebuggerMain"),
  s_debuggerThis("__debugger$this"),
  s_uninitClsName("__uninitSentinel");

ExecutionContext::EvaluationResult
ExecutionContext::evalPHPDebugger(Unit* unit, int frame) {
  always_assert(!Cfg::Repo::Authoritative);

  // Do not JIT this unit, we are using it exactly once.
  unit->setInterpretOnly();

  VMRegAnchor _;

  auto fp = getFrameAtDepthForDebuggerUnsafe(frame);

  // Continue walking up the stack until we find a frame that can have
  // a variable environment context attached to it, or we run out out frames.
  while (fp && (fp->skipFrame() || fp->isInlined())) {
    fp = getPrevVMStateSkipFrame(fp);
  }

  if (fp) phpDebuggerEvalHook(fp->func());

  const static StaticString s_cppException("Hit an exception");
  const static StaticString s_phpException("Hit a php exception");
  const static StaticString s_exit("Hit exit");
  const static StaticString s_fatal("Hit fatal");
  std::ostringstream errorString;
  std::string stack;

  // Find a suitable PC to use when switching to the target frame. If the target
  // is the current frame, this is just vmpc(). For other cases, this will
  // generally be the address of a call from that frame's function. If we can't
  // find the target frame (because it lies deeper in the stack), then just use
  // the target frame's func's entry point.
  auto const findSuitablePC = [this](const ActRec* target){
    if (auto fp = vmfp()) {
      if (fp == target) return vmpc();
      while (true) {
        auto prevFp = getPrevVMState(fp);
        if (!prevFp) break;
        if (prevFp == target) {
          return prevFp->func()->entry() + fp->callOffset();
        }
        fp = prevFp;
      }
    }
    return target->func()->entry();
  };

  try {
    // We also need to change vmpc() to match, since we assert in a few places
    // that the vmpc() lies within vmfp()'s code.
    auto savedFP = vmfp();
    auto savedPC = vmpc();
    if (fp) {
      vmpc() = findSuitablePC(fp);
      vmfp() = fp;
    }
    SCOPE_EXIT { vmpc() = savedPC; vmfp() = savedFP; };
    unit->merge();

    enum VarAction { StoreFrame, StoreEnv };

    auto const uninit_cls = Class::load(s_uninitClsName.get());

    auto& env = m_debuggerEnv;

    auto const ctx = fp ? fp->func()->cls() : nullptr;
    auto const f = [&] () -> Func* {
      for (auto orig_f : unit->funcs()) {
        if (orig_f->userAttributes().contains(s_DebuggerMainAttr.get())) {
          auto const f = ctx ? orig_f->clone(ctx) : orig_f;
          if (ctx) {
            f->setBaseCls(ctx);
            if (fp && fp->hasThis()) {
              f->setAttrs(Attr(f->attrs() & ~AttrStatic));
            } else if (fp && fp->hasClass()) {
              f->setAttrs(Attr(f->attrs() | AttrStatic));
            }
          }
          assertx(f->numParams() > 0);
          return f;
        }
      }
      return nullptr;
    }();
    always_assert(f);

    VecInit args{f->numParams()};
    std::vector<VarAction> actions{f->numParams(), StoreEnv};
    std::vector<Id> frameIds;
    frameIds.resize(f->numParams(), 0);
    auto const appendUninit = [&] {
      args.append(make_tv<KindOfObject>(Object{uninit_cls}.detach()));
    };
    for (Id id = 0; id < f->numParams() - 1; id++) {
      assertx(id < f->numNamedLocals());
      assertx(!f->params()[id].isInOut());
      assertx(!f->params()[id].isVariadic());
      if (f->localVarName(id)->equal(s_debuggerThis.get()) &&
          ctx && fp->hasThis()) {
        args.append(make_tv<KindOfObject>(fp->getThis()));
        continue;
      }
      if (fp) {
        auto const idx = fp->func()->lookupVarId(f->localVarName(id));
        if (idx != kInvalidId) {
          Variant var{tvAsCVarRef(*frame_local(fp, idx))};
          if (var.isInitialized()) args.append(var);
          else appendUninit();
          actions[id] = StoreFrame;
          frameIds[id] = idx;
          continue;
        }
      }
      auto const val = env.lookup(StrNR{f->localVarName(id)});
      if (val.is_init()) args.append(val);
      else appendUninit();
    }
    args.append(make_tv<KindOfNull>()); // $__debugger_exn$output

    auto const obj = ctx && fp->hasThis() ? fp->getThis() : nullptr;
    auto const cls = ctx && fp->hasClass() ? fp->getClass() : nullptr;
    // TODO(named_params) support debugging calls with named args here
    const ArrayData* namedArgNames = nullptr;
    auto const wh = invokeFunc(f, args.toArray(), namedArgNames, obj, cls,
                               RuntimeCoeffects::defaults(), false);
    assertx(f->isAsync());
    assertx(tvIsObject(wh));
    auto const arr_tv = HHVM_FN(join)(Object::attach(wh.m_data.pobj));
    assertx(isArrayLikeType(arr_tv.getType()));
    Array arr = arr_tv.toArray();
    assertx(arr->size() == f->numParams() + 1);
    for (Id id = 0; id < f->numParams() - 1; id++) {
      auto const tv = arr.lookup(id + 1);
      if (isObjectType(type(tv)) &&
          val(tv).pobj->instanceof(uninit_cls)) {
        switch (actions[id]) {
        case StoreFrame:
          variant_ref{frame_local(fp, frameIds[id])}.unset();
          break;
        case StoreEnv:
          env.remove(StrNR{f->localVarName(id)});
          break;
        }
        continue;
      }
      switch (actions[id]) {
      case StoreFrame:
        variant_ref{frame_local(fp, frameIds[id])} = arr[id + 1];
        break;
      case StoreEnv:
        env.set(StrNR{f->localVarName(id)}, tv);
        break;
      }
    }
    auto const exn = arr[int64_t(f->numParams())];
    if (exn.isObject()) {
      assertx(exn.toObject().instanceof("Throwable"));
      throw_object(exn.toObject());
    }
    assertx(exn.isNull());

    return {false, arr[0], ""};
  } catch (FatalErrorException& e) {
    errorString << s_fatal.data();
    errorString << " : ";
    errorString << e.getMessage().c_str();
    errorString << "\n";
    stack = ExtendedLogger::StringOfStackTrace(e.getBacktrace());
  } catch (ExitException&) {
    errorString << s_exit.data();
    errorString << " : ";
    errorString << *rl_exit_code;
  } catch (Eval::DebuggerException&) {
  } catch (Exception& e) {
    errorString << s_cppException.data();
    errorString << " : ";
    errorString << e.getMessage().c_str();
    ExtendedException* ee = dynamic_cast<ExtendedException*>(&e);
    if (ee) {
      errorString << "\n";
      stack = ExtendedLogger::StringOfStackTrace(ee->getBacktrace());
    }
  } catch (Object &e) {
    errorString << s_phpException.data();
    errorString << " : ";
    try {
      errorString << throwable_to_string(e.get()).data();
    } catch (...) {
      errorString << e->getVMClass()->name()->data();
    }
  } catch (...) {
    errorString << s_cppException.data();
  }

  auto errorStr = errorString.str();
  g_context->write(errorStr);
  if (!stack.empty()) {
    g_context->write(stack.c_str());
  }

  return {true, init_null_variant, errorStr};
}

const StaticString s_include("include");

void ExecutionContext::enterDebuggerDummyEnv() {
  static Unit* s_debuggerDummy = compile_debugger_string(
    "<?hh", getRepoOptionsForDebuggerEval()
  );
  // Ensure that the VM stack is completely empty (vmfp() should be null)
  // and that we're not in a nested VM (reentrancy)
  assertx(vmfp() == nullptr);
  assertx(m_nestedVMs.size() == 0);
  assertx(m_nesting == 0);
  assertx(vmStack().count() == 0);
  ActRec* ar = vmStack().allocA();
  ar->setFunc(s_debuggerDummy->lookupFuncId(0));
  assertx(ar->func() && ar->func()->name()->equal(s_include.get()));
  for (int i = 0; i < ar->func()->numLocals(); ++i) vmStack().pushInt(1);
  ar->trashThis();
  ar->setReturnVMExit();
  vmfp() = ar;
  vmpc() = ar->func()->entry();
  vmFirstAR() = ar;
}

void ExecutionContext::exitDebuggerDummyEnv() {
  assertx(m_globalNVTable);
  // Ensure that vmfp() is valid
  assertx(vmfp() != nullptr);
  // Ensure that vmfp() points to the only frame on the call stack.
  // In other words, make sure there are no VM frames directly below
  // this one and that we are not in a nested VM (reentrancy)
  assertx(!vmfp()->sfp());
  assertx(m_nestedVMs.size() == 0);
  assertx(m_nesting == 0);
  // Teardown the frame we erected by enterDebuggerDummyEnv()
  const Func* func = vmfp()->func();
  try {
    vmfp()->setLocalsDecRefd();
    frame_free_locals_no_hook(vmfp());
  } catch (...) {}
  vmStack().ndiscard(func->numSlotsInFrame());
  vmStack().discardAR();
  // After tearing down this frame, the VM stack should be completely empty
  assertx(vmStack().count() == 0);
  vmfp() = nullptr;
  vmpc() = nullptr;
}

ThrowAllErrorsSetter::ThrowAllErrorsSetter() {
  m_throwAllErrors = g_context->getThrowAllErrors();
  g_context->setThrowAllErrors(true);
}

ThrowAllErrorsSetter::~ThrowAllErrorsSetter() {
  g_context->setThrowAllErrors(m_throwAllErrors);
}

GloballySuppressNonFatals::GloballySuppressNonFatals() {
  auto const wasSet = s_GloballySuppressNonFatals.exchange(true);
  always_assert(!wasSet);
}

GloballySuppressNonFatals::~GloballySuppressNonFatals() {
  auto const wasSet = s_GloballySuppressNonFatals.exchange(false);
  always_assert(wasSet);
}

}
