/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
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

#include "hphp/runtime/ext/async_mysql/ext_async_mysql.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <folly/Singleton.h>
#include <fmt/core.h>
#include <folly/ssl/OpenSSLCertUtils.h>
#include <squangle/mysql_client/AsyncHelpers.h>
#include <squangle/mysql_client/ClientPool.h>

#include "squangle/mysql_client/Connection.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/ext/mysql/ext_mysql.h"
#include "hphp/runtime/ext/mysql/mysql_common.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/logger.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

using AsyncMysqlClientPool = am::ClientPool<am::AsyncMysqlClient, am::AsyncMysqlClientFactory>;

namespace {
int HdfAsyncMysqlClientPoolSize = -1;

struct SQLQuery : SystemLib::ClassLoader<"HH\\Lib\\SQL\\Query"> {};

const Slot s_query_format_idx { 0 };
const Slot s_query_args_idx { 1 };


folly::Singleton<AsyncMysqlClientPool> clientPool([]() {
  if (HdfAsyncMysqlClientPoolSize == -1) {
    Logger::Error("AsyncMysql Config should have been initialized");
    HdfAsyncMysqlClientPoolSize = 2;
  }
  return new AsyncMysqlClientPool(
      std::make_unique<am::AsyncMysqlClientFactory>(),
      HdfAsyncMysqlClientPoolSize);
});

am::Query amquery_from_queryf(const StringData* pattern, const ArrayData* args);

am::QueryArgument queryarg_from_variant(const Variant& arg) {
  if (arg.isInteger()) {
    return arg.toInt64();
  }
  if (arg.isDouble()) {
    return arg.toDouble();
  }
  if (arg.isString()) {
    return static_cast<std::string>(arg.toString());
  }
  if (arg.isNull()) {
    return nullptr;
  }

  if (arg.isVec()) {
    const Array& vec = arg.asCArrRef();
    std::vector<am::QueryArgument> elems;
    elems.reserve(vec.size());
    for (ArrayIter listIter(vec); listIter; ++listIter) {
      const Variant& item = listIter.second();
      elems.push_back(queryarg_from_variant(item));
    }
    return elems;
  }

  if (arg.isObject()) {
    const Object& obj = arg.asCObjRef();

    auto queryCls = SQLQuery::classof();
    if (obj->getVMClass() == queryCls) {
      const auto format =
        val(obj->propRvalAtOffset(s_query_format_idx).tv()).pstr;
      const auto args = val(obj->propRvalAtOffset(s_query_args_idx).tv()).parr;
      return amquery_from_queryf(format, args);
    }

    if (obj->isCollection() && isVectorCollection(obj->collectionType())) {
      std::vector<am::QueryArgument> elems;
      elems.reserve(collections::getSize(obj.get()));
      for (ArrayIter listIter(arg); listIter; ++listIter) {
        const Variant& item = listIter.second();
        elems.push_back(queryarg_from_variant(item));
      }
      return elems;
    }
  }

  SystemLib::throwInvalidArgumentExceptionObject(
    fmt::format("Unable to serialize type '{}' for SQL",
                   getDataTypeString(arg.getType()).c_str())
  );
}

am::Query amquery_from_queryf(const StringData* pattern,
                              const ArrayData* args) {
  // Not directly calling argsv.toFollyDynamic() as that creates a folly
  // dynamic object, not list
  std::vector<am::QueryArgument> query_args;
  query_args.reserve(args->size());

  for (ArrayIter iter(args); iter; ++iter) {
    const Variant& arg = iter.second();
    query_args.push_back(queryarg_from_variant(arg));
  }

  return am::Query(pattern->toCppString(), query_args);
}

AsyncMysqlConnectionOptions* getConnectionOptions(const Object& opts) {
  if (!opts.instanceof(AsyncMysqlConnectionOptions::className())) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format(
        "Invalid argument. Expected {}, received {}",
        AsyncMysqlConnectionOptions::className().c_str(),
        opts->getClassName().c_str()
      )
    );
  }
  return Native::data<AsyncMysqlConnectionOptions>(opts);
}

MySSLContextProvider* getSSLContextProvider(const Object& ctx) {
  if (!ctx.instanceof(MySSLContextProvider::className())) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format(
        "Invalid argument. Expected {}, received {}",
        MySSLContextProvider::className().c_str(),
        ctx->getClassName().c_str()
      )
    );
  }
  return Native::data<MySSLContextProvider>(ctx);
}

}

static std::shared_ptr<am::AsyncMysqlClient> getClient() {
  return folly::Singleton<AsyncMysqlClientPool>::try_get()->getClient();
}

static std::vector<std::string> certLoggingImpl(
    X509* cert,
    const std::vector<std::string>& extNames,
    const am::Operation& op,
    bool validated) {
  // Capture the certificare Common Name
  std::string cn =
    folly::ssl::OpenSSLCertUtils::getCommonName(*cert).value_or("none");
  // Capture cert extension values for the extensions requested by the
  // callback installer
  std::vector<std::string> extValues;
  std::vector<std::pair<std::string, std::string>> allExtensions =
      folly::ssl::OpenSSLCertUtils::getAllExtensions(*cert);
  for (const auto& extName: extNames) {
    const auto& i =
      std::find_if(allExtensions.begin(), allExtensions.end(),
                   [&extName]
                   (const std::pair<std::string, std::string>& element) {
                     return element.first == extName;
                   });
    if (i != allExtensions.end() && !i->second.empty()) {
      extValues.push_back(i->second);
    }
  }

  // Capture the cert ASN values
  std::vector<std::string> subjectAltNames =
      folly::ssl::OpenSSLCertUtils::getSubjectAltNames(*cert);

  // Update the operation with the cert parameters
  op.reportServerCertContent(
      cn,
      subjectAltNames,
      extValues,
      validated);
  return extValues;
}

static bool certValidationImpl(
    const std::vector<std::string>& expectedValues,
    const std::vector<std::string>& certValues) {
  // Search for the expected extension values
  for (const auto& value: expectedValues) {
    if (std::find(certValues.begin(), certValues.end(), value) != certValues.end()) {
      return true;
    }
  }

  // No expected extension values found
  return false;
}

static bool serverCertLoggingCallback(
    X509* server_cert,
    const std::weak_ptr<am::Operation>& wptr,
    folly::StringPiece& /*errMsg*/,
    const std::vector<std::string>& extNames) {
  auto op = wptr.lock();
  if (!op) {
    return false;
  }

  // Log the server cert content
  certLoggingImpl(server_cert, extNames, *op, false);

  return true;
}

static bool serverCertValidationCallback(
    X509* server_cert,
    const std::weak_ptr<am::Operation>& wptr,
    folly::StringPiece& /* errMsg */,
    const std::vector<std::string>& extNames,
    const std::vector<std::string>& extValues) {
  auto op = wptr.lock();
  if (!op) {
    return false;
  }

  // Log the server cert content
  auto certValues = certLoggingImpl(server_cert, extNames, *op, true);
  return certValidationImpl(extValues, certValues);
}

static am::CertValidatorCallback generateCertValidationCallback(
    const std::string& serverCertExtNames,
    const std::string& extensionValues) {
  std::vector<std::string> extNames;
  folly::split(',', serverCertExtNames, extNames);
  if (extensionValues.empty()) {
    return [extNames = std::move(extNames)] (
          X509* server_cert,
          const std::weak_ptr<am::Operation>& wptr,
          folly::StringPiece& errMsg) {
      return serverCertLoggingCallback(server_cert, wptr, errMsg, extNames);
    };
  } else {
    std::vector<std::string> extValues;
    folly::split(',', extensionValues, extValues);
    return [extNames = std::move(extNames), extValues = std::move(extValues)] (
          X509* server_cert,
          const std::weak_ptr<am::Operation>& wptr,
          folly::StringPiece& errMsg) {
        return serverCertValidationCallback(
            server_cert, wptr, errMsg, extNames, extValues);
    };
  }
}
///////////////////////////////////////////////////////////////////////////
// AsyncMysqlClientStats

struct AsyncMysqlClientStats : SystemLib::ClassLoader<"AsyncMysqlClientStats"> {
  AsyncMysqlClientStats& operator=(const AsyncMysqlClientStats& other) {
    m_values = other.m_values;
    return *this;
  }

  static Object newInstance(db::ClientPerfStats values) {
    Object obj{classof()};
    auto* data = Native::data<AsyncMysqlClientStats>(obj);
    data->setPerfValues(std::move(values));
    return obj;
  }

  void setPerfValues(db::ClientPerfStats values) {
    m_values = std::move(values);
  }

  db::ClientPerfStats m_values;
};

static double HHVM_METHOD(AsyncMysqlClientStats, ioEventLoopMicrosAvg) {
  auto* data = Native::data<AsyncMysqlClientStats>(this_);
  return data->m_values.ioEventLoopMicrosAvg;
}

static double HHVM_METHOD(AsyncMysqlClientStats, callbackDelayMicrosAvg) {
  auto* data = Native::data<AsyncMysqlClientStats>(this_);
  return data->m_values.callbackDelayMicrosAvg;
}

static double HHVM_METHOD(AsyncMysqlClientStats, ioThreadBusyMicrosAvg) {
  auto* data = Native::data<AsyncMysqlClientStats>(this_);
  return data->m_values.ioThreadBusyTime;
}

static double HHVM_METHOD(AsyncMysqlClientStats, ioThreadIdleMicrosAvg) {
  auto* data = Native::data<AsyncMysqlClientStats>(this_);
  return data->m_values.ioThreadIdleTime;
}

static int64_t HHVM_METHOD(AsyncMysqlClientStats, notificationQueueSize) {
  auto* data = Native::data<AsyncMysqlClientStats>(this_);
  return data->m_values.notificationQueueSize;
}

static OptString HHLibSQLQuery__toString__FOR_DEBUGGING_ONLY(
  ObjectData* this_,
  const Object& conn) {

  const auto format =
    val(this_->propRvalAtOffset(s_query_format_idx).tv()).pstr;
  const auto args = val(this_->propRvalAtOffset(s_query_args_idx).tv()).parr;
  const auto query = amquery_from_queryf(format, args);

  auto* data = Native::data<AsyncMysqlConnection>(conn);
  data->verifyValidConnection();

  auto mysql = data->m_conn->mysql_for_testing_only();
  const auto str = query.render(mysql);
  return OptString(str.data(), str.length(), CopyString);
}

static OptString HHLibSQLQuery__toUnescapedString__FOR_DEBUGGING_ONLY__UNSAFE(
  ObjectData* this_) {
  const auto format =
    val(this_->propRvalAtOffset(s_query_format_idx).tv()).pstr;
  const auto args = val(this_->propRvalAtOffset(s_query_args_idx).tv()).parr;
  const auto query = amquery_from_queryf(format, args);
  const auto str = query.renderInsecure();
  return OptString(str.data(), str.length(), CopyString);
}

//////////////////////////////////////////////////////////////////////////////
// MySSLContextProvider
MySSLContextProvider::MySSLContextProvider(
    std::shared_ptr<am::SSLOptionsProviderBase> provider)
    : m_provider(provider) {}

Object MySSLContextProvider::newInstance(
    std::shared_ptr<am::SSLOptionsProviderBase> ssl_provider) {
  Object obj{classof()};
  Native::data<MySSLContextProvider>(obj)->setSSLProvider(
      std::move(ssl_provider));
  return obj;
}

std::shared_ptr<am::SSLOptionsProviderBase>
MySSLContextProvider::getSSLProvider() {
  return m_provider;
}
void MySSLContextProvider::setSSLProvider(
    std::shared_ptr<am::SSLOptionsProviderBase> ssl_provider) {
  m_provider = std::move(ssl_provider);
}

static bool HHVM_METHOD(MySSLContextProvider, isValid) {
  auto* data = Native::data<MySSLContextProvider>(this_);
  return data->m_provider != nullptr;
}

static void HHVM_METHOD(MySSLContextProvider, allowSessionResumption, bool allow) {
  auto* data = Native::data<MySSLContextProvider>(this_);
  if (data->m_provider) {
    data->m_provider->allowSessionResumption(allow);
  }
}

//////////////////////////////////////////////////////////////////////////////
// AsyncMysqlConnectionOptions

AsyncMysqlConnectionOptions::AsyncMysqlConnectionOptions() {
  // set default timeout
  auto default_timeout = mysqlExtension::ConnectTimeout * 1000;
  m_conn_opts.setTotalTimeout(am::Duration(default_timeout));
}

const am::ConnectionOptions&
AsyncMysqlConnectionOptions::getConnectionOptions() {
  return m_conn_opts;
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setConnectTimeout, int64_t timeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setTimeout(am::Duration(timeout));
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setConnectTcpTimeout, int64_t timeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setConnectTcpTimeout(am::Duration(timeout));
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setConnectAttempts, int64_t attempts) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setConnectAttempts(attempts);
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setTotalTimeout, int64_t timeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setTotalTimeout(am::Duration(timeout));
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setQueryTimeout, int64_t timeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setQueryTimeout(am::Duration(timeout));
}

static void HHVM_METHOD(
    AsyncMysqlConnectionOptions,
    setConnectionAttributes,
    const Array& attrs) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);

  IterateKV(attrs.get(), [&](TypedValue k, TypedValue v) {
    data->m_conn_opts.setAttribute(
      tvCastToString(k).toCppString(),
      tvCastToString(v).toCppString()
    );
  });
}

static void HHVM_METHOD(
    AsyncMysqlConnectionOptions,
    setSSLOptionsProvider,
    const Variant& sslContextProvider /* = null */) {
  if (sslContextProvider.isNull()) {
    return;
  }
  if (!sslContextProvider.isObject() ||
          !sslContextProvider.toObject()->instanceof
      (MySSLContextProvider::classof())) {
    SystemLib::throwInvalidArgumentExceptionObject(
        "Wrong type: expected MySSLContextProvider argument");
    return;
  }
  auto* sslProvider =
      Native::data<MySSLContextProvider>(sslContextProvider.toObject());
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setSSLOptionsProvider(sslProvider->getSSLProvider());
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, setSniServerName, const OptString& sniServername) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.setSniServerName(static_cast<std::string>(sniServername));
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, enableResetConnBeforeClose) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.enableResetConnBeforeClose();
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, enableDelayedResetConn) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.enableDelayedResetConn();
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions, enableChangeUser) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  data->m_conn_opts.enableChangeUser();
}

static void
HHVM_METHOD(AsyncMysqlConnectionOptions,
            setServerCertValidation,
            const OptString& serverCertExtNames /* = "" */,
            const OptString& extensionValues /* = "" */) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  // #ifdef HHVM_FACEBOOK until Open Source squangle pin is updated - needed as of
  // Squangle 2020-10-21
  #ifdef HHVM_FACEBOOK
  data->m_conn_opts.setCertValidationCallback(
      generateCertValidationCallback(
          std::string(serverCertExtNames), std::string(extensionValues)));
  #endif
}

static int64_t
HHVM_METHOD(AsyncMysqlConnectionOptions, getConnectTimeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  return data->m_conn_opts.getTimeout().count();
}

static int64_t
HHVM_METHOD(AsyncMysqlConnectionOptions, getConnectTcpTimeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  auto timeout = data->m_conn_opts.getConnectTcpTimeout();
  return timeout.has_value() ? timeout.value().count() : -1;
}

static int64_t
HHVM_METHOD(AsyncMysqlConnectionOptions, getConnectAttempts) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  return data->m_conn_opts.getConnectAttempts();
}

static int64_t
HHVM_METHOD(AsyncMysqlConnectionOptions, getTotalTimeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  return data->m_conn_opts.getTotalTimeout().count();
}

static int64_t
HHVM_METHOD(AsyncMysqlConnectionOptions, getQueryTimeout) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  return data->m_conn_opts.getQueryTimeout().count();
}

static Array
HHVM_METHOD(AsyncMysqlConnectionOptions, getConnectionAttributes) {
  auto* data = Native::data<AsyncMysqlConnectionOptions>(this_);
  const auto& attrs = data->m_conn_opts.getAttributes();
  DictInit result(attrs.size());
  for (const auto& [key, value] : attrs) {
    result.set(OptString(key), OptString(value));
  }
  return result.toArray();
}

static int64_t getQueryTimeout(int64_t timeout_micros) {
  if (timeout_micros < 0) {
    return mysqlExtension::ReadTimeout * 1000;
  } else {
    return timeout_micros;
  }
}

static std::vector<am::Query> transformQueries(const Array& queries) {
  std::vector<am::Query> queries_vec;
  queries_vec.reserve(queries.size());
  for (ArrayIter iter(queries); iter; ++iter) {
    queries_vec.emplace_back(am::Query::unsafe(
        static_cast<std::string>(iter.second().toString().data())));
  }
  return queries_vec;
}

//////////////////////////////////////////////////////////////////////////////
// AsyncMysqlClient
void HHVM_STATIC_METHOD(
    AsyncMysqlClient,
    setPoolsConnectionLimit,
    int64_t limit) {
  getClient()->setPoolsConnectionLimit(limit);
}

static AsyncMysqlConnection::AttributeMap transformAttributes(
    const Array& attributes) {
  AsyncMysqlConnection::AttributeMap cppAttributes;
  IterateKV(attributes.get(), [&](TypedValue k, TypedValue v) {
    cppAttributes[tvCastToString(k).toCppString()] =
        tvCastToString(v).toCppString();
  });
  return cppAttributes;
}

static Object newAsyncMysqlConnectEvent(
    std::shared_ptr<am::ConnectOperation> op,
    std::shared_ptr<am::AsyncMysqlClient> clientPtr) {
  // Set connection context to store the cert parameters
  if (op->getCertValidationCallback() &&
      op->getConnectionContext() == nullptr) {
    auto context = std::make_shared<db::ConnectionContextBase>();
    op->setConnectionContext(std::move(context));
  }
  auto event = new AsyncMysqlConnectEvent(op);
  try {
    op->setCallback([event, clientPtr](am::ConnectOperation& /* op */) {
      // Get current stats
      event->setClientStats(clientPtr->collectPerfStats());

      event->opFinished();
    });
    op->run();

    return Object{event->getWaitHandle()};
  } catch (...) {
    assertx(false);
    event->abandon();
    return Object{};
  }
}

static Object newAsyncMysqlConnectAndQueryEvent(
    std::shared_ptr<am::ConnectOperation> connectOp,
    std::shared_ptr<am::AsyncMysqlClient> clientPtr,
    const Array& queries,
    const Array& queryAttributes,
    bool connReusable) {

  if (connectOp->getCertValidationCallback() &&
      connectOp->getConnectionContext() == nullptr) {
    auto context = std::make_shared<db::ConnectionContextBase>();
    connectOp->setConnectionContext(std::move(context));
  }
  auto event = new AsyncMysqlConnectAndMultiQueryEvent(connectOp);
  auto transformedQueries = transformQueries(queries);
  auto transformedAttributes = transformAttributes(queryAttributes);
  try {
    connectOp->setCallback(
            [clientPtr,
            connReusable,
            event,
            transformedQueries,
            transformedAttributes]
        (am::ConnectOperation& op) mutable {

        if (!op.ok()) {
          // early exit must collect stats
          event->setClientStats(clientPtr->collectPerfStats());
          event->opFinished();
          return;
        }

        auto query_op = am::Connection::beginMultiQuery(
          op.releaseConnection(), std::move(transformedQueries));
        query_op->setAttributes(transformedAttributes);
        event->setQueryOp(query_op);

        try {
          am::MultiQueryAppenderCallback appender_callback = [event, clientPtr, connReusable](
            am::MultiQueryOperation& op,
            std::vector<am::QueryResult> query_results,
            am::QueryCallbackReason reason) {
            DCHECK(reason != am::QueryCallbackReason::RowsFetched);
            DCHECK(reason != am::QueryCallbackReason::QueryBoundary);
            if (!op.done()) {
              Logger::Error("Invalid state! Callback called as finished "
                            "but operation didn't finish");
            }
            op.setQueryResults(std::move(query_results));
            event->setClientStats(clientPtr->collectPerfStats());
            if (connReusable) {
              auto conn = op.releaseConnection();
              conn->setReusable(true);
            }
            event->opFinished();
          };
          query_op->setCallback(am::resultAppender(std::move(appender_callback)));
          query_op->run();
        } catch (...) {
          Logger::Error("Unexpected exception while executing Query");
          // If this assertion doesn't always hold, then we can add an m_error
          // flag to AsyncMysqlConnectAndMultiQueryEvent and set it here. But
          // my guess is that if query_op throws during its execution, then it
          // will have a non-ok result code.
          always_assert(!query_op->ok());
          event->opFinished();
        }
    });
    connectOp->run();
    return Object{event->getWaitHandle()};
  } catch (...) {
    Logger::Error("Unexpected exception while creating Connection");
    event->abandon();
    return Object{};
  }
}

Object HHVM_STATIC_METHOD(
    AsyncMysqlClient,
    connect,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    int64_t timeout_micros /* = -1 */,
    const Variant& sslContextProvider /* = null */,
    int64_t tcp_timeout_micros /* = 0 */,
    const OptString& sni_server_name /* = "" */,
    const OptString& serverCertExtNames /* = "" */,
    const OptString& serverCertExtValues /* = "" */) {
  auto key = std::make_shared<const am::MysqlConnectionKey>(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password));
  auto op = getClient()->beginConnection(std::move(key));
  if (!sslContextProvider.isNull()) {
    auto* mysslContextProvider = getSSLContextProvider(
            sslContextProvider.toObject());
    op->setSSLOptionsProvider(mysslContextProvider->getSSLProvider());
  }

  if (timeout_micros < 0) {
    timeout_micros = mysqlExtension::ConnectTimeout * 1000;
  }
  if (timeout_micros > 0) {
    op->setTimeout(am::Duration(timeout_micros));
  }
  // If tcp_timeout_micros is <= 0, skip setting the timeout
  if (tcp_timeout_micros > 0) {
    op->setTcpTimeout(am::Duration(tcp_timeout_micros));
  }
  // If ssl sni name is not empty, set it
  if (!sni_server_name.empty()) {
    op->setSniServerName(static_cast<std::string>(sni_server_name));
  }
  if (!serverCertExtNames.empty()) {
    op->setCertValidationCallback(
        generateCertValidationCallback(
            std::string(serverCertExtNames), std::string(serverCertExtValues)));
  }

  return newAsyncMysqlConnectEvent(std::move(op), getClient());
}

Object HHVM_STATIC_METHOD(
    AsyncMysqlClient,
    connectWithOpts,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    const Object& asyncMysqlConnOpts) {
  auto key = std::make_shared<const am::MysqlConnectionKey>(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password));
  auto op = getClient()->beginConnection(std::move(key));

  auto* obj = getConnectionOptions(asyncMysqlConnOpts);
  const auto& connOpts = obj->getConnectionOptions();
  op->setConnectionOptions(connOpts);

  return newAsyncMysqlConnectEvent(std::move(op), getClient());
}

Object HHVM_STATIC_METHOD(
    AsyncMysqlClient,
    connectAndQuery,
    const Array& queries,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    const Object& asyncMysqlConnOpts,
    const Array& queryAttributes) {

  auto key = std::make_shared<const am::MysqlConnectionKey>(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password));
  auto clientPtr = getClient();
  auto connectOp = clientPtr->beginConnection(key);
  auto* obj = getConnectionOptions(asyncMysqlConnOpts);
  const auto& connOpts = obj->getConnectionOptions();
  connectOp->setConnectionOptions(connOpts);

  return newAsyncMysqlConnectAndQueryEvent(
    std::move(connectOp),
    clientPtr,
    queries,
    queryAttributes,
    false /* connReusable*/);

}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnectionPool

// `connection_limit` - Defines the limit of opened connections for each set of
// User, Database, Host, etc
// `total_connection_limit` - Defines the total limit of opened connection as a
// whole
// `idle_timeout_micros` - Sets the maximum idle time in microseconds a
// connection can be left in the pool without being killed by the pool
// `age_timeout_micros` - Sets the maximum age (means the time since started) of
// a connection, the pool will then kill this connection when reaches that limit
// `expiration_policy` - We offer 2 policies for the expiration of a
// connection: `IdleTime` and `Age`, in the Idle policy a connection will only
// die after some time being idle; in Age policy we extend the idle one to kill
// also by age
// `validate_on_checkout` - Discard a cached connection at checkout when it is
// past its expiry or has pending input (a close or unrequested data). On by
// default; pass false to disable
//
const StaticString s_per_key_connection_limit("per_key_connection_limit"),
    s_pool_connection_limit("pool_connection_limit"),
    s_idle_timeout_micros("idle_timeout_micros"),
    s_age_timeout_micros("age_timeout_micros"),
    s_expiration_policy("expiration_policy"),
    s_validate_on_checkout("validate_on_checkout");

static void
HHVM_METHOD(AsyncMysqlConnectionPool, __construct, const Array& options) {
  auto* data = Native::data<AsyncMysqlConnectionPool>(this_);
  am::PoolOptions pool_options;
  if (options.exists(s_per_key_connection_limit)) {
    pool_options.setPerKeyLimit((int)options[s_per_key_connection_limit].toInt64());
  }
  if (options.exists(s_pool_connection_limit)) {
    pool_options.setPoolLimit((int)options[s_pool_connection_limit].toInt64());
  }
  if (options.exists(s_idle_timeout_micros)) {
    pool_options.setIdleTimeout(
        am::Duration(options[s_idle_timeout_micros].toInt64()));
  }
  if (options.exists(s_age_timeout_micros)) {
    pool_options.setAgeTimeout(
        am::Duration(options[s_age_timeout_micros].toInt64()));
  }
  if (options.exists(s_expiration_policy)) {
    static StaticString s_IdleTime{"IdleTime"};
    pool_options.setExpPolicy(options[s_expiration_policy].toString() ==
                                      s_IdleTime
                                  ? am::ExpirationPolicy::IdleTime
                                  : am::ExpirationPolicy::Age);
  }
  if (options.exists(s_validate_on_checkout)) {
    pool_options.setValidateOnCheckout(
        options[s_validate_on_checkout].toBoolean());
  }
  data->m_async_pool =
      am::AsyncConnectionPool::makePool(getClient(), pool_options);
}

void AsyncMysqlConnectionPool::sweep() {
  if (m_async_pool) {
    m_async_pool->shutdown();
    m_async_pool.reset();
  }
}

// `created_pool_connections` - Number of connections created by the pool
// `destroyed_pool_connections` - Number of connections destroyed by the pool,
//  be careful with this number, it will only be equal to the above when all
//  created connections have been close. This may not be true by the end of
//  a request.
// `connections_requested` - This number helps with comparing how many
//  connection would have been made if the there were no pooling.
// `pool_hits` - Counts the number of times a request for connection went to
//  the pool and it had a connection ready in cache
// `pool_misses` - Counts the number of times a we needed a connection and
//  none was ready to return
// `validation_discards` - Cached connections discarded at checkout because
//  `validate_on_checkout` found them expired or unusable
const StaticString s_created_pool_connections("created_pool_connections"),
    s_destroyed_pool_connections("destroyed_pool_connections"),
    s_connections_requested("connections_requested"), s_pool_hits("pool_hits"),
    s_pool_misses("pool_misses"),
    s_validation_discards("validation_discards");

static Array HHVM_METHOD(AsyncMysqlConnectionPool, getPoolStats) {
  auto* data = Native::data<AsyncMysqlConnectionPool>(this_);
  auto* pool_stats = data->m_async_pool->stats();
  Array ret = make_dict_array(
      s_created_pool_connections,
      pool_stats->numCreatedPoolConnections(),
      s_destroyed_pool_connections,
      pool_stats->numDestroyedPoolConnections(),
      s_connections_requested,
      pool_stats->numConnectionsRequested(),
      s_pool_hits,
      pool_stats->numPoolHits(),
      s_pool_misses,
      pool_stats->numPoolMisses(),
      s_validation_discards,
      pool_stats->numValidationDiscards());
  return ret;
}

static Object HHVM_METHOD(
    AsyncMysqlConnectionPool,
    connect,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    int64_t timeout_micros,
    const OptString& extra_key,
    const Variant& sslContextProvider,
    int64_t tcp_timeout_micros,
    const OptString& sni_server_name,
    const OptString& serverCertExtNames,
    const OptString& serverCertExtValues) {
  auto* data = Native::data<AsyncMysqlConnectionPool>(this_);
  auto op = data->m_async_pool->beginConnection(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password),
      static_cast<std::string>(extra_key));
  if (!sslContextProvider.isNull()) {
    auto* mysslContextProvider = getSSLContextProvider(
            sslContextProvider.toObject());
    op->setSSLOptionsProvider(mysslContextProvider->getSSLProvider());
  }
  if (timeout_micros < 0) {
    timeout_micros = mysqlExtension::ConnectTimeout * 1000;
  }
  if (timeout_micros > 0) {
    op->setTimeout(am::Duration(timeout_micros));
  }
  // If tcp_timeout_micros is <= 0, skip setting the timeout
  if (tcp_timeout_micros > 0) {
    op->setTcpTimeout(am::Duration(tcp_timeout_micros));
  }
  // If ssl sni name is not empty, set it
  if (!sni_server_name.empty()) {
    op->setSniServerName(static_cast<std::string>(sni_server_name));
  }
  if (!serverCertExtNames.empty()) {
    op->setCertValidationCallback(
        generateCertValidationCallback(
            std::string(serverCertExtNames), std::string(serverCertExtValues)));
  }

  return newAsyncMysqlConnectEvent(
      std::move(op), data->m_async_pool->getMysqlClient());
}

static Object HHVM_METHOD(
    AsyncMysqlConnectionPool,
    connectWithOpts,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    const Object& asyncMysqlConnOpts,
    const OptString& extra_key) {
  auto* data = Native::data<AsyncMysqlConnectionPool>(this_);
  auto op = data->m_async_pool->beginConnection(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password),
      static_cast<std::string>(extra_key));

  auto* obj = getConnectionOptions(asyncMysqlConnOpts);
  const auto& connOpts = obj->getConnectionOptions();
  op->setConnectionOptions(connOpts);
  if (connOpts.isEnableResetConnBeforeClose()) {
    op->enableResetConnBeforeClose();
  }

  return newAsyncMysqlConnectEvent(
      std::move(op), data->m_async_pool->getMysqlClient());
}

static Object HHVM_METHOD(
    AsyncMysqlConnectionPool,
    connectAndQuery,
    const Array& queries,
    const OptString& host,
    int64_t port,
    const OptString& dbname,
    const OptString& user,
    const OptString& password,
    const Object& asyncMysqlConnOpts,
    const OptString& extra_key,
    const Array& queryAttributes) {
  auto* data = Native::data<AsyncMysqlConnectionPool>(this_);

  auto connectOp = data->m_async_pool->beginConnection(
      static_cast<std::string>(host),
      port,
      static_cast<std::string>(dbname),
      static_cast<std::string>(user),
      static_cast<std::string>(password),
      static_cast<std::string>(extra_key));

  auto* obj = getConnectionOptions(asyncMysqlConnOpts);
  const auto& connOpts = obj->getConnectionOptions();
  connectOp->setConnectionOptions(connOpts);
  if (connOpts.isEnableResetConnBeforeClose()) {
    connectOp->enableResetConnBeforeClose();
  }
  return newAsyncMysqlConnectAndQueryEvent(
    std::move(connectOp),
    data->m_async_pool->getMysqlClient(),
    queries,
    queryAttributes,
    true /* connReusable*/);
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnection

Object AsyncMysqlConnection::newInstance(
    std::unique_ptr<am::Connection> conn,
    std::shared_ptr<am::ConnectOperation> conn_op,
    db::ClientPerfStats clientStats) {
  Object ret{classof()};
  auto* retPtr = Native::data<AsyncMysqlConnection>(ret);
  retPtr->setConnection(std::move(conn));
  retPtr->setConnectOperation(std::move(conn_op));
  retPtr->setClientStats(std::move(clientStats));
  return ret;
}

AsyncMysqlConnection::AsyncMysqlConnection() : m_port(0), m_closed(false) {}

void AsyncMysqlConnection::sweep() {
  m_conn.reset();
}

void AsyncMysqlConnection::setConnection(std::unique_ptr<am::Connection> conn) {
  m_conn = std::move(conn);
  m_host = OptString(m_conn->host(), CopyString);
  m_port = m_conn->port();
}

void AsyncMysqlConnection::setConnectOperation(
    std::shared_ptr<am::ConnectOperation> op) {
  m_op = std::move(op);
}

void AsyncMysqlConnection::setClientStats(db::ClientPerfStats clientStats) {
  m_clientStats = std::move(clientStats);
}

void AsyncMysqlConnection::verifyValidConnection() {
  if (UNLIKELY(!m_conn || !m_conn->ok())) {
    if (m_closed) {
      SystemLib::throwInvalidArgumentExceptionObject(
        "attempt to invoke method on a closed connection");
    } else if (m_conn && !m_conn->ok()) {
      SystemLib::throwInvalidArgumentExceptionObject(
        "attempt to invoke method on an invalid connection");
    } else {
      SystemLib::throwInvalidArgumentExceptionObject(
        "attempt to invoke method on a busy connection");
    }
  }
}

bool AsyncMysqlConnection::isValidConnection() {
  // When a query timeout happens, the connection is invalid and SQuangLe
  // layer closes it for precaution.
  return m_conn && m_conn->ok() && !m_closed;
}

Object AsyncMysqlConnection::query(
    ObjectData* this_,
    const am::Query& query,
    int64_t timeout_micros /* = -1 */,
    const AttributeMap& queryAttributes /*  = AttributeMap() */) {
  verifyValidConnection();
  auto* clientPtr = static_cast<am::AsyncMysqlClient*>(&m_conn->client());
  auto op = am::Connection::beginQuery(std::move(m_conn), query);

  op->setAttributes(queryAttributes);
  op->setTimeout(am::Duration(getQueryTimeout(timeout_micros)));

  auto event = new AsyncMysqlQueryEvent(this_, op);
  try {
    am::QueryAppenderCallback appender_callback = [event, clientPtr](
        am::QueryOperation& op,
        am::QueryResult query_result,
        am::QueryCallbackReason reason) {
      DCHECK(reason != am::QueryCallbackReason::RowsFetched);
      if (!op.done()) {
        Logger::Error("Invalid state! Callback called as finished "
                      "but operation didn't finish");
      }

      op.setQueryResult(std::move(query_result));
      event->setClientStats(clientPtr->collectPerfStats());
      event->opFinished();
    };
    op->setCallback(am::resultAppender(std::move(appender_callback)));
    op->run();

    return Object{event->getWaitHandle()};
  }
  catch (...) {
    Logger::Error("Unexpected exception while beginning ConnectOperation");
    assertx(false);
    event->abandon();
    return Object{};
  }
}

static Object HHVM_METHOD(
    AsyncMysqlConnection,
    query,
    const OptString& query,
    int64_t timeout_micros /* = -1 */,
    const Array& queryAttributes) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  return data->query(
      this_,
      am::Query::unsafe(static_cast<std::string>(query)),
      timeout_micros,
      transformAttributes(queryAttributes));
}

static Object HHVM_METHOD(
    AsyncMysqlConnection,
    queryf,
    const OptString& pattern,
    const Array& args) {

  const auto query = amquery_from_queryf(pattern.get(), args.get());
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  return data->query(this_, query);
}

static Object HHVM_METHOD(
    AsyncMysqlConnection,
    queryAsync,
    const Object& query) {
  const auto format =
    val(query->propRvalAtOffset(s_query_format_idx).tv()).pstr;
  const auto args = val(query->propRvalAtOffset(s_query_args_idx).tv()).parr;
  return Native::data<AsyncMysqlConnection>(this_)->query(
    this_, amquery_from_queryf(format, args));
}

static Object HHVM_METHOD(
    AsyncMysqlConnection,
    multiQuery,
    const Array& queries,
    int64_t timeout_micros /* = -1 */,
    const Array& queryAttributes) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  data->verifyValidConnection();
  auto* clientPtr = static_cast<am::AsyncMysqlClient*>(&data->m_conn->client());
  auto op = am::Connection::beginMultiQuery(std::move(data->m_conn),
                                            transformQueries(queries));

  op->setAttributes(transformAttributes(queryAttributes));
  op->setTimeout(am::Duration(getQueryTimeout(timeout_micros)));

  auto event = new AsyncMysqlMultiQueryEvent(this_, op);
  try {
    am::MultiQueryAppenderCallback appender_callback = [event, clientPtr](
        am::MultiQueryOperation& op,
        std::vector<am::QueryResult> query_results,
        am::QueryCallbackReason reason) {
      DCHECK(reason != am::QueryCallbackReason::RowsFetched);
      DCHECK(reason != am::QueryCallbackReason::QueryBoundary);
      if (!op.done()) {
        Logger::Error("Invalid state! Callback called as finished "
                      "but operation didn't finish");
      }

      op.setQueryResults(std::move(query_results));
      event->setClientStats(clientPtr->collectPerfStats());
      event->opFinished();
    };
    op->setCallback(am::resultAppender(std::move(appender_callback)));
    op->run();

    return Object{event->getWaitHandle()};
  }
  catch (...) {
    assertx(false);
    event->abandon();
    return Object{};
  }
}

static bool HHVM_METHOD(AsyncMysqlConnection, isValid) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  return data->isValidConnection();
}

static OptString
HHVM_METHOD(AsyncMysqlConnection, escapeString, const OptString& input) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  data->verifyValidConnection();
  OptString ret = data->m_conn->escapeString(input.data());
  return ret;
}

static OptString HHVM_METHOD(AsyncMysqlConnection, serverInfo) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  OptString ret = "";
  if (data->isValidConnection()) {
    ret = data->m_conn->serverInfo();
  } else {
    Logger::Error("Accessing closed connection");
  }
  return ret;
}

static bool HHVM_METHOD(AsyncMysqlConnection, sslSessionReused) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  // Will throw PHP catchable Exception in case the connection isn't valid.
  data->verifyValidConnection();
  return data->m_conn->sslSessionReused();
}

static bool HHVM_METHOD(AsyncMysqlConnection, isSSL) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  // Will throw PHP catchable Exception in case the connection isn't valid.
  data->verifyValidConnection();
  return data->m_conn->isSSL();
}

static int64_t HHVM_METHOD(AsyncMysqlConnection, warningCount) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  int count = 0;
  if (data->isValidConnection()) {
    count = data->m_conn->warningCount();
  } else {
    Logger::Error("Accessing closed connection");
  }
  return count;
}

static OptString HHVM_METHOD(AsyncMysqlConnection, host) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  return data->m_host;
}

static int64_t HHVM_METHOD(AsyncMysqlConnection, port) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  return data->m_port;
}

static void HHVM_METHOD(AsyncMysqlConnection, setReusable, bool reusable) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  if (data->m_conn) {
    data->m_conn->setReusable(reusable);
  } else {
    Logger::Error("Accessing closed connection");
  }
}

static bool HHVM_METHOD(AsyncMysqlConnection, isReusable) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  if (data->m_conn) {
    return data->m_conn->isReusable();
  } else {
    Logger::Error("Accessing closed connection");
  }
  return false;
}

static Variant HHVM_METHOD(AsyncMysqlConnection, connectResult) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  if (data->m_op) {
    return AsyncMysqlConnectResult::newInstance(data->m_op,
                                                data->m_clientStats);
  } else {
    Logger::Error("ConnectResult only available when Connection created by "
                  "AsyncMysqlClient");
  }
  return false;
}

static double HHVM_METHOD(AsyncMysqlConnection, lastActivityTime) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  if (data->m_conn) {
    auto d = std::chrono::duration_cast<std::chrono::microseconds>(
        data->m_op->startTime().time_since_epoch());
    return d.count() / 1000.0 / 1000.0;
  } else {
    Logger::Error("Accessing closed connection");
  }
  return false;
}

static void HHVM_METHOD(AsyncMysqlConnection, close) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);

  data->m_conn.reset();
  data->m_closed = true;
}

static OptString HHVM_METHOD(AsyncMysqlConnection, getSslCertCn) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  data->verifyValidConnection();

  const auto* context = data->m_conn->getConnectionContext();
  if (context && context->sslCertCn.hasValue()) {
    return context->sslCertCn.value();
  } else {
    return OptString();
  }
}

static Object HHVM_METHOD(AsyncMysqlConnection, getSslCertSan) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  data->verifyValidConnection();

  auto ret = req::make<c_Vector>();
  const auto* context = data->m_conn->getConnectionContext();
  if (context && context->sslCertSan.hasValue()) {
    for (const auto& san: context->sslCertSan.value()) {
      ret->add(Variant(san));
    }
  }
  return Object(std::move(ret));
}

static Object HHVM_METHOD(AsyncMysqlConnection, getSslCertExtensions) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  data->verifyValidConnection();

  auto ret = req::make<c_Vector>();
  const auto* context = data->m_conn->getConnectionContext();
  if (context && context->sslCertIdentities.hasValue()) {
    for (const auto& id: context->sslCertIdentities.value()) {
      ret->add(Variant(id));
    }
  }
  return Object(std::move(ret));
}

static bool HHVM_METHOD(AsyncMysqlConnection, isSslCertValidationEnforced) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  data->verifyValidConnection();

  const auto* context = data->m_conn->getConnectionContext();
  return context && context->isServerCertValidated;
}

static OptString HHVM_METHOD(AsyncMysqlConnection, getSslVersion) {
  auto* data = Native::data<AsyncMysqlConnection>(this_);
  if (data->m_conn) {
    return data->m_conn->getTlsVersion();
  }
  return OptString();
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlResult

int64_t AsyncMysqlResult::elapsedMicros() {
  return op()->elapsed().count();
}

double AsyncMysqlResult::startTime() {
  am::Duration d = std::chrono::duration_cast<std::chrono::microseconds>(
      op()->startTime().time_since_epoch());
  return d.count() / 1000.0 / 1000.0;
}

double AsyncMysqlResult::endTime() {
  am::Duration d = std::chrono::duration_cast<std::chrono::microseconds>(
      op()->endTime().time_since_epoch());
  return d.count() / 1000.0 / 1000.0;
}

am::Operation* AsyncMysqlResult::op() {
  if (m_op.get() == nullptr) {
    // m_op is null if this object is directly created. It is possible if
    // a derived class is defined that does not call this class' constructor.
    SystemLib::throwInvalidOperationExceptionObject(
        "AsyncMysqlErrorResult object is not properly initialized.");
  }
  return m_op.get();
}

Object AsyncMysqlResult::clientStats() {
  return AsyncMysqlClientStats::newInstance(m_clientStats);
}

//
// Depending on the scenario finding the link to connection context
// associated with the operation can be tricky. If the operation
// completed successfully and there is a valid connection associated
// with the operation, then we should search for the connection context
// linked to the connection associated with the operation. In case of
// failed conneciton (failed ConnectOperation, failed ConnectPoolOperation)
// there is no connection associated with the operation at the end because
// we failed to connect. In this case checking for the connection context
// directly linked to the operation is our best bet.
//
static const db::ConnectionContextBase*
connectionContextFromOperation(const am::Operation* operation) {
  const db::ConnectionContextBase* context = nullptr;
  if (auto* connection = operation->connection()) {
    context = connection->getConnectionContext();
  }
  if (!context) {
    auto* connectOp = dynamic_cast<const am::ConnectOperation*>(operation);
    if (connectOp) {
      context = connectOp->getConnectionContext();
    }
  }
  return context;
}

static bool HHVM_METHOD(AsyncMysqlResult, sslSessionReused) {
  auto* data = Native::data<AsyncMysqlResult>(this_);

  if (auto* op = data->m_op.get()) {
    const auto* context = connectionContextFromOperation(op);
    return context && context->sslSessionReused;
  }
  return false;
}

static OptString HHVM_METHOD(AsyncMysqlResult, getSslCertCn) {
  auto* data = Native::data<AsyncMysqlResult>(this_);
  if (auto* op = data->m_op.get()) {
    const auto* context = connectionContextFromOperation(op);
    if (context && context->sslCertCn.hasValue()) {
      return context->sslCertCn.value();
    }
  }
  return OptString();
}

static Object HHVM_METHOD(AsyncMysqlResult, getSslCertSan) {
  auto* data = Native::data<AsyncMysqlResult>(this_);
  auto ret = req::make<c_Vector>();
  if (auto* op = data->m_op.get() ) {
    const auto* context = connectionContextFromOperation(op);
    if (context && context->sslCertSan.hasValue()) {
      for (const auto& san: context->sslCertSan.value()) {
        ret->add(Variant(san));
      }
    }
  }
  return Object(std::move(ret));
}

static Object HHVM_METHOD(AsyncMysqlResult, getSslCertExtensions) {
  auto* data = Native::data<AsyncMysqlResult>(this_);
  auto ret = req::make<c_Vector>();
  if (auto* op = data->m_op.get()) {
    const auto* context = connectionContextFromOperation(op);
    if (context && context->sslCertIdentities.hasValue()) {
      for (const auto& id: context->sslCertIdentities.value()) {
        ret->add(Variant(id));
      }
    }
  }
  return Object(std::move(ret));
}

static bool HHVM_METHOD(AsyncMysqlResult, isSslCertValidationEnforced) {
  auto* data = Native::data<AsyncMysqlResult>(this_);
  if (auto* op = data->m_op.get()) {
    const auto* context = connectionContextFromOperation(op);
    return context && context->isServerCertValidated;
  }
  return false;
}

static OptString HHVM_METHOD(AsyncMysqlResult, getSslVersion) {
  auto* data = Native::data<AsyncMysqlResult>(this_);
  if (auto* op = data->m_op.get()) {
    const auto* context = connectionContextFromOperation(op);
    if (context && !context->sslVersion.empty()) {
      return context->sslVersion;
    }
  }
  return OptString();
}

#define DEFINE_PROXY_METHOD(cls, method, type) \
  type HHVM_METHOD(cls, method) { return Native::data<cls>(this_)->method(); }

#define EXTENDS_ASYNC_MYSQL_RESULT(cls)            \
  DEFINE_PROXY_METHOD(cls, elapsedMicros, int64_t) \
  DEFINE_PROXY_METHOD(cls, startTime, double)      \
  DEFINE_PROXY_METHOD(cls, endTime, double)        \
  DEFINE_PROXY_METHOD(cls, clientStats, Object)

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnectResult

Object AsyncMysqlConnectResult::newInstance(std::shared_ptr<am::Operation> op,
                                            db::ClientPerfStats values) {
  Object ret{classof()};
  Native::data<AsyncMysqlConnectResult>(ret)
      ->create(std::move(op), std::move(values));
  return ret;
}

EXTENDS_ASYNC_MYSQL_RESULT(AsyncMysqlConnectResult)

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlErrorResult

Object AsyncMysqlErrorResult::newInstance(std::shared_ptr<am::Operation> op,
                                          db::ClientPerfStats values) {
  Object ret{classof()};
  Native::data<AsyncMysqlErrorResult>(ret)
      ->create(std::move(op), std::move(values));
  return ret;
}

EXTENDS_ASYNC_MYSQL_RESULT(AsyncMysqlErrorResult)

static int64_t HHVM_METHOD(AsyncMysqlErrorResult, mysql_errno) {
  auto* data = Native::data<AsyncMysqlErrorResult>(this_);
  return data->m_op->mysql_errno();
}

static OptString HHVM_METHOD(AsyncMysqlErrorResult, mysql_error) {
  auto* data = Native::data<AsyncMysqlErrorResult>(this_);
  return data->m_op->mysql_error();
}

static OptString HHVM_METHOD(AsyncMysqlErrorResult, failureType) {
  auto* data = Native::data<AsyncMysqlErrorResult>(this_);
  return data->m_op->resultString().toString();
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlQueryErrorResult

Object AsyncMysqlQueryErrorResult::newInstance(
    std::shared_ptr<am::Operation> op,
    db::ClientPerfStats values,
    req::ptr<c_Vector> results) {
  Object ret{classof()};
  Native::data<AsyncMysqlQueryErrorResult>(ret)
      ->create(std::move(op), std::move(values), results);
  return ret;
}

AsyncMysqlQueryErrorResult::AsyncMysqlQueryErrorResult() {
  static_assert(offsetof(AsyncMysqlQueryErrorResult, m_parent) +
    sizeof(AsyncMysqlErrorResult) == sizeof(AsyncMysqlQueryErrorResult),
    "m_parent must be the last member of AsyncMysqlQueryErrorResult");
}

void AsyncMysqlQueryErrorResult::sweep() {
  m_parent.sweep();
}

void AsyncMysqlQueryErrorResult::create(std::shared_ptr<am::Operation> op,
                                        db::ClientPerfStats stats,
                                        req::ptr<c_Vector> results) {
  m_parent.create(std::move(op), std::move(stats));
  m_query_results = results;
}

static int64_t HHVM_METHOD(AsyncMysqlQueryErrorResult, numSuccessfulQueries) {
  auto* data = Native::data<AsyncMysqlQueryErrorResult>(this_);
  return std::dynamic_pointer_cast<am::FetchOperation>(data->m_parent.m_op)
      ->numQueriesExecuted();
}

static Object HHVM_METHOD(AsyncMysqlQueryErrorResult, getSuccessfulResults) {
  auto* data = Native::data<AsyncMysqlQueryErrorResult>(this_);
  return Object(data->m_query_results);
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlQueryResult

void AsyncMysqlQueryResult::sweep() {
  m_op.reset();
  m_query_result.reset();
}

Object AsyncMysqlQueryResult::newInstance(std::shared_ptr<am::FetchOperation> op,
                                          db::ClientPerfStats stats,
                                          am::QueryResult query_result,
                                          bool noIndexUsed) {
  Object ret{ classof() };
  Native::data<AsyncMysqlQueryResult>(ret)->create(
    std::move(op), std::move(stats), std::move(query_result), noIndexUsed);
  return ret;
}

void AsyncMysqlQueryResult::create(std::shared_ptr<am::FetchOperation> op,
                                   db::ClientPerfStats stats,
                                   am::QueryResult query_result,
                                   bool noIndexUsed) {
  AsyncMysqlResult::create(std::move(op), std::move(stats));
  m_query_result = std::make_unique<am::QueryResult>(std::move(query_result));
  m_no_index_used = noIndexUsed;
  m_field_index = req::make_shared<FieldIndex>(m_query_result->getRowFields());
}

EXTENDS_ASYNC_MYSQL_RESULT(AsyncMysqlQueryResult)

static int64_t HHVM_METHOD(AsyncMysqlQueryResult, lastInsertId) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->m_query_result->lastInsertId();
}

static int64_t HHVM_METHOD(AsyncMysqlQueryResult, numRowsAffected) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->m_query_result->numRowsAffected();
}

static int64_t HHVM_METHOD(AsyncMysqlQueryResult, numRows) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->m_query_result->numRows();
}
static Object HHVM_METHOD(AsyncMysqlQueryResult, vectorRows) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->buildRows(false /* as_maps */, false /* typed */);
}

static int64_t HHVM_METHOD(AsyncMysqlQueryResult, resultSizeBytes) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  auto* fetchOp =  dynamic_cast<const am::FetchOperation*>(data->op());
  if(fetchOp) {
    return fetchOp->resultSize();
  }
  return -1;
}

static Object HHVM_METHOD(AsyncMysqlQueryResult, mapRows) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->buildRows(true /* as_maps */, false /* typed */);
}

static Object HHVM_METHOD(AsyncMysqlQueryResult, vectorRowsTyped) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->buildRows(false /* as_maps */, true /* typed */);
}

static Object HHVM_METHOD(AsyncMysqlQueryResult, mapRowsTyped) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->buildRows(true /* as_maps */, true /* typed */);
}

static Array HHVM_METHOD(AsyncMysqlQueryResult, dictRowsTyped) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->buildTypedVecMaps();
}

static Object HHVM_METHOD(AsyncMysqlQueryResult, rowBlocks) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  auto ret = req::make<c_Vector>();
  auto row_blocks = data->m_query_result->stealRows();
  ret->reserve(row_blocks.size());

  for (auto& row_block : row_blocks) {
    ret->add(AsyncMysqlRowBlock::newInstance(&row_block,
                                             data->m_field_index));
  }
  return Object{std::move(ret)};
}

static bool HHVM_METHOD(AsyncMysqlQueryResult, noIndexUsed) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->m_no_index_used;
}

static OptString HHVM_METHOD(AsyncMysqlQueryResult, recvGtid) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return OptString(data->m_query_result->recvGtid(), CopyString);
}

static Object HHVM_METHOD(AsyncMysqlQueryResult, responseAttributes) {
  auto ret = req::make<c_Map>();
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  const auto& responseAttributes = data->m_query_result->responseAttributes();
  for (const auto& [key, value] : responseAttributes) {
    ret->set(key, value);
  }
  return Object{std::move(ret)};
}

static OptString HHVM_METHOD(AsyncMysqlQueryResult, mysqlInfo) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  const auto& mysqlInfo = data->m_query_result->mysqlInfo();
  if (mysqlInfo) {
    return OptString(*mysqlInfo, CopyString);
  }
  return OptString{};
}

static Variant HHVM_METHOD(AsyncMysqlQueryResult, rowsMatched) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  const auto& rowsMatched = data->m_query_result->rowsMatched();
  if (rowsMatched) {
    return static_cast<int64_t>(*rowsMatched);
  }
  return init_null();
}

static int64_t HHVM_METHOD(AsyncMysqlQueryResult, warningsCount) {
  auto* data = Native::data<AsyncMysqlQueryResult>(this_);
  return data->m_query_result->warningsCount();
}

namespace {
Variant buildTypedValue(const am::RowFields* row_fields,
                        const am::Row& row,
                        int field_num,
                        bool typed_values) {
  if (row.isNull(field_num)) {
    return init_null();
  }

  // The underlying library may return zero length null ptr's to
  // indicate an empty string (since the isNull check above would tell
  // if it were actually NULL).
  OptString string_value =
      (row[field_num].data() == nullptr && row[field_num].size() == 0)
          ? String::Empty()
          : OptString(row[field_num].data(), row[field_num].size(), CopyString);

  if (!typed_values) {
    return string_value;
  }

  return mysql_makevalue(string_value, row_fields->getFieldType(field_num));
}
}

Object AsyncMysqlQueryResult::buildRows(bool as_maps, bool typed_values) {
  auto ret = req::make<c_Vector>();
  ret->reserve(m_query_result->numRows());
  for (const auto& row : *m_query_result) {
    if (as_maps) {
      auto row_map = req::make<c_Map>();
      for (int i = 0; i < row.size(); ++i) {
        row_map->set(
            m_field_index->getFieldString(i),
            buildTypedValue(
                m_query_result->getRowFields(), row, i, typed_values));
      }
      ret->add(Variant(std::move(row_map)));
    } else {
      auto row_vector = req::make<c_Vector>();
      row_vector->reserve(row.size());
      for (int i = 0; i < row.size(); ++i) {
        row_vector->add(buildTypedValue(
            m_query_result->getRowFields(), row, i, typed_values));
      }
      ret->add(Variant(std::move(row_vector)));
    }
  }
  return Object(std::move(ret));
}

Array AsyncMysqlQueryResult::buildTypedVecMaps() {
  VecInit ret{m_query_result->numRows()};
  for (const auto& row : *m_query_result) {
    DictInit row_map{row.size()};
    for (int i = 0; i < row.size(); ++i) {
      row_map.set(
          m_field_index->getFieldString(i),
          buildTypedValue(
              m_query_result->getRowFields(), row, i, true));
    }
    ret.append(row_map.toArray());
  }
  return ret.toArray();
}

FieldIndex::FieldIndex(const am::RowFields* row_fields) {
  if (row_fields == nullptr)
    return;
  auto n = row_fields->numFields();
  field_names_.reserve(n);
  field_name_map_.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto name = OptString(row_fields->fieldName(i).str());
    field_names_.push_back(name);
    field_name_map_[name] = i; // last duplicate field name wins
  }
}

size_t FieldIndex::getFieldIndex(const OptString& field_name) const {
  auto it = field_name_map_.find(field_name);
  if (it == field_name_map_.end()) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Given field name doesn't exist");
  }
  return it->second;
}

OptString FieldIndex::getFieldString(size_t field_index) const {
  // Leaving out of bounds to be thrown by the vector in case needed
  return field_names_.at(field_index);
}

namespace {

req::ptr<c_Vector> transformQueryResults(
  std::shared_ptr<am::MultiQueryOperation> op,
  db::ClientPerfStats stats) {
  auto results = req::make<c_Vector>();
  auto query_results = op->stealQueryResults();
  results->reserve(query_results.size());
  for (int i = 0; i < query_results.size(); ++i) {
    auto ret = AsyncMysqlQueryResult::newInstance(
        op, stats, std::move(query_results[i]), op->noIndexUsed());
    results->add(std::move(ret));
  }
  query_results.clear();
  return results;
}

struct AsyncMysqlQueryException: SystemLib::ClassLoader<"AsyncMysqlQueryException"> {};
struct AsyncMysqlConnectException: SystemLib::ClassLoader<"AsyncMysqlConnectException"> {};

void throwAsyncMysqlConnectException(std::shared_ptr<am::Operation> op,
                                     db::ClientPerfStats clientStats) {
  auto error =
      AsyncMysqlErrorResult::newInstance(op, std::move(clientStats));

  assertx(op->result() == am::OperationResult::Failed ||
         op->result() == am::OperationResult::TimedOut ||
         op->result() == am::OperationResult::Cancelled);

  throw_object(
    AsyncMysqlConnectException::classof(),
    make_vec_array(std::move(error)));
}

void throwAsyncMysqlQueryException(std::shared_ptr<am::Operation> op,
                                   db::ClientPerfStats clientStats,
                                   req::ptr<c_Vector> res) {
  auto error = AsyncMysqlQueryErrorResult::newInstance(
      op, std::move(clientStats), res);

  assertx(op->result() == am::OperationResult::Failed ||
         op->result() == am::OperationResult::TimedOut ||
         op->result() == am::OperationResult::Cancelled);

  throw_object(
    AsyncMysqlQueryException::classof(),
    make_vec_array(std::move(error)));
}

}

void AsyncMysqlConnectEvent::unserialize(TypedValue& result) {
  if (m_op->ok()) {
    auto ret = AsyncMysqlConnection::newInstance(
        m_op->releaseConnection(), m_op, std::move(m_clientStats));

    tvCopy(make_tv<KindOfObject>(ret.detach()), result);
  } else {
    throwAsyncMysqlConnectException(m_op, std::move(m_clientStats));
  }
}

void AsyncMysqlQueryEvent::unserialize(TypedValue& result) {
  // Retrieve the original conn and return the underlying connection
  // to it.
  assertx(getPrivData()->instanceof(AsyncMysqlConnection::classof()));
  auto* conn = Native::data<AsyncMysqlConnection>(getPrivData());
  conn->setConnection(m_query_op->releaseConnection());

  if (m_query_op->ok()) {
    auto query_result = m_query_op->stealQueryResult();
    auto ret = AsyncMysqlQueryResult::newInstance(
      m_query_op, std::move(m_clientStats), std::move(query_result),
      m_query_op->noIndexUsed());
    tvCopy(make_tv<KindOfObject>(ret.detach()), result);
  } else {
    throwAsyncMysqlQueryException(m_query_op,
                                  std::move(m_clientStats),
                                  req::make<c_Vector>());
  }
}

void AsyncMysqlMultiQueryEvent::unserialize(TypedValue& result) {
  // Same as unserialize from AsyncMysqlQueryEvent but the result is a
  // vector of query results
  assertx(getPrivData()->instanceof(AsyncMysqlConnection::classof()));
  auto* conn = Native::data<AsyncMysqlConnection>(getPrivData());
  conn->setConnection(m_multi_op->releaseConnection());


  // Retrieving the results for all executed queries
  auto results = transformQueryResults(m_multi_op, m_clientStats);

  if (m_multi_op->ok()) {
    tvDup(make_tv<KindOfObject>(results.get()), result);
  } else {
    throwAsyncMysqlQueryException(m_multi_op, std::move(m_clientStats), results);
  }
}

void AsyncMysqlConnectAndMultiQueryEvent::unserialize(TypedValue& result) {
  if (!m_connect_op->ok()) {
    throwAsyncMysqlConnectException(m_connect_op, std::move(m_clientStats));
  }
  // Retrieving the results for all executed queries
  auto queryResults = transformQueryResults(m_multi_query_op, m_clientStats);
  auto connResult = AsyncMysqlConnectResult::newInstance(
      m_connect_op, m_clientStats);
  if (m_multi_query_op->ok()) {
    auto resTuple = make_vec_array(connResult, queryResults);
    tvCopy(make_array_like_tv(resTuple.detach()), result);
  } else {
    throwAsyncMysqlQueryException(m_multi_query_op, std::move(m_clientStats), queryResults);
  }
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowBlock

Object AsyncMysqlRowBlock::newInstance(am::RowBlock* row_block,
    req::shared_ptr<FieldIndex> indexer) {
  Object ret{AsyncMysqlRowBlock::classof()};
  auto* data = Native::data<AsyncMysqlRowBlock>(ret);
  data->m_row_block.reset(new am::RowBlock(std::move(*row_block)));
  data->m_field_index = indexer;
  return ret;
}

void AsyncMysqlRowBlock::sweep() {
  m_row_block.reset();
}

size_t AsyncMysqlRowBlock::getIndexFromVariant(const Variant& field) {
  if (field.isInteger()) {
    return field.toInt64();
  } else if (field.isString()) {
    return m_field_index->getFieldIndex(field.toString());
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    "Only integer or string field names may be used with RowBlock");
}

// The String conversion allows `NULL` to ""
template <>
folly::StringPiece AsyncMysqlRowBlock::getFieldAs(int64_t row,
                                                  const Variant& field) {
  auto index = getIndexFromVariant(field);
  try {
    // Note that for String before you return to PHP you need to copy it into
    // HPHP::String.
    return m_row_block->getField<folly::StringPiece>(row, index);
  }
  catch (std::range_error& excep) {
    SystemLib::throwBadMethodCallExceptionObject(
      std::string("Error during conversion: ") + excep.what());
  }
}

template <typename FieldType>
FieldType AsyncMysqlRowBlock::getFieldAs(int64_t row, const Variant& field) {
  auto index = getIndexFromVariant(field);

  if (m_row_block->isNull(row, index)) {
    SystemLib::throwBadMethodCallExceptionObject(
        "Field value needs to be non-null.");
  }
  try {
    return m_row_block->getField<FieldType>(row, index);
  }
  catch (std::range_error& excep) {
    SystemLib::throwBadMethodCallExceptionObject(
      std::string("Error during conversion: ") + excep.what());
  }
}

static Variant
HHVM_METHOD(AsyncMysqlRowBlock, at, int64_t row, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  auto col_index = data->getIndexFromVariant(field);
  return buildTypedValue(
      data->m_row_block->getRowFields(),
      data->m_row_block->getRow(row),
      col_index,
      true);
}

static int64_t HHVM_METHOD(
    AsyncMysqlRowBlock,
    getFieldAsInt,
    int64_t row,
    const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->getFieldAs<int64_t>(row, field);
}

static double HHVM_METHOD(
    AsyncMysqlRowBlock,
    getFieldAsDouble,
    int64_t row,
    const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->getFieldAs<double>(row, field);
}

static OptString HHVM_METHOD(
    AsyncMysqlRowBlock,
    getFieldAsString,
    int64_t row,
    const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  auto val = data->getFieldAs<folly::StringPiece>(row, field);
  if (val.empty()) {
    return String::Empty();
  }
  return OptString{val};
}

static bool
HHVM_METHOD(AsyncMysqlRowBlock, isNull, int64_t row, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->isNull(row, data->getIndexFromVariant(field));
}

static int64_t
HHVM_METHOD(AsyncMysqlRowBlock, fieldType, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->getFieldType(data->getIndexFromVariant(field));
}

static int64_t
HHVM_METHOD(AsyncMysqlRowBlock, fieldFlags, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->getFieldFlags(data->getIndexFromVariant(field));
}

static OptString HHVM_METHOD(AsyncMysqlRowBlock, fieldName, int64_t field_id) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_field_index->getFieldString(field_id);
}

static bool HHVM_METHOD(AsyncMysqlRowBlock, isEmpty) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->empty();
}

static int64_t HHVM_METHOD(AsyncMysqlRowBlock, fieldsCount) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->numFields();
}

static int64_t HHVM_METHOD(AsyncMysqlRowBlock, count) {
  auto* data = Native::data<AsyncMysqlRowBlock>(this_);
  return data->m_row_block->numRows();
}

static Object HHVM_METHOD(AsyncMysqlRowBlock, getRow, int64_t row_no) {
  return AsyncMysqlRow::newInstance(Object{this_}, row_no);
}

static Object HHVM_METHOD(AsyncMysqlRowBlock, getIterator) {
  return AsyncMysqlRowBlockIterator::newInstance(Object{this_}, 0);
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowBlockIterator

Object AsyncMysqlRowBlockIterator::newInstance(Object row_block,
                                               size_t row_number) {
  Object ret{classof()};
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(ret);
  data->m_row_block = std::move(row_block);
  data->m_row_number = row_number;
  return ret;
}

static void HHVM_METHOD(AsyncMysqlRowBlockIterator, next) {
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(this_);
  data->m_row_number++;
}

static bool HHVM_METHOD(AsyncMysqlRowBlockIterator, valid) {
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(this_);

  static_assert(
      std::is_unsigned<decltype(data->m_row_number)>::value,
      "m_row_number should be unsigned");
  int64_t count = HHVM_MN(AsyncMysqlRowBlock, count)(data->m_row_block.get());
  return data->m_row_number < count;
}

static Object HHVM_METHOD(AsyncMysqlRowBlockIterator, current) {
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(this_);

  if (!HHVM_MN(AsyncMysqlRowBlockIterator, valid)(this_)) {
    throw_iterator_not_valid();
  }
  return HHVM_MN(AsyncMysqlRowBlock, getRow)(
      data->m_row_block.get(), data->m_row_number);
}

static int64_t HHVM_METHOD(AsyncMysqlRowBlockIterator, key) {
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(this_);
  return data->m_row_number;
}

static void HHVM_METHOD(AsyncMysqlRowBlockIterator, rewind) {
  auto* data = Native::data<AsyncMysqlRowBlockIterator>(this_);
  data->m_row_number = 0;
}

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRow

Object AsyncMysqlRow::newInstance(Object row_block, size_t row_number) {
  Object ret{classof()};
  auto* data = Native::data<AsyncMysqlRow>(ret);
  data->m_row_block = std::move(row_block);
  data->m_row_number = row_number;
  return ret;
}

#define ROW_BLOCK(method, ...) \
  HHVM_MN(AsyncMysqlRowBlock, method)(data->m_row_block.get(), __VA_ARGS__)

static Variant HHVM_METHOD(AsyncMysqlRow, at, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(at, data->m_row_number, field);
}

static int64_t HHVM_METHOD(AsyncMysqlRow, getFieldAsInt, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(getFieldAsInt, data->m_row_number, field);
}

static double
HHVM_METHOD(AsyncMysqlRow, getFieldAsDouble, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(getFieldAsDouble, data->m_row_number, field);
}

static OptString
HHVM_METHOD(AsyncMysqlRow, getFieldAsString, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(getFieldAsString, data->m_row_number, field);
}

static bool HHVM_METHOD(AsyncMysqlRow, isNull, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(isNull, data->m_row_number, field);
}

static int64_t HHVM_METHOD(AsyncMysqlRow, fieldType, const Variant& field) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return ROW_BLOCK(fieldType, field);
}

static int64_t HHVM_METHOD(AsyncMysqlRow, count) {
  auto* data = Native::data<AsyncMysqlRow>(this_);
  return HHVM_MN(AsyncMysqlRowBlock, fieldsCount)(data->m_row_block.get());
}

static Object HHVM_METHOD(AsyncMysqlRow, getIterator) {
  return AsyncMysqlRowIterator::newInstance(Object{this_}, 0);
}

#undef ROW_BLOCK

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowIterator

Object AsyncMysqlRowIterator::newInstance(Object row,
                                          size_t field_number) {
  Object ret{classof()};
  auto* data = Native::data<AsyncMysqlRowIterator>(ret);
  data->m_row = std::move(row);
  data->m_field_number = field_number;
  return ret;
}

static void HHVM_METHOD(AsyncMysqlRowIterator, next) {
  auto* data = Native::data<AsyncMysqlRowIterator>(this_);
  data->m_field_number++;
}

static bool HHVM_METHOD(AsyncMysqlRowIterator, valid) {
  auto* data = Native::data<AsyncMysqlRowIterator>(this_);

  static_assert(
      std::is_unsigned<decltype(data->m_field_number)>::value,
      "m_field_number should be unsigned");
  int64_t count = HHVM_MN(AsyncMysqlRow, count)(data->m_row.get());
  return data->m_field_number < count;
}

/*?? return as string? */
static OptString HHVM_METHOD(AsyncMysqlRowIterator, current) {
  auto* data = Native::data<AsyncMysqlRowIterator>(this_);
  return HHVM_MN(AsyncMysqlRow, getFieldAsString)(
      data->m_row.get(), (uint64_t)data->m_field_number);
}

static int64_t HHVM_METHOD(AsyncMysqlRowIterator, key) {
  auto* data = Native::data<AsyncMysqlRowIterator>(this_);
  return data->m_field_number;
}

static void HHVM_METHOD(AsyncMysqlRowIterator, rewind) {
  auto* data = Native::data<AsyncMysqlRowIterator>(this_);
  data->m_field_number = 0;
}

///////////////////////////////////////////////////////////////////////////////

static const int64_t DISABLE_COPY_AND_SWEEP = Native::NDIFlags::NO_COPY |
  Native::NDIFlags::NO_SWEEP;

static struct AsyncMysqlExtension final : Extension {
  // Since hhvm (and thereby the extension) is on a one-week release cycle
  // whereas www release is continuous, for forward compatibility, any future
  // modification of the extension that requires client (www) changes should
  // bump the version number and use a version guard in www:
  //   $ext = new ReflectionExtension("async_mysql");
  //   $version = (float) $ext->getVersion();
  AsyncMysqlExtension() : Extension("async_mysql", "8.5", "mysql_gateway") {}
  void moduleRegisterNative() override {
    // expose the mysql flags
    HHVM_RC_INT_SAME(NOT_NULL_FLAG);
    HHVM_RC_INT_SAME(PRI_KEY_FLAG);
    HHVM_RC_INT_SAME(UNIQUE_KEY_FLAG);
    HHVM_RC_INT_SAME(MULTIPLE_KEY_FLAG);
    HHVM_RC_INT_SAME(UNSIGNED_FLAG);
    HHVM_RC_INT_SAME(ZEROFILL_FLAG);
    HHVM_RC_INT_SAME(BINARY_FLAG);
    HHVM_RC_INT_SAME(AUTO_INCREMENT_FLAG);
    HHVM_RC_INT_SAME(ENUM_FLAG);
    HHVM_RC_INT_SAME(SET_FLAG);
    HHVM_RC_INT_SAME(BLOB_FLAG);
    HHVM_RC_INT_SAME(TIMESTAMP_FLAG);
    HHVM_RC_INT_SAME(NUM_FLAG);
    HHVM_RC_INT_SAME(NO_DEFAULT_VALUE_FLAG);

    // expose the mysql field types
    HHVM_RC_INT_SAME(MYSQL_TYPE_TINY);
    HHVM_RC_INT_SAME(MYSQL_TYPE_SHORT);
    HHVM_RC_INT_SAME(MYSQL_TYPE_LONG);
    HHVM_RC_INT_SAME(MYSQL_TYPE_INT24);
    HHVM_RC_INT_SAME(MYSQL_TYPE_LONGLONG);
    HHVM_RC_INT_SAME(MYSQL_TYPE_DECIMAL);
    HHVM_RC_INT_SAME(MYSQL_TYPE_NEWDECIMAL);
    HHVM_RC_INT_SAME(MYSQL_TYPE_FLOAT);
    HHVM_RC_INT_SAME(MYSQL_TYPE_DOUBLE);
    HHVM_RC_INT_SAME(MYSQL_TYPE_BIT);
    HHVM_RC_INT_SAME(MYSQL_TYPE_TIMESTAMP);
    HHVM_RC_INT_SAME(MYSQL_TYPE_DATE);
    HHVM_RC_INT_SAME(MYSQL_TYPE_TIME);
    HHVM_RC_INT_SAME(MYSQL_TYPE_DATETIME);
    HHVM_RC_INT_SAME(MYSQL_TYPE_YEAR);
    HHVM_RC_INT_SAME(MYSQL_TYPE_STRING);
    HHVM_RC_INT_SAME(MYSQL_TYPE_VAR_STRING);
    HHVM_RC_INT_SAME(MYSQL_TYPE_BLOB);
    HHVM_RC_INT_SAME(MYSQL_TYPE_SET);
    HHVM_RC_INT_SAME(MYSQL_TYPE_ENUM);
    HHVM_RC_INT_SAME(MYSQL_TYPE_GEOMETRY);
    HHVM_RC_INT_SAME(MYSQL_TYPE_NULL);

    HHVM_STATIC_ME(AsyncMysqlClient, setPoolsConnectionLimit);
    HHVM_STATIC_ME(AsyncMysqlClient, connect);
    HHVM_STATIC_ME(AsyncMysqlClient, connectWithOpts);
    HHVM_STATIC_ME(AsyncMysqlClient, connectAndQuery);

    HHVM_ME(AsyncMysqlConnectionPool, __construct);
    HHVM_ME(AsyncMysqlConnectionPool, getPoolStats);
    HHVM_ME(AsyncMysqlConnectionPool, connect);
    HHVM_ME(AsyncMysqlConnectionPool, connectWithOpts);
    HHVM_ME(AsyncMysqlConnectionPool, connectAndQuery);
    Native::registerNativeDataInfo<AsyncMysqlConnectionPool>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlClientStats, ioEventLoopMicrosAvg);
    HHVM_ME(AsyncMysqlClientStats, callbackDelayMicrosAvg);

    HHVM_ME(AsyncMysqlClientStats, ioThreadBusyMicrosAvg);
    HHVM_ME(AsyncMysqlClientStats, ioThreadIdleMicrosAvg);
    HHVM_ME(AsyncMysqlClientStats, notificationQueueSize);
    Native::registerNativeDataInfo<AsyncMysqlClientStats>();

    Native::registerNativeDataInfo<MySSLContextProvider>();
    HHVM_ME(MySSLContextProvider, isValid);
    HHVM_ME(MySSLContextProvider, allowSessionResumption);

    HHVM_ME(AsyncMysqlConnection, query);
    HHVM_ME(AsyncMysqlConnection, queryf);
    HHVM_ME(AsyncMysqlConnection, queryAsync);
    HHVM_ME(AsyncMysqlConnection, multiQuery);
    HHVM_ME(AsyncMysqlConnection, escapeString);
    HHVM_ME(AsyncMysqlConnection, close);
    HHVM_ME(AsyncMysqlConnection, isValid);
    HHVM_ME(AsyncMysqlConnection, serverInfo);
    HHVM_ME(AsyncMysqlConnection, sslSessionReused);
    HHVM_ME(AsyncMysqlConnection, isSSL);
    HHVM_ME(AsyncMysqlConnection, warningCount);
    HHVM_ME(AsyncMysqlConnection, host);
    HHVM_ME(AsyncMysqlConnection, port);
    HHVM_ME(AsyncMysqlConnection, setReusable);
    HHVM_ME(AsyncMysqlConnection, isReusable);
    HHVM_ME(AsyncMysqlConnection, connectResult);
    HHVM_ME(AsyncMysqlConnection, lastActivityTime);
    HHVM_ME(AsyncMysqlConnection, getSslCertCn);
    HHVM_ME(AsyncMysqlConnection, getSslCertSan);
    HHVM_ME(AsyncMysqlConnection, getSslCertExtensions);
    HHVM_ME(AsyncMysqlConnection, isSslCertValidationEnforced);
    HHVM_ME(AsyncMysqlConnection, getSslVersion);

    Native::registerNativeDataInfo<AsyncMysqlConnection>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlResult, sslSessionReused);
    HHVM_ME(AsyncMysqlResult, getSslCertCn);
    HHVM_ME(AsyncMysqlResult, getSslCertSan);
    HHVM_ME(AsyncMysqlResult, getSslCertExtensions);
    HHVM_ME(AsyncMysqlResult, isSslCertValidationEnforced);
    HHVM_ME(AsyncMysqlResult, getSslVersion);

    HHVM_ME(AsyncMysqlConnectResult, elapsedMicros);
    HHVM_ME(AsyncMysqlConnectResult, startTime);
    HHVM_ME(AsyncMysqlConnectResult, endTime);
    HHVM_ME(AsyncMysqlConnectResult, clientStats);
    Native::registerNativeDataInfo<AsyncMysqlConnectResult>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlErrorResult, elapsedMicros);
    HHVM_ME(AsyncMysqlErrorResult, startTime);
    HHVM_ME(AsyncMysqlErrorResult, endTime);
    HHVM_ME(AsyncMysqlErrorResult, clientStats);
    HHVM_ME(AsyncMysqlErrorResult, mysql_errno);
    HHVM_ME(AsyncMysqlErrorResult, mysql_error);
    HHVM_ME(AsyncMysqlErrorResult, failureType);
    Native::registerNativeDataInfo<AsyncMysqlErrorResult>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlQueryErrorResult, numSuccessfulQueries);
    HHVM_ME(AsyncMysqlQueryErrorResult, getSuccessfulResults);
    Native::registerNativeDataInfo<AsyncMysqlQueryErrorResult>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlQueryResult, elapsedMicros);
    HHVM_ME(AsyncMysqlQueryResult, startTime);
    HHVM_ME(AsyncMysqlQueryResult, endTime);
    HHVM_ME(AsyncMysqlQueryResult, clientStats);
    HHVM_ME(AsyncMysqlQueryResult, numRowsAffected);
    HHVM_ME(AsyncMysqlQueryResult, lastInsertId);
    HHVM_ME(AsyncMysqlQueryResult, numRows);
    HHVM_ME(AsyncMysqlQueryResult, mapRows);
    HHVM_ME(AsyncMysqlQueryResult, vectorRows);
    HHVM_ME(AsyncMysqlQueryResult, mapRowsTyped);
    HHVM_ME(AsyncMysqlQueryResult, dictRowsTyped);
    HHVM_ME(AsyncMysqlQueryResult, vectorRowsTyped);
    HHVM_ME(AsyncMysqlQueryResult, rowBlocks);
    HHVM_ME(AsyncMysqlQueryResult, noIndexUsed);
    HHVM_ME(AsyncMysqlQueryResult, recvGtid);
    HHVM_ME(AsyncMysqlQueryResult, responseAttributes);
    HHVM_ME(AsyncMysqlQueryResult, resultSizeBytes);
    HHVM_ME(AsyncMysqlQueryResult, mysqlInfo);
    HHVM_ME(AsyncMysqlQueryResult, rowsMatched);
    HHVM_ME(AsyncMysqlQueryResult, warningsCount);
    Native::registerNativeDataInfo<AsyncMysqlQueryResult>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlRowBlock, at);
    HHVM_ME(AsyncMysqlRowBlock, getFieldAsInt);
    HHVM_ME(AsyncMysqlRowBlock, getFieldAsDouble);
    HHVM_ME(AsyncMysqlRowBlock, getFieldAsString);
    HHVM_ME(AsyncMysqlRowBlock, isNull);
    HHVM_ME(AsyncMysqlRowBlock, fieldType);
    HHVM_ME(AsyncMysqlRowBlock, fieldFlags);
    HHVM_ME(AsyncMysqlRowBlock, fieldName);
    HHVM_ME(AsyncMysqlRowBlock, isEmpty);
    HHVM_ME(AsyncMysqlRowBlock, fieldsCount);
    HHVM_ME(AsyncMysqlRowBlock, count);
    HHVM_ME(AsyncMysqlRowBlock, getIterator);
    HHVM_ME(AsyncMysqlRowBlock, getRow);
    Native::registerNativeDataInfo<AsyncMysqlRowBlock>(
      Native::NDIFlags::NO_COPY);

    HHVM_ME(AsyncMysqlRowBlockIterator, valid);
    HHVM_ME(AsyncMysqlRowBlockIterator, next);
    HHVM_ME(AsyncMysqlRowBlockIterator, current);
    HHVM_ME(AsyncMysqlRowBlockIterator, key);
    HHVM_ME(AsyncMysqlRowBlockIterator, rewind);
    Native::registerNativeDataInfo<AsyncMysqlRowBlockIterator>(
      DISABLE_COPY_AND_SWEEP);

    HHVM_ME(AsyncMysqlRow, at);
    HHVM_ME(AsyncMysqlRow, getFieldAsInt);
    HHVM_ME(AsyncMysqlRow, getFieldAsDouble);
    HHVM_ME(AsyncMysqlRow, getFieldAsString);
    HHVM_ME(AsyncMysqlRow, isNull);
    HHVM_ME(AsyncMysqlRow, fieldType);
    HHVM_ME(AsyncMysqlRow, count);
    HHVM_ME(AsyncMysqlRow, getIterator);
    Native::registerNativeDataInfo<AsyncMysqlRow>(
      DISABLE_COPY_AND_SWEEP);

    HHVM_ME(AsyncMysqlRowIterator, valid);
    HHVM_ME(AsyncMysqlRowIterator, next);
    HHVM_ME(AsyncMysqlRowIterator, current);
    HHVM_ME(AsyncMysqlRowIterator, key);
    HHVM_ME(AsyncMysqlRowIterator, rewind);
    Native::registerNativeDataInfo<AsyncMysqlRowIterator>(
      DISABLE_COPY_AND_SWEEP);

    HHVM_ME(AsyncMysqlConnectionOptions, setConnectTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, setConnectTcpTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, setConnectAttempts);
    HHVM_ME(AsyncMysqlConnectionOptions, setTotalTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, setQueryTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, setConnectionAttributes);
    HHVM_ME(AsyncMysqlConnectionOptions, setSSLOptionsProvider);
    HHVM_ME(AsyncMysqlConnectionOptions, setSniServerName);
    HHVM_ME(AsyncMysqlConnectionOptions, enableResetConnBeforeClose);
    HHVM_ME(AsyncMysqlConnectionOptions, enableDelayedResetConn);
    HHVM_ME(AsyncMysqlConnectionOptions, enableChangeUser);
    HHVM_ME(AsyncMysqlConnectionOptions, setServerCertValidation);
    HHVM_ME(AsyncMysqlConnectionOptions, getConnectTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, getConnectTcpTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, getConnectAttempts);
    HHVM_ME(AsyncMysqlConnectionOptions, getTotalTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, getQueryTimeout);
    HHVM_ME(AsyncMysqlConnectionOptions, getConnectionAttributes);

    Native::registerNativeDataInfo<AsyncMysqlConnectionOptions>();

    HHVM_NAMED_ME(HH\\Lib\\SQL\\Query,
        toString__FOR_DEBUGGING_ONLY,
        HHLibSQLQuery__toString__FOR_DEBUGGING_ONLY);
    HHVM_NAMED_ME(HH\\Lib\\SQL\\Query,
        toUnescapedString__FOR_DEBUGGING_ONLY__UNSAFE,
        HHLibSQLQuery__toUnescapedString__FOR_DEBUGGING_ONLY__UNSAFE);
  }

  std::vector<std::string> hackFiles() const override {
    return {
      "ext_mysqlrow.php",
      "ext_async_mysql_exceptions.php",
      "ext_async_mysql.php",
    };
  }

  void moduleLoad(const IniSetting::Map& ini, Hdf config) override {
    Config::Bind(
        HdfAsyncMysqlClientPoolSize,
        ini,
        config,
        "AsyncMysql.ClientPoolSize",
        2);
  }
} s_async_mysql_extension;

///////////////////////////////////////////////////////////////////////////////
}
