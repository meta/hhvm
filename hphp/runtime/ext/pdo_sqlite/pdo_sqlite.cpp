/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include "hphp/runtime/ext/pdo_sqlite/pdo_sqlite.h"
#include "hphp/runtime/ext/sqlite3/ext_sqlite3.h"
#include "hphp/runtime/ext/std/ext_std_function.h"
#include "hphp/runtime/ext/stream/ext_stream.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/file.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

namespace HPHP {

IMPLEMENT_DEFAULT_EXTENSION_VERSION(pdo_sqlite, 1.0.1);

///////////////////////////////////////////////////////////////////////////////

struct PDOSqliteStatement : PDOStatement {
  DECLARE_RESOURCE_ALLOCATION(PDOSqliteStatement)
  PDOSqliteStatement(sqlite3 *db, sqlite3_stmt* stmt);
  ~PDOSqliteStatement() override;

  bool support(SupportedMethod method) override;
  bool executer() override;
  bool fetcher(PDOFetchOrientation ori, long offset) override;
  bool describer(int colno) override;
  bool getColumn(int colno, Variant &value) override;
  bool paramHook(PDOBoundParam* param, PDOParamEvent event_type) override;
  bool getColumnMeta(int64_t colno, Array &return_value) override;
  bool cursorCloser() override;

private:
  sqlite3 *m_db;
  sqlite3_stmt *m_stmt;
  unsigned m_pre_fetched:1;
  unsigned m_done:1;

  int handleError(const char *file, int line);
};

///////////////////////////////////////////////////////////////////////////////

static int
authorizer(void* /*autharg*/, int access_type, const char* arg3,
           const char* arg4, const char* /*arg5*/, const char* /*arg6*/) {
  switch (access_type) {
  case SQLITE_COPY: {
    OptString filename = File::TranslatePath(arg4);
    return filename.empty() ? SQLITE_DENY : SQLITE_OK;
  }
  case SQLITE_ATTACH: {
    OptString filename = File::TranslatePath(arg3);
    return filename.empty() ? SQLITE_DENY : SQLITE_OK;
  }
  default:
    return SQLITE_OK; /* access allowed */
  }
}

///////////////////////////////////////////////////////////////////////////////

PDOSqliteConnection::PDOSqliteConnection() : m_db(NULL) {
  m_einfo.file = NULL;
  m_einfo.line = 0;
  m_einfo.errcode = 0;
  m_einfo.errmsg = NULL;

  alloc_own_columns = 1;
  max_escaped_char_length = 2;
}

PDOSqliteConnection::~PDOSqliteConnection() {
  if (m_db) {
    sqlite3_close_v2(m_db);
  }
  if (m_einfo.errmsg) {
    free(m_einfo.errmsg);
  }
}

bool PDOSqliteConnection::create(const Array& options) {
  OptString filename = m_data_source.substr(0,1) == ":" ? OptString(m_data_source) :
                    File::TranslatePath(m_data_source);
  if (filename.empty()) {
    throw_pdo_exception(Array(),
                        "safe_mode/open_basedir prohibits opening %s",
                        m_data_source.c_str());
    return false;
  }

  if (sqlite3_open(filename.data(), &m_db) != SQLITE_OK) {
    handleError(__FILE__, __LINE__);
    return false;
  }

  sqlite3_set_authorizer(m_db, authorizer, NULL);

  long timeout = 60;
  if (options.exists(PDO_ATTR_TIMEOUT)) {
    timeout = options[PDO_ATTR_TIMEOUT].toInt64();
  }
  sqlite3_busy_timeout(m_db, timeout * 1000);

  return true;
}

bool PDOSqliteConnection::support(SupportedMethod method) {
  return method != MethodCheckLiveness;
}

int PDOSqliteConnection::handleError(const char *file, int line,
                                     PDOStatement *stmt /* = NULL */) {
  PDOErrorType *pdo_err = stmt ? &stmt->error_code : &error_code;

  m_einfo.errcode = sqlite3_errcode(m_db);
  m_einfo.file = file;
  m_einfo.line = line;

  if (m_einfo.errcode != SQLITE_OK) {
    if (m_einfo.errmsg) {
      free(m_einfo.errmsg);
    }
    m_einfo.errmsg = strdup((char*)sqlite3_errmsg(m_db));
  } else { /* no error */
    setPDOErrorNone(*pdo_err);
    return false;
  }

  switch (m_einfo.errcode) {
  case SQLITE_NOTFOUND:    setPDOError(*pdo_err, "42S02");  break;
  case SQLITE_INTERRUPT:   setPDOError(*pdo_err, "01002");  break;
  case SQLITE_NOLFS:       setPDOError(*pdo_err, "HYC00");  break;
  case SQLITE_TOOBIG:      setPDOError(*pdo_err, "22001");  break;
  case SQLITE_CONSTRAINT:  setPDOError(*pdo_err, "23000");  break;
  case SQLITE_ERROR:
  default:
    setPDOError(*pdo_err, "HY000");
    break;
  }

  return m_einfo.errcode;
}

bool PDOSqliteConnection::closer() {
  if (m_db) {
    sqlite3_close_v2(m_db);
    m_db = NULL;
  }
  if (m_einfo.errmsg) {
    free(m_einfo.errmsg);
    m_einfo.errmsg = NULL;
  }
  return true;
}

bool PDOSqliteConnection::preparer(const OptString& sql, sp_PDOStatement *stmt,
                                   const Variant& options) {
  if (options.toArray().exists(PDO_ATTR_CURSOR) &&
      options.toArray()[PDO_ATTR_CURSOR].toInt64() != PDO_CURSOR_FWDONLY) {
    m_einfo.errcode = SQLITE_ERROR;
    handleError(__FILE__, __LINE__);
    return false;
  }

  const char *tail;
  sqlite3_stmt *rawstmt = NULL;
  if (sqlite3_prepare_v2(m_db, sql.data(), sql.size(), &rawstmt, &tail)
      == SQLITE_OK) {

    *stmt = req::make<PDOSqliteStatement>(m_db, rawstmt);
    return true;
  }

  handleError(__FILE__, __LINE__);
  return false;
}

int64_t PDOSqliteConnection::doer(const OptString& sql) {
  char *errmsg = NULL;
  if (sqlite3_exec(m_db, sql.data(), NULL, NULL, &errmsg) != SQLITE_OK) {
    handleError(__FILE__, __LINE__);
    if (errmsg) sqlite3_free(errmsg);
    return -1;
  }
  return sqlite3_changes(m_db);
}

bool PDOSqliteConnection::quoter(const OptString& input, OptString& quoted,
                                 PDOParamType /*paramtype*/) {
  int len = 2 * input.size() + 3;
  OptString s(len, ReserveString);
  char *buf = s.mutableData();
  sqlite3_snprintf(len, buf, "'%q'", input.data());
  quoted = s.setSize(strlen(buf));
  return true;
}

bool PDOSqliteConnection::begin() {
  char *errmsg = NULL;
  if (sqlite3_exec(m_db, "BEGIN", NULL, NULL, &errmsg) != SQLITE_OK) {
    handleError(__FILE__, __LINE__);
    if (errmsg) sqlite3_free(errmsg);
    return false;
  }
  return true;
}

bool PDOSqliteConnection::commit() {
  char *errmsg = NULL;
  if (sqlite3_exec(m_db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
    handleError(__FILE__, __LINE__);
    if (errmsg) sqlite3_free(errmsg);
    return false;
  }
  return true;
}

bool PDOSqliteConnection::rollback() {
  char *errmsg = NULL;
  if (sqlite3_exec(m_db, "ROLLBACK", NULL, NULL, &errmsg) != SQLITE_OK) {
    handleError(__FILE__, __LINE__);
    if (errmsg) sqlite3_free(errmsg);
    return false;
  }
  return true;
}

bool PDOSqliteConnection::setAttribute(int64_t attr, const Variant& value) {
  switch (attr) {
  case PDO_ATTR_TIMEOUT:
    sqlite3_busy_timeout(m_db, value.toInt64() * 1000);
    return true;
  }
  return false;
}

OptString PDOSqliteConnection::lastId(const char* /*name*/) {
  return OptString::FromInt64((int64_t)sqlite3_last_insert_rowid(m_db));
}

bool PDOSqliteConnection::fetchErr(PDOStatement* /*stmt*/, Array& info) {
  if (m_einfo.errcode) {
    info.append((int64_t)m_einfo.errcode);
    info.append(OptString(m_einfo.errmsg, CopyString));
  }
  return true;
}

int PDOSqliteConnection::getAttribute(int64_t attr, Variant &value) {
  switch (attr) {
  case PDO_ATTR_CLIENT_VERSION:
  case PDO_ATTR_SERVER_VERSION:
    value = OptString((char *)sqlite3_libversion(), CopyString);
    return true;
  default:
    return false;
  }
  return true;
}

// hidden in ext/ext_sqlite.cpp
void php_sqlite3_callback_func(sqlite3_context* context, int argc,
                               sqlite3_value** argv);

///////////////////////////////////////////////////////////////////////////////

bool PDOSqliteResource::createFunction(const OptString& name,
                                       const Variant& callback,
                                       int argcount) {
  if (!is_callable(callback)) {
    raise_warning("function '%s' is not callable", callback.toString().data());
    return false;
  }

  auto udf = req::make_unique<UDF>();

  udf->func = callback;
  udf->argc = argcount;
  udf->name = name.toCppString();

  auto stat = sqlite3_create_function(
    conn()->m_db,
    udf->name.c_str(),
    argcount,
    SQLITE_UTF8,
    static_cast<SQLite3::UserDefinedFunc*>(udf.get()),
    php_sqlite3_callback_func,
    nullptr,
    nullptr
  );

  if (stat != SQLITE_OK) {
    return false;
  }
  m_udfs.emplace_back(std::move(udf));

  return true;
}

void PDOSqliteResource::sweep() {
  for (auto& udf : m_udfs) {
    udf->func.releaseForSweep();
    udf->step.releaseForSweep();
    udf->fini.releaseForSweep();
  }

  for (auto const& udf : m_udfs) {
    sqlite3_create_function(conn()->m_db, udf->name.data(), 0, SQLITE_UTF8,
                            nullptr, nullptr, nullptr, nullptr);
  }
  m_udfs.clear();

  PDOResource::sweep();
}

///////////////////////////////////////////////////////////////////////////////

PDOSqliteStatement::PDOSqliteStatement(sqlite3 *db, sqlite3_stmt* stmt)
    : m_db(db), m_stmt(stmt), m_pre_fetched(0), m_done(0) {
  supports_placeholders = PDO_PLACEHOLDER_POSITIONAL | PDO_PLACEHOLDER_NAMED;
}

PDOSqliteStatement::~PDOSqliteStatement() {
  sweep();
}

void PDOSqliteStatement::sweep() {
  if (m_stmt) {
    sqlite3_finalize(m_stmt);
  }
}

bool PDOSqliteStatement::support(SupportedMethod method) {
  switch (method) {
  case MethodSetAttribute:
  case MethodGetAttribute:
  case MethodNextRowset:
    return false;
  default:
    break;
  }
  return true;
}

int PDOSqliteStatement::handleError(const char *file, int line) {
  auto rsrc = unsafe_cast<PDOSqliteResource>(dbh);
  assertx(rsrc);
  auto conn = rsrc->conn();
  assertx(conn);
  return conn->handleError(file, line, this);
}

bool PDOSqliteStatement::executer() {
  if (executed && !m_done) {
    sqlite3_reset(m_stmt);
  }

  m_done = 0;
  switch (sqlite3_step(m_stmt)) {
  case SQLITE_ROW:
    m_pre_fetched = 1;
    column_count = sqlite3_data_count(m_stmt);
    return true;

  case SQLITE_DONE:
    column_count = sqlite3_column_count(m_stmt);
    row_count = sqlite3_changes(m_db);
    sqlite3_reset(m_stmt);
    m_done = 1;
    return true;

  case SQLITE_ERROR:
    sqlite3_reset(m_stmt);
    [[fallthrough]];
  case SQLITE_MISUSE:
  case SQLITE_BUSY:
  default:
    handleError(__FILE__, __LINE__);
    return false;
  }
}

bool PDOSqliteStatement::fetcher(PDOFetchOrientation /*ori*/, long /*offset*/) {
  SYNC_VM_REGS_SCOPED();
  if (!m_stmt) {
    return false;
  }
  if (m_pre_fetched) {
    m_pre_fetched = 0;
    return true;
  }
  if (m_done) {
    return false;
  }
  switch (sqlite3_step(m_stmt)) {
  case SQLITE_ROW:
    return true;

  case SQLITE_DONE:
    m_done = 1;
    sqlite3_reset(m_stmt);
    return false;

  case SQLITE_ERROR:
    sqlite3_reset(m_stmt);
    [[fallthrough]];
  default:
    handleError(__FILE__, __LINE__);
    return false;
  }
}

bool PDOSqliteStatement::describer(int colno) {
  if (colno < 0 || colno >= column_count) {
    /* error invalid column */
    handleError(__FILE__, __LINE__);
    return false;
  }

  if (columns.empty()) {
    for (int i = 0; i < column_count; i++) {
      columns.set(i, Variant(req::make<PDOColumn>()));
    }
  }

  auto col = cast<PDOColumn>(columns[colno]);
  col->name = OptString(sqlite3_column_name(m_stmt, colno), CopyString);
  col->maxlen = 0xffffffff;
  col->precision = 0;

  switch (sqlite3_column_type(m_stmt, colno)) {
  case SQLITE_INTEGER:
  case SQLITE_FLOAT:
  case SQLITE3_TEXT:
  case SQLITE_BLOB:
  case SQLITE_NULL:
  default:
    col->param_type = PDO_PARAM_STR;
    break;
  }

  return true;
}

bool PDOSqliteStatement::getColumn(int colno, Variant &value) {
  if (!m_stmt) {
    return false;
  }
  if (colno >= sqlite3_data_count(m_stmt)) {
    /* error invalid column */
    handleError(__FILE__, __LINE__);
    return false;
  }
  char *ptr; int len;
  switch (sqlite3_column_type(m_stmt, colno)) {
  case SQLITE_NULL:
    ptr = NULL;
    len = 0;
    break;
  case SQLITE_BLOB:
    ptr = (char*)sqlite3_column_blob(m_stmt, colno);
    len = sqlite3_column_bytes(m_stmt, colno);
    break;
  case SQLITE3_TEXT:
    ptr = (char*)sqlite3_column_text(m_stmt, colno);
    len = sqlite3_column_bytes(m_stmt, colno);
    break;
  default:
    ptr = (char*)sqlite3_column_text(m_stmt, colno);
    len = sqlite3_column_bytes(m_stmt, colno);
    break;
  }
  value = OptString(ptr, len, CopyString);
  return true;
}

bool PDOSqliteStatement::paramHook(PDOBoundParam* param,
                                   PDOParamEvent event_type) {
  switch (event_type) {
  case PDO_PARAM_EVT_EXEC_PRE:
    if (executed && !m_done) {
      sqlite3_reset(m_stmt);
      m_done = 1;
    }

      if (param->paramno == -1) {
        param->paramno = sqlite3_bind_parameter_index(m_stmt,
                                                      param->name.c_str()) - 1;
      }

      switch (PDO_PARAM_TYPE(param->param_type)) {
      case PDO_PARAM_STMT:
        return false;

      case PDO_PARAM_NULL:
        if (sqlite3_bind_null(m_stmt, param->paramno + 1) == SQLITE_OK) {
          return true;
        }
        handleError(__FILE__, __LINE__);
        return false;

      case PDO_PARAM_INT:
      case PDO_PARAM_BOOL:
        if (param->parameter.isNull()) {
          if (sqlite3_bind_null(m_stmt, param->paramno + 1) == SQLITE_OK) {
            return true;
          }
        } else {
          if (SQLITE_OK == sqlite3_bind_int(m_stmt, param->paramno + 1,
                                            param->parameter.toInt64())) {
            return true;
          }
        }
        handleError(__FILE__, __LINE__);
        return false;

      case PDO_PARAM_LOB:
        if (param->parameter.isResource()) {
          Variant buf = HHVM_FN(stream_get_contents)(
                        param->parameter.toResource());
          if (!same(buf, false)) {
            param->parameter = buf;
          } else {
            pdo_raise_impl_error(dbh, this, "HY105",
                                 "Expected a stream resource");
            return false;
          }
        } else if (param->parameter.isNull()) {
          if (sqlite3_bind_null(m_stmt, param->paramno + 1) == SQLITE_OK) {
            return true;
          }
          handleError(__FILE__, __LINE__);
          return false;
        }

        {
          OptString sparam = param->parameter.toString();
          if (SQLITE_OK == sqlite3_bind_blob(m_stmt, param->paramno + 1,
                                             sparam.data(), sparam.size(),
                                             SQLITE_STATIC)) {
            return true;
          }
        }
        handleError(__FILE__, __LINE__);
        return false;

      case PDO_PARAM_STR:
      default:
        if (param->parameter.isNull()) {
          if (sqlite3_bind_null(m_stmt, param->paramno + 1) == SQLITE_OK) {
            return true;
          }
        } else {
          OptString sparam = param->parameter.toString();
          if (SQLITE_OK == sqlite3_bind_text(m_stmt, param->paramno + 1,
                                             sparam.data(), sparam.size(),
                                             SQLITE_STATIC)) {
            return true;
          }
        }
        handleError(__FILE__, __LINE__);
        return false;
      }

  default:;
  }
  return true;
}

const StaticString
  s_native_type("native_type"),
  s_null("null"),
  s_double("double"),
  s_blob("blob"),
  s_string("string"),
  s_integer("integer"),
  s_sqlite_decl_type("sqlite:decl_type"),
  s_table("table"),
  s_flags("flags");

bool PDOSqliteStatement::getColumnMeta(int64_t colno, Array &ret) {
  if (!m_stmt) {
    return false;
  }
  if (colno >= sqlite3_data_count(m_stmt)) {
    /* error invalid column */
    handleError(__FILE__, __LINE__);
    return false;
  }

  ret = Array::CreateDict();
  Array flags = Array::CreateVec();
  switch (sqlite3_column_type(m_stmt, colno)) {
  case SQLITE_NULL:    ret.set(s_native_type, s_null);    break;
  case SQLITE_FLOAT:   ret.set(s_native_type, s_double);  break;
  case SQLITE_BLOB:    flags.append(s_blob); [[fallthrough]];
  case SQLITE_TEXT:    ret.set(s_native_type, s_string);  break;
  case SQLITE_INTEGER: ret.set(s_native_type, s_integer); break;
  }

  const char *str = sqlite3_column_decltype(m_stmt, colno);
  if (str) {
    ret.set(s_sqlite_decl_type, OptString((char *)str, CopyString));
  }

#ifdef SQLITE_ENABLE_COLUMN_METADATA
  str = sqlite3_column_table_name(m_stmt, colno);
  if (str) {
    ret.set(s_table, OptString((char *)str, CopyString));
  }
#endif

  ret.set(s_flags, flags);
  return true;
}

bool PDOSqliteStatement::cursorCloser() {
  sqlite3_reset(m_stmt);
  return true;
}

///////////////////////////////////////////////////////////////////////////////

PDOSqlite::PDOSqlite() : PDODriver("sqlite") {}

req::ptr<PDOResource> PDOSqlite::createResourceImpl() {
  return req::make<PDOSqliteResource>(
    std::make_shared<PDOSqliteConnection>());
}

req::ptr<PDOResource> PDOSqlite::createResource(
  const sp_PDOConnection& conn
) {
  return req::make<PDOSqliteResource>(
      std::dynamic_pointer_cast<PDOSqliteConnection>(conn));
}

///////////////////////////////////////////////////////////////////////////////
}
