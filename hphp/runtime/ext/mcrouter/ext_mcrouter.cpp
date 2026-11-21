#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/ext/asio/asio-external-thread-event.h"

#include <memory>
#include <stdexcept>

#include <folly/Memory.h>
#include <folly/Range.h>

#include "mcrouter/config.h" // @nolint
#include "mcrouter/McrouterClient.h" // @nolint
#include "mcrouter/McrouterInstance.h" // @nolint
#include "mcrouter/lib/McResUtil.h" // @nolint
#include "mcrouter/lib/carbon/Result.h" // @nolint
#include "mcrouter/lib/network/CarbonMessageList.h" // @nolint
#include "mcrouter/lib/network/gen/MemcacheMessages.h" // @nolint
#include "mcrouter/options.h" // @nolint

namespace mc  = facebook::memcache;
namespace mcr = facebook::memcache::mcrouter;

namespace HPHP {
/////////////////////////////////////////////////////////////////////////////

struct MCRouterResult;

const StaticString
  s_MCRouter("MCRouter"),
  s_option("option"),
  s_error("error"),
  s_value("value"),
  s_cas("cas"),
  s_flags("flags");

/**
 * Below are to maintain the compatibility during the migration
 * from mc_res_t to enum class carbon::Result
 */
namespace compatibility {
#ifdef CARBON_RESULT_ENUM_CLASS
const carbon::Result mc_res_unknown = carbon::Result::UNKNOWN;
const carbon::Result mc_res_deleted = carbon::Result::DELETED;
const carbon::Result mc_res_ok = carbon::Result::OK;
const carbon::Result mc_nres = carbon::Result::NUM_RESULTS;

const char* carbonResultToString(const carbon::Result result) {
  return carbon::resultToString(result);
}
#else
const carbon::Result mc_res_unknown = ::mc_res_unknown;
const carbon::Result mc_res_deleted = ::mc_res_deleted;
const carbon::Result mc_res_ok = ::mc_res_ok;
const carbon::Result mc_nres = ::mc_nres;

const char* carbonResultToString(const carbon::Result result) {
  return mc_res_to_string(result);
}
#endif
} // namespace compatibility

namespace detail {
// Flags helpers
// TODO: Once OSS McRouter version is staged, this code can be
// removed and the methods in mcrouter/lib/network/MessageHelpers.h
// used instead.
template <typename Message>
uint64_t getFlagsImpl(std::true_type, Message& message) {
  return *message.flags_ref();
}
template <typename Message>
uint64_t getFlagsImpl(std::false_type, Message&) {
  return 0;
}
template <typename Message, typename = std::enable_if_t<true>>
class HasFlagsTrait : public std::false_type {};
template <typename Message>
class HasFlagsTrait<
    Message,
    std::enable_if_t<std::is_same<
        decltype(std::declval<std::remove_const_t<Message>&>().flags_ref()),
        std::uint64_t&>{}>> : public std::true_type {};
// Return flags if it exists in Message and 0 otherwise.
template <class Message>
uint64_t getFlagsIfExist(Message& message) {
  return detail::getFlagsImpl(HasFlagsTrait<Message>{}, message);
}
}

namespace {
struct MCRouterException : SystemLib::ClassLoader<"MCRouterException"> {};

struct MCRouterOptionException :
  SystemLib::ClassLoader<"MCRouterOptionException"> {};
}

[[noreturn]]
static void mcr_throwException(const std::string& message,
                               mc_op_t op = mc_op_unknown,
                               carbon::Result result = compatibility::mc_res_unknown,
                               const std::string& key = "") {
  throw_object(MCRouterException::classof(),
               make_vec_array(message, (int64_t)op, (int64_t)result, key));
}

[[noreturn]]
static void mcr_throwOptionException(
  const std::vector<mc::McrouterOptionError>& errors) {

  VecInit errorArray(errors.size());
  for (auto err : errors) {
    auto e = make_dict_array(
      s_option, OptString(err.requestedName),
      s_value, OptString(err.requestedValue),
      s_error, OptString(err.errorMsg)
    );
    errorArray.append(e);
  }

  throw_object(MCRouterOptionException::classof(),
               make_vec_array(errorArray.toArray()));
}

namespace {

// Helpers for retrieving 'delta' field
template <class Reply>
uint64_t getDelta(const Reply& /*reply*/) {
  mcr_throwException(
      "getDelta expected arithmetic reply type",
      mc::OpFromType<Reply, mc::ReplyOpMapping>::value);
}
uint64_t getDelta(const mc::McIncrReply& reply) {
  return *reply.delta();
}
uint64_t getDelta(const mc::McDecrReply& reply) {
  return *reply.delta();
}

// Helpers for retrieving 'casToken' field
template <class Reply>
uint64_t getCasToken(const Reply& /*reply*/) {
  mcr_throwException(
      "getCasToken expected reply type McGetsReply",
      mc::OpFromType<Reply, mc::ReplyOpMapping>::value);
}
uint64_t getCasToken(const mc::McGetsReply& reply) {
  return *reply.casToken();
}

} // anonymous

/////////////////////////////////////////////////////////////////////////////

struct MCRouter {
  MCRouter() = default;
  MCRouter& operator=(const MCRouter& str) = delete;

  template <class Request>
  void send(std::unique_ptr<const Request> request, MCRouterResult* res);

  void init(const Array& options, const OptString& pid) {
    mc::McrouterOptions opts;
    parseOptions(opts, options);

    mcr::McrouterInstance* router;
    try {
      if (pid.empty()) {
        m_transientRouter = mcr::McrouterInstance::create(opts.clone());
        router = m_transientRouter.get();
      } else {
        router = mcr::McrouterInstance::init(pid.toCppString(), opts);
      }
    } catch (const std::runtime_error& e) {
      mcr_throwException(e.what());
    }

    if (!router) {
      mcr_throwException("Unable to initialize MCRouter instance");
    }

    m_client = router->createClient(0 /* disable max_outstanding_requests */);

    if (!m_client) {
      mcr_throwException("Unable to initialize MCRouterClient instance");
    }
  }

  template <class Request>
  Object issue(std::unique_ptr<const Request> request);

 private:
  std::shared_ptr<mcr::McrouterInstance> m_transientRouter;
  mcr::McrouterClient::Pointer m_client;

  void parseOptions(mc::McrouterOptions& opts, const Array& options) {
#ifdef HPHP_OSS
    // Change defaults for these since they make assumptions about the system
    opts.asynclog_disable = true;
    opts.async_spool = "";
    opts.stats_logging_interval = 0;
    opts.stats_root = "";
#endif

    std::unordered_map<std::string, std::string> dict;
    for (ArrayIter iter(options); iter; ++iter) {
      auto key = iter.first().toString().toCppString();
      auto val = iter.second();
      if (val.isBoolean()) {
        // false -> toString() == "" which will fail foll::to<bool>
        dict[key] = val.toBoolean() ? "1" : "0";
      } else {
        dict[key] = val.toString().toCppString();
      }
    }

    auto errors = opts.updateFromDict(dict);
    if (!errors.empty()) {
      mcr_throwOptionException(errors);
    }
  }
};

struct MCRouterResult : AsioExternalThreadEvent {
  template <class Request>
  MCRouterResult(MCRouter* router, std::unique_ptr<const Request> request) {
    m_result.m_type = KindOfNull;
    router->send(std::move(request), this);
  }

  /**
   * Unserialize happens in the request thread where we can allocate smart
   * pointers. Use this opportunity to marshal the saved data from persistent
   * data structures into per-request data.
   */
  void unserialize(TypedValue& c) override {
    if (!m_exception.empty()) {
      mcr_throwException(m_exception, m_op, m_replyCode, m_key);
    }
    if ((m_result.m_type == KindOfString) && !m_result.m_data.pstr) {
      // Deferred string init, see below
      m_result.m_data.pstr = StringData::Make(
        m_stringResult.c_str(), m_stringResult.size(), CopyString);
      m_stringResult.clear();
    } else if ((m_result.m_type == KindOfResource) && !m_result.m_data.pres) {
      // Deferred string value and cas, see below
      Array ret = make_dict_array(
        s_value,
          OptString(m_stringResult.c_str(), m_stringResult.size(), CopyString),
        s_cas, (int64_t)m_cas,
        s_flags, (int64_t)m_flags
      );
      m_result = make_array_like_tv(ret.detach());
      m_stringResult.clear();
    }
    tvDup(m_result, c);
  }

  /* Callback invoked by libmcrouter on the receipt of a reply.
   * We're in the worker thread here, so we can't do any request
   * allocations or the memory manager will get confused.
   * We also can't store `msg' directly on the object as it'll be
   * freed after the result_ref() method returns.
   *
   * Marshal the data we actually care about into fields on this
   * object, then remarshal them into smart_ptr structures during unserialize()
   */
  template <class Request>
  void result(const Request& request, mc::ReplyT<Request>&& reply) {
    if (mc::isErrorResult(*reply.result_ref())) {
      setResultException(request, reply);
    } else {
      const auto mc_op =
          mc::OpFromType<Request, mc::RequestOpMapping>::value;
      switch (mc_op) {
        case mc_op_add:
        case mc_op_cas:
        case mc_op_set:
        case mc_op_replace:
        case mc_op_prepend:
        case mc_op_append:
          if (!mc::isStoredResult(*reply.result_ref())) {
            setResultException(request, reply);
            break;
          }
          break;

        case mc_op_flushall:
          if (*reply.result_ref() != compatibility::mc_res_ok) {
            setResultException(request, reply);
            break;
          }
          break;

        case mc_op_delete:
          if (*reply.result_ref() != compatibility::mc_res_deleted) {
            setResultException(request, reply);
            break;
          }
          break;

        case mc_op_incr:
        case mc_op_decr:
          if (!mc::isStoredResult(*reply.result_ref())) {
            setResultException(request, reply);
            break;
          }
          m_result.m_type = KindOfInt64;
          m_result.m_data.num = getDelta(reply);
          break;

        case mc_op_gets:
          m_cas = getCasToken(reply);
          [[fallthrough]];
        case mc_op_get:
          m_flags = detail::getFlagsIfExist(reply);
          if (mc::isMissResult(*reply.result_ref())) {
            setResultException(request, reply);
            break;
          }
          [[fallthrough]];
        case mc_op_version:
          // We can only allocate memory in the memory-manager thread so stash
          // the data in a std::string until we get to unserialize(). We use a
          // sentinal nullptr and a sentinel datatype here.
          m_result.m_type = mc_op == mc_op_gets ? KindOfResource : KindOfString;
          m_result.m_data.pstr = nullptr;
          m_stringResult = carbon::valueRangeSlow(reply).str();
          break;

        default:
          always_assert(false);
      }
    }
    markAsFinished();
  }

 private:

  // Store the important parts of the exception in non-thread vars
  // to bubble up during unserialize
  template <class Request>
  void setResultException(const Request& request,
                          const mc::ReplyT<Request>& reply) {
    m_op = mc::OpFromType<Request, mc::RequestOpMapping>::value;
    m_replyCode = *reply.result_ref();
    m_exception  = mc_op_to_string(m_op);
    m_exception += " failed with result ";
    m_exception += compatibility::carbonResultToString(m_replyCode);
    if (!reply.message_ref()->empty()) {
      m_exception += ": ";
      m_exception += *reply.message_ref();
    }
    m_key = request.key_ref()->fullKey().str();
  }

  TypedValue m_result;

  // Deferred string result and metadata
  std::string m_stringResult;
  uint64_t m_cas{0};
  uint64_t m_flags{0};

  // Deferred exception data
  mc_op_t m_op;
  carbon::Result m_replyCode;
  std::string m_exception, m_key;
};

template <class Request>
void MCRouter::send(std::unique_ptr<const Request> request,
                    MCRouterResult* res) {
  auto requestPtr = request.get();
  m_client->send(
      *requestPtr,
      [res, request = std::move(request)](const Request& req,
                                          mc::ReplyT<Request>&& reply) {
        if (*reply.result_ref() == compatibility::mc_res_unknown) {
          // McrouterClient has signaled this request is cancelled
          res->cancel();
        } else {
          res->result(req, std::move(reply));
        }
      });
}

template <class Request>
Object MCRouter::issue(std::unique_ptr<const Request> request) {
  auto ev = new MCRouterResult(this, std::move(request));
  try {
    return Object{ev->getWaitHandle()};
  } catch (...) {
    assertx(false);
    ev->abandon();
    throw;
  }
}

/////////////////////////////////////////////////////////////////////////////

static void HHVM_METHOD(MCRouter, __construct,
                        const Array& opts, const OptString& pid) {
  Native::data<MCRouter>(this_)->init(opts, pid);
}

template <class M>
static Object mcr_str(ObjectData* this_, const OptString& key) {
  return Native::data<MCRouter>(this_)->issue(
      std::make_unique<const M>(folly::StringPiece(key.c_str(), key.size())));
}

template <class Request>
static Object mcr_set(ObjectData* this_,
                      const OptString& key, const OptString& val,
                      int64_t flags, int64_t expiration) {
  auto request =
      std::make_unique<Request>(folly::StringPiece(key.c_str(), key.size()));
  request->value_ref() = folly::IOBuf(
      folly::IOBuf::COPY_BUFFER, folly::StringPiece(val.c_str(), val.size()));
  request->flags_ref() = flags;
  request->exptime_ref() = expiration;

  return Native::data<MCRouter>(this_)->issue<Request>(std::move(request));
}

template <class Request>
static Object mcr_aprepend(ObjectData* this_,
                           const OptString& key, const OptString& val) {
  auto request =
      std::make_unique<Request>(folly::StringPiece(key.c_str(), key.size()));
  request->value_ref() = folly::IOBuf(
      folly::IOBuf::COPY_BUFFER, folly::StringPiece(val.c_str(), val.size()));

  return Native::data<MCRouter>(this_)->issue<Request>(std::move(request));
}

template <class Request>
static Object mcr_str_delta(ObjectData* this_,
                            const OptString& key, int64_t val) {
  auto request =
      std::make_unique<Request>(folly::StringPiece(key.c_str(), key.size()));
  request->delta_ref() = val;

  return Native::data<MCRouter>(this_)->issue<Request>(std::move(request));
}

static Object mcr_flushall(ObjectData* this_, int64_t val) {
  using Request = mc::McFlushAllRequest;

  auto request = std::make_unique<Request>("unused");
  request->delay() = val;

  return Native::data<MCRouter>(this_)->issue<Request>(std::move(request));
}

static Object mcr_version(ObjectData* this_) {
  return Native::data<MCRouter>(this_)->issue(
      std::make_unique<const mc::McVersionRequest>("unused"));
}

static Object HHVM_METHOD(MCRouter, cas,
                          int64_t cas,
                          const OptString& key,
                          const OptString& val,
                          int64_t expiration /*=0*/) {
  using Request = mc::McCasRequest;

  auto request = std::make_unique<Request>(
      folly::StringPiece(key.c_str(), key.size()));
  request->value() = folly::IOBuf(
      folly::IOBuf::COPY_BUFFER, folly::StringPiece(val.c_str(), val.size()));
  request->exptime() = expiration;
  request->casToken() = cas;

  return Native::data<MCRouter>(this_)->issue<Request>(std::move(request));
}

/////////////////////////////////////////////////////////////////////////////

static OptString HHVM_STATIC_METHOD(MCRouter, getOpName, int64_t op) {
  auto name = mc_op_to_string((mc_op_t)op);
  if (!name) {
    std::string msg = "Unknown mc_op_* value: ";
    msg += op;
    mcr_throwException(msg, (mc_op_t)op);
  }
  return name;
}

static OptString HHVM_STATIC_METHOD(MCRouter, getResultName, int64_t res) {
  auto name = compatibility::carbonResultToString((carbon::Result)res);
  if (!name) {
    std::string msg = "Unknown mc_res_* value: ";
    msg += res;
    mcr_throwException(msg, mc_op_unknown, (carbon::Result)res);
  }
  return name;
}

/////////////////////////////////////////////////////////////////////////////

struct MCRouterExtension : Extension {
  MCRouterExtension(): Extension("mcrouter", "1.0.0", "memcache") {}

  void moduleRegisterNative() override {
    HHVM_ME(MCRouter, __construct);

    HHVM_NAMED_ME(MCRouter, get,  mcr_str<mc::McGetRequest>);
    HHVM_NAMED_ME(MCRouter, gets, mcr_str<mc::McGetsRequest>);

    HHVM_NAMED_ME(MCRouter, add, mcr_set<mc::McAddRequest>);
    HHVM_NAMED_ME(MCRouter, set, mcr_set<mc::McSetRequest>);
    HHVM_NAMED_ME(MCRouter, replace, mcr_set<mc::McReplaceRequest>);
    HHVM_NAMED_ME(MCRouter, prepend, mcr_aprepend<mc::McPrependRequest>);
    HHVM_NAMED_ME(MCRouter, append, mcr_aprepend<mc::McAppendRequest>);

    HHVM_NAMED_ME(MCRouter, incr, mcr_str_delta<mc::McIncrRequest>);
    HHVM_NAMED_ME(MCRouter, decr, mcr_str_delta<mc::McDecrRequest>);

    HHVM_NAMED_ME(MCRouter, del, mcr_str<mc::McDeleteRequest>);
    HHVM_NAMED_ME(MCRouter, flushAll, mcr_flushall);

    HHVM_NAMED_ME(MCRouter, version, mcr_version);

    HHVM_ME(MCRouter, cas);

    Native::registerNativeDataInfo<MCRouter>(s_MCRouter.get());

    HHVM_STATIC_ME(MCRouter, getOpName);
    HHVM_STATIC_ME(MCRouter, getResultName);

    std::string opname("mc_op_");
    for (int i = 0; i < mc_nops; ++i) {
      std::string name;
      name = opname + mc_op_to_string((mc_op_t)i);
      // mcrouter defines op names as foo-bar,
      // but PHP wants constants like foo_bar
      for (int j = opname.size(); j < name.size(); ++j) {
        if (name[j] == '-') {
          name[j] = '_';
        }
      }
      Native::registerClassConstant<KindOfInt64>(
        s_MCRouter.get(),
        makeStaticString(name),
        i);
    }
    for (size_t i = 0; i < static_cast<size_t>(compatibility::mc_nres); ++i) {
      Native::registerClassConstant<KindOfInt64>(
        s_MCRouter.get(),
        makeStaticString(
          compatibility::carbonResultToString(static_cast<carbon::Result>(i))),
        i);
    }
  }
} s_mcrouter_extension;

/////////////////////////////////////////////////////////////////////////////
} // namespace HPHP
