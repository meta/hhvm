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

#include <set>
#include <string>
#include <unordered_map>

#include "hphp/system/systemlib.h"
#include "hphp/util/hphp-config.h"
#include "hphp/util/rds-local.h"
#include "hphp/util/string-vsnprintf.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/file.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/vm/interp-helpers.h"

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/array/ext_array.h"
#include "hphp/runtime/ext/pdo/pdo_driver.h"
#ifdef ENABLE_EXTENSION_PDO_MYSQL
#include "hphp/runtime/ext/pdo_mysql/pdo_mysql.h"
#endif
#ifdef ENABLE_EXTENSION_PDO_SQLITE
#include "hphp/runtime/ext/pdo_sqlite/pdo_sqlite.h"
#endif
#include "hphp/runtime/ext/std/ext_std_classobj.h"
#include "hphp/runtime/ext/std/ext_std_function.h"
#include "hphp/runtime/ext/stream/ext_stream.h"
#include "hphp/runtime/ext/string/ext_string.h"

#define PDO_HANDLE_DBH_ERR(dbh)                         \
  if (strcmp(dbh->conn()->error_code, PDO_ERR_NONE)) {  \
    pdo_handle_error(dbh, nullptr);                     \
  }                                                     \

#define PDO_HANDLE_STMT_ERR(stmt)                       \
  if (strcmp(stmt->error_code, PDO_ERR_NONE)) {         \
    pdo_handle_error(stmt->dbh, stmt);                  \
  }                                                     \

namespace HPHP {

struct PDOData {
  sp_PDOResource m_dbh;
};

struct PDOStatementData {
  PDOStatementData();
  ~PDOStatementData();

  sp_PDOStatement m_stmt;
  Variant m_row;
  int m_rowIndex;
};

using std::string;
///////////////////////////////////////////////////////////////////////////////
// extension functions

Array HHVM_FUNCTION(pdo_drivers) {
  const auto& drivers = PDODriver::GetDrivers();
  VecInit ret{drivers.size()};
  for (const auto& driver : drivers) {
    ret.append(driver.second->getName());
  }
  return ret.toArray();
}

///////////////////////////////////////////////////////////////////////////////
// error handling

struct pdo_sqlstate_info {
  const char *state;
  const char *desc;
};

static const struct pdo_sqlstate_info err_initializer[] = {
  { "00000",  "No error" },
  { "01000",  "Warning" },
  { "01001",  "Cursor operation conflict" },
  { "01002",  "Disconnect error" },
  { "01003",  "NULL value eliminated in set function" },
  { "01004",  "String data, right truncated" },
  { "01006",  "Privilege not revoked" },
  { "01007",  "Privilege not granted" },
  { "01008",  "Implicit zero bit padding" },
  { "0100C",  "Dynamic result sets returned" },
  { "01P01",  "Deprecated feature" },
  { "01S00",  "Invalid connection string attribute" },
  { "01S01",  "Error in row" },
  { "01S02",  "Option value changed" },
  { "01S06",
    "Attempt to fetch before the result set returned the first rowset" },
  { "01S07",  "Fractional truncation" },
  { "01S08",  "Error saving File DSN" },
  { "01S09",  "Invalid keyword" },
  { "02000",  "No data" },
  { "02001",  "No additional dynamic result sets returned" },
  { "03000",  "Sql statement not yet complete" },
  { "07002",  "COUNT field incorrect" },
  { "07005",  "Prepared statement not a cursor-specification" },
  { "07006",  "Restricted data type attribute violation" },
  { "07009",  "Invalid descriptor index" },
  { "07S01",  "Invalid use of default parameter" },
  { "08000",  "Connection exception" },
  { "08001",  "Client unable to establish connection" },
  { "08002",  "Connection name in use" },
  { "08003",  "Connection does not exist" },
  { "08004",  "Server rejected the connection" },
  { "08006",  "Connection failure" },
  { "08007",  "Connection failure during transaction" },
  { "08S01",  "Communication link failure" },
  { "09000",  "Triggered action exception" },
  { "0A000",  "Feature not supported" },
  { "0B000",  "Invalid transaction initiation" },
  { "0F000",  "Locator exception" },
  { "0F001",  "Invalid locator specification" },
  { "0L000",  "Invalid grantor" },
  { "0LP01",  "Invalid grant operation" },
  { "0P000",  "Invalid role specification" },
  { "21000",  "Cardinality violation" },
  { "21S01",  "Insert value list does not match column list" },
  { "21S02",  "Degree of derived table does not match column list" },
  { "22000",  "Data exception" },
  { "22001",  "String data, right truncated" },
  { "22002",  "Indicator variable required but not supplied" },
  { "22003",  "Numeric value out of range" },
  { "22004",  "Null value not allowed" },
  { "22005",  "Error in assignment" },
  { "22007",  "Invalid datetime format" },
  { "22008",  "Datetime field overflow" },
  { "22009",  "Invalid time zone displacement value" },
  { "2200B",  "Escape character conflict" },
  { "2200C",  "Invalid use of escape character" },
  { "2200D",  "Invalid escape octet" },
  { "2200F",  "Zero length character string" },
  { "2200G",  "Most specific type mismatch" },
  { "22010",  "Invalid indicator parameter value" },
  { "22011",  "Substring error" },
  { "22012",  "Division by zero" },
  { "22015",  "Interval field overflow" },
  { "22018",  "Invalid character value for cast specification" },
  { "22019",  "Invalid escape character" },
  { "2201B",  "Invalid regular expression" },
  { "2201E",  "Invalid argument for logarithm" },
  { "2201F",  "Invalid argument for power function" },
  { "2201G",  "Invalid argument for width bucket function" },
  { "22020",  "Invalid limit value" },
  { "22021",  "Character not in repertoire" },
  { "22022",  "Indicator overflow" },
  { "22023",  "Invalid parameter value" },
  { "22024",  "Unterminated c string" },
  { "22025",  "Invalid escape sequence" },
  { "22026",  "String data, length mismatch" },
  { "22027",  "Trim error" },
  { "2202E",  "Array subscript error" },
  { "22P01",  "Floating point exception" },
  { "22P02",  "Invalid text representation" },
  { "22P03",  "Invalid binary representation" },
  { "22P04",  "Bad copy file format" },
  { "22P05",  "Untranslatable character" },
  { "23000",  "Integrity constraint violation" },
  { "23001",  "Restrict violation" },
  { "23502",  "Not null violation" },
  { "23503",  "Foreign key violation" },
  { "23505",  "Unique violation" },
  { "23514",  "Check violation" },
  { "24000",  "Invalid cursor state" },
  { "25000",  "Invalid transaction state" },
  { "25001",  "Active sql transaction" },
  { "25002",  "Branch transaction already active" },
  { "25003",  "Inappropriate access mode for branch transaction" },
  { "25004",  "Inappropriate isolation level for branch transaction" },
  { "25005",  "No active sql transaction for branch transaction" },
  { "25006",  "Read only sql transaction" },
  { "25007",  "Schema and data statement mixing not supported" },
  { "25008",  "Held cursor requires same isolation level" },
  { "25P01",  "No active sql transaction" },
  { "25P02",  "In failed sql transaction" },
  { "25S01",  "Transaction state" },
  { "25S02",  "Transaction is still active" },
  { "25S03",  "Transaction is rolled back" },
  { "26000",  "Invalid sql statement name" },
  { "27000",  "Triggered data change violation" },
  { "28000",  "Invalid authorization specification" },
  { "2B000",  "Dependent privilege descriptors still exist" },
  { "2BP01",  "Dependent objects still exist" },
  { "2D000",  "Invalid transaction termination" },
  { "2F000",  "Sql routine exception" },
  { "2F002",  "Modifying sql data not permitted" },
  { "2F003",  "Prohibited sql statement attempted" },
  { "2F004",  "Reading sql data not permitted" },
  { "2F005",  "Function executed no return statement" },
  { "34000",  "Invalid cursor name" },
  { "38000",  "External routine exception" },
  { "38001",  "Containing sql not permitted" },
  { "38002",  "Modifying sql data not permitted" },
  { "38003",  "Prohibited sql statement attempted" },
  { "38004",  "Reading sql data not permitted" },
  { "39000",  "External routine invocation exception" },
  { "39001",  "Invalid sqlstate returned" },
  { "39004",  "Null value not allowed" },
  { "39P01",  "Trigger protocol violated" },
  { "39P02",  "Srf protocol violated" },
  { "3B000",  "Savepoint exception" },
  { "3B001",  "Invalid savepoint specification" },
  { "3C000",  "Duplicate cursor name" },
  { "3D000",  "Invalid catalog name" },
  { "3F000",  "Invalid schema name" },
  { "40000",  "Transaction rollback" },
  { "40001",  "Serialization failure" },
  { "40002",  "Transaction integrity constraint violation" },
  { "40003",  "Statement completion unknown" },
  { "40P01",  "Deadlock detected" },
  { "42000",  "Syntax error or access violation" },
  { "42501",  "Insufficient privilege" },
  { "42601",  "Syntax error" },
  { "42602",  "Invalid name" },
  { "42611",  "Invalid column definition" },
  { "42622",  "Name too long" },
  { "42701",  "Duplicate column" },
  { "42702",  "Ambiguous column" },
  { "42703",  "Undefined column" },
  { "42704",  "Undefined object" },
  { "42710",  "Duplicate object" },
  { "42712",  "Duplicate alias" },
  { "42723",  "Duplicate function" },
  { "42725",  "Ambiguous function" },
  { "42803",  "Grouping error" },
  { "42804",  "Datatype mismatch" },
  { "42809",  "Wrong object type" },
  { "42830",  "Invalid foreign key" },
  { "42846",  "Cannot coerce" },
  { "42883",  "Undefined function" },
  { "42939",  "Reserved name" },
  { "42P01",  "Undefined table" },
  { "42P02",  "Undefined parameter" },
  { "42P03",  "Duplicate cursor" },
  { "42P04",  "Duplicate database" },
  { "42P05",  "Duplicate prepared statement" },
  { "42P06",  "Duplicate schema" },
  { "42P07",  "Duplicate table" },
  { "42P08",  "Ambiguous parameter" },
  { "42P09",  "Ambiguous alias" },
  { "42P10",  "Invalid column reference" },
  { "42P11",  "Invalid cursor definition" },
  { "42P12",  "Invalid database definition" },
  { "42P13",  "Invalid function definition" },
  { "42P14",  "Invalid prepared statement definition" },
  { "42P15",  "Invalid schema definition" },
  { "42P16",  "Invalid table definition" },
  { "42P17",  "Invalid object definition" },
  { "42P18",  "Indeterminate datatype" },
  { "42S01",  "Base table or view already exists" },
  { "42S02",  "Base table or view not found" },
  { "42S11",  "Index already exists" },
  { "42S12",  "Index not found" },
  { "42S21",  "Column already exists" },
  { "42S22",  "Column not found" },
  { "44000",  "WITH CHECK OPTION violation" },
  { "53000",  "Insufficient resources" },
  { "53100",  "Disk full" },
  { "53200",  "Out of memory" },
  { "53300",  "Too many connections" },
  { "54000",  "Program limit exceeded" },
  { "54001",  "Statement too complex" },
  { "54011",  "Too many columns" },
  { "54023",  "Too many arguments" },
  { "55000",  "Object not in prerequisite state" },
  { "55006",  "Object in use" },
  { "55P02",  "Cant change runtime param" },
  { "55P03",  "Lock not available" },
  { "57000",  "Operator intervention" },
  { "57014",  "Query canceled" },
  { "57P01",  "Admin shutdown" },
  { "57P02",  "Crash shutdown" },
  { "57P03",  "Cannot connect now" },
  { "58030",  "Io error" },
  { "58P01",  "Undefined file" },
  { "58P02",  "Duplicate file" },
  { "F0000",  "Config file error" },
  { "F0001",  "Lock file exists" },
  { "HY000",  "General error" },
  { "HY001",  "Memory allocation error" },
  { "HY003",  "Invalid application buffer type" },
  { "HY004",  "Invalid SQL data type" },
  { "HY007",  "Associated statement is not prepared" },
  { "HY008",  "Operation canceled" },
  { "HY009",  "Invalid use of null pointer" },
  { "HY010",  "Function sequence error" },
  { "HY011",  "Attribute cannot be set now" },
  { "HY012",  "Invalid transaction operation code" },
  { "HY013",  "Memory management error" },
  { "HY014",  "Limit on the number of handles exceeded" },
  { "HY015",  "No cursor name available" },
  { "HY016",  "Cannot modify an implementation row descriptor" },
  { "HY017",  "Invalid use of an automatically allocated descriptor handle" },
  { "HY018",  "Server declined cancel request" },
  { "HY019",  "Non-character and non-binary data sent in pieces" },
  { "HY020",  "Attempt to concatenate a null value" },
  { "HY021",  "Inconsistent descriptor information" },
  { "HY024",  "Invalid attribute value" },
  { "HY090",  "Invalid string or buffer length" },
  { "HY091",  "Invalid descriptor field identifier" },
  { "HY092",  "Invalid attribute/option identifier" },
  { "HY093",  "Invalid parameter number" },
  { "HY095",  "Function type out of range" },
  { "HY096",  "Invalid information type" },
  { "HY097",  "Column type out of range" },
  { "HY098",  "Scope type out of range" },
  { "HY099",  "Nullable type out of range" },
  { "HY100",  "Uniqueness option type out of range" },
  { "HY101",  "Accuracy option type out of range" },
  { "HY103",  "Invalid retrieval code" },
  { "HY104",  "Invalid precision or scale value" },
  { "HY105",  "Invalid parameter type" },
  { "HY106",  "Fetch type out of range" },
  { "HY107",  "Row value out of range" },
  { "HY109",  "Invalid cursor position" },
  { "HY110",  "Invalid driver completion" },
  { "HY111",  "Invalid bookmark value" },
  { "HYC00",  "Optional feature not implemented" },
  { "HYT00",  "Timeout expired" },
  { "HYT01",  "Connection timeout expired" },
  { "IM001",  "Driver does not support this function" },
  { "IM002",  "Data source name not found and no default driver specified" },
  { "IM003",  "Specified driver could not be loaded" },
  { "IM004",  "Driver's SQLAllocHandle on SQL_HANDLE_ENV failed" },
  { "IM005",  "Driver's SQLAllocHandle on SQL_HANDLE_DBC failed" },
  { "IM006",  "Driver's SQLSetConnectAttr failed" },
  { "IM007",  "No data source or driver specified; dialog prohibited" },
  { "IM008",  "Dialog failed" },
  { "IM009",  "Unable to load translation DLL" },
  { "IM010",  "Data source name too long" },
  { "IM011",  "Driver name too long" },
  { "IM012",  "DRIVER keyword syntax error" },
  { "IM013",  "Trace file error" },
  { "IM014",  "Invalid name of File DSN" },
  { "IM015",  "Corrupt file data source" },
  { "P0000",  "Plpgsql error" },
  { "P0001",  "Raise exception" },
  { "XX000",  "Internal error" },
  { "XX001",  "Data corrupted" },
  { "XX002",  "Index corrupted" }
};

struct PDOErrorHash : private hphp_const_char_map<const char *> {
  PDOErrorHash() {
    for (unsigned int i = 0;
         i < sizeof(err_initializer)/sizeof(err_initializer[0]); i++) {
      const struct pdo_sqlstate_info *info = &err_initializer[i];
      (*this)[info->state] = info->desc;
    }
  }

  const char *description(const char *state) {
    const_iterator iter = find(state);
    if (iter != end()) {
      return iter->second;
    }
    return "<<Unknown error>>";
  }
};
static PDOErrorHash s_err_hash;

const StaticString
  s_code("code"),
  s_message("message"),
  s_errorInfo("errorInfo"),
  s_PDOException("PDOException");

void throw_pdo_exception(const Variant& info, const char *fmt, ...) {
  auto obj = SystemLib::AllocPDOExceptionObject();
  obj->o_set(s_code, 0, s_PDOException);

  va_list ap;
  va_start(ap, fmt);
  string msg;
  string_vsnprintf(msg, fmt, ap);
  obj->o_set(s_message, OptString(msg), s_PDOException);
  va_end(ap);

  if (!info.isNull()) {
    obj->o_set(s_errorInfo, info, s_PDOException);
  }
  throw_object(obj);
}

void pdo_raise_impl_error(sp_PDOResource rsrc, PDOStatement* stmt,
                          const char *sqlstate, const char *supp) {
  auto const& dbh = rsrc->conn();

  PDOErrorType *pdo_err = &dbh->error_code;
  if (stmt) {
    pdo_err = &stmt->error_code;
  }
  setPDOError(*pdo_err, sqlstate);

  const char *msg = s_err_hash.description(sqlstate);
  string err = "SQLSTATE["; err += sqlstate; err += "]: "; err += msg;
  if (supp) {
    err += ": "; err += supp;
  }

  if (dbh->error_mode != PDO_ERRMODE_EXCEPTION) {
    raise_warning("%s", err.c_str());
  } else {
    VecInit info(2);
    info.append(OptString(*pdo_err, CopyString));
    info.append(0LL);
    throw_pdo_exception(info.toArray(), "%s",
                        err.c_str());
  }
}

void pdo_raise_impl_error(sp_PDOResource rsrc, sp_PDOStatement stmt,
                          const char *sqlstate, const char *supp) {
  pdo_raise_impl_error(rsrc, stmt.get(), sqlstate, supp);
}

namespace {

void pdo_handle_error(sp_PDOResource rsrc, PDOStatement* stmt) {
  auto const& dbh = rsrc->conn();

  if (dbh->error_mode == PDO_ERRMODE_SILENT) {
    return;
  }
  PDOErrorType *pdo_err = &dbh->error_code;
  if (stmt) {
    pdo_err = &stmt->error_code;
  }

  /* hash sqlstate to error messages */
  const char *msg = s_err_hash.description(*pdo_err);

  int64_t native_code = 0;
  OptString supp;
  Array info;
  if (dbh->support(PDOConnection::MethodFetchErr)) {
    info = make_vec_array(OptString(*pdo_err, CopyString));
    if (dbh->fetchErr(stmt, info)) {
      if (info.exists(1)) {
        native_code = info[1].toInt64();
      }
      if (info.exists(2)) {
        supp = info[2].toString();
      }
    }
  }

  string err = "SQLSTATE["; err += *pdo_err; err += "]: "; err += msg;
  if (!supp.empty()) {
    err += ": "; err += OptString::FromInt64(native_code).data();
    err += " ";  err += supp.data();
  }

  if (dbh->error_mode != PDO_ERRMODE_EXCEPTION) {
    raise_warning("%s", err.c_str());
  } else {
    throw_pdo_exception(info, "%s", err.c_str());
  }
}

void pdo_handle_error(sp_PDOResource rsrc, sp_PDOStatement stmt) {
  pdo_handle_error(rsrc, stmt.get());
}

}

///////////////////////////////////////////////////////////////////////////////
// helpers for PDO class

static inline int64_t pdo_attr_lval(const Array& options, PDOAttributeType name,
                                  int64_t defval) {
  if (options.exists(name)) {
    return options[name].toInt64();
  }
  return defval;
}

static Object pdo_stmt_instantiate(sp_PDOResource dbh, const OptString& clsname,
                                   const Variant& ctor_args) {
  OptString name = clsname;
  if (name.empty()) {
    name = "PDOStatement";
  }
  if (!ctor_args.isNull() && !ctor_args.isArray()) {
    pdo_raise_impl_error(dbh, nullptr, "HY000",
                         "constructor arguments must be passed as an array");
    return Object();
  }
  Class* cls = Class::load(name.get());
  if (!cls) {
    return Object();
  }
  callerDynamicConstructChecks(cls);
  return Object{cls};
}

const StaticString s_queryString("queryString");

static void pdo_stmt_construct(sp_PDOStatement stmt, Object object,
                               const OptString& clsname,
                               const Variant& ctor_args) {
  object->setProp(nullctx, s_queryString.get(), stmt->query_string.asTypedValue());
  if (clsname.empty()) {
    return;
  }
  Class* cls = Class::load(clsname.get());
  if (!cls) {
    return;
  }
  ObjectData* inst = object.get();
  tvDecRefGen(
    g_context->invokeFunc(cls->getCtor(), ctor_args.toArray(), nullptr, inst)
  );
}

static bool valid_statement_class(sp_PDOResource dbh, const Variant& opt,
                                  OptString &clsname, Variant &ctor_args) {
  if (!opt.isArray() || !opt.toArray().exists(0) ||
      !opt.toArray()[0].isString() ||
      !HHVM_FN(class_exists)(opt.toArray()[0].toString())) {
    pdo_raise_impl_error
      (dbh, nullptr, "HY000",
       "PDO::ATTR_STATEMENT_CLASS requires format array(classname, "
       "array(ctor_args)); the classname must be a string specifying "
       "an existing class");
    PDO_HANDLE_DBH_ERR(dbh);
    return false;
  }
  clsname = opt.toArray()[0].toString();
  if (clsname == OptString("PDOStatement")) {
    ctor_args = Variant(Array());
    return true;
  }
  if (!HHVM_FN(is_a)(clsname, "PDOStatement", /* allow_string */ true)) {
    pdo_raise_impl_error
      (dbh, nullptr, "HY000",
       "user-supplied statement class must be derived from PDOStatement");
    PDO_HANDLE_DBH_ERR(dbh);
    return false;
  }
  HPHP::Class* cls = HPHP::Class::load(clsname.get());
  if (cls) {
    const HPHP::Func* method = cls->getDeclaredCtor();
    if (method && method->isPublic()) {
      pdo_raise_impl_error
        (dbh, nullptr, "HY000",
         "user-supplied statement class cannot have a public constructor");
      PDO_HANDLE_DBH_ERR(dbh);
      return false;
    }
  }
  if (opt.toArray().exists(1)) {
    Variant item = opt.toArray()[1];
    if (!item.isArray()) {
      pdo_raise_impl_error
        (dbh, nullptr, "HY000",
         "PDO::ATTR_STATEMENT_CLASS requires format array(classname, "
         "ctor_args); ctor_args must be an array");
      PDO_HANDLE_DBH_ERR(dbh);
      return false;
    }
    ctor_args = item;
  }
  return true;
}

static bool pdo_stmt_describe_columns(sp_PDOStatement stmt) {
  for (int col = 0; col < stmt->column_count; col++) {
    if (!stmt->describer(col)) {
      return false;
    }

    auto column = cast<PDOColumn>(stmt->columns[col]);

    /* if we are applying case conversions on column names, do so now */
    if (stmt->dbh->conn()->native_case != stmt->dbh->conn()->desired_case &&
        stmt->dbh->conn()->desired_case != PDO_CASE_NATURAL) {
      switch (stmt->dbh->conn()->desired_case) {
      case PDO_CASE_UPPER:
        column->name = HHVM_FN(strtoupper)(column->name);
        break;
      case PDO_CASE_LOWER:
        column->name = HHVM_FN(strtolower)(column->name);
        break;
      default:;
      }
    }

    auto const column_key =
      stmt->bound_columns.convertKey<IntishCast::Cast>(column->name);
    if (stmt->bound_columns.exists(column_key)) {
      auto param = cast<PDOBoundParam>(stmt->bound_columns[column_key]);
      param->paramno = col;
    }
  }
  return true;
}

static bool pdo_stmt_verify_mode(sp_PDOStatement stmt, int64_t mode,
                                 bool fetch_all) {
  int flags = mode & PDO_FETCH_FLAGS;
  mode = mode & ~PDO_FETCH_FLAGS;

  if (mode < 0 || mode > PDO_FETCH__MAX) {
    pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "invalid fetch mode");
    return false;
  }

  if (mode == PDO_FETCH_USE_DEFAULT) {
    flags = stmt->default_fetch_type & PDO_FETCH_FLAGS;
    mode = stmt->default_fetch_type & ~PDO_FETCH_FLAGS;
  }

  switch (mode) {
  case PDO_FETCH_FUNC:
    if (!fetch_all) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "PDO::FETCH_FUNC is only allowed in "
                           "PDOStatement::fetchAll()");
      return false;
    }
    return true;

  case PDO_FETCH_LAZY:
    if (fetch_all) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "PDO::FETCH_LAZY can't be used with "
                           "PDOStatement::fetchAll()");
      return false;
    }
    [[fallthrough]];

  default:
    if ((flags & PDO_FETCH_SERIALIZE) == PDO_FETCH_SERIALIZE) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "PDO::FETCH_SERIALIZE can only be used "
                           "together with PDO::FETCH_CLASS");
      return false;
    }
    if ((flags & PDO_FETCH_CLASSTYPE) == PDO_FETCH_CLASSTYPE) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "PDO::FETCH_CLASSTYPE can only be used "
                           "together with PDO::FETCH_CLASS");
      return false;
    }
    if (mode >= PDO_FETCH__MAX) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "invalid fetch mode");
      return false;
    }
    [[fallthrough]];

  case PDO_FETCH_CLASS:
    break;
  }
  return true;
}

static bool do_fetch_class_prepare(sp_PDOStatement stmt) {
  OptString clsname = stmt->fetch.clsname;
  if (clsname.empty()) {
    stmt->fetch.clsname = "stdClass";
  }
  stmt->fetch.constructor = String::Empty(); //NULL;
  HPHP::Class* cls = HPHP::Class::load(clsname.get());
  if (cls) {
    const HPHP::Func* method = cls->getDeclaredCtor();
    if (method) {
      stmt->fetch.constructor = method->nameStr();
      return true;
    }
  }
  if (!stmt->fetch.ctor_args.isNull()) {
    pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                         "user-supplied class does not have a constructor, "
                         "use NULL for the ctor_params parameter, or simply "
                         "omit it");
    return false;
  }
  return true; /* no ctor no args is also ok */
}

static bool pdo_stmt_set_fetch_mode(sp_PDOStatement stmt, int _argc,
                                    int64_t mode, const Array& _argv) {
  _argc = _argv.size() + 1;

  if (stmt->default_fetch_type == PDO_FETCH_INTO) {
    stmt->fetch.into.unset();
  }
  stmt->default_fetch_type = PDO_FETCH_BOTH;

  if (!pdo_stmt_verify_mode(stmt, mode, false)) {
    setPDOErrorNone(stmt->error_code);
    return false;
  }

  int flags = mode & PDO_FETCH_FLAGS;
  bool retval = false;
  switch (mode & ~PDO_FETCH_FLAGS) {
  case PDO_FETCH_USE_DEFAULT:
  case PDO_FETCH_LAZY:
  case PDO_FETCH_ASSOC:
  case PDO_FETCH_NUM:
  case PDO_FETCH_BOTH:
  case PDO_FETCH_OBJ:
  case PDO_FETCH_BOUND:
  case PDO_FETCH_NAMED:
  case PDO_FETCH_KEY_PAIR:
    if (_argc != 1) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "fetch mode doesn't allow any extra arguments");
    } else {
      retval = true;
    }
    break;

  case PDO_FETCH_COLUMN:
    if (_argc != 2) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "fetch mode requires the colno argument");
    } else  if (!_argv[0].isInteger()) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "colno must be an integer");
    } else {
      stmt->fetch.column = _argv[0].toInt64();
      retval = true;
    }
    break;

  case PDO_FETCH_CLASS:
    /* Gets its class name from 1st column */
    if ((flags & PDO_FETCH_CLASSTYPE) == PDO_FETCH_CLASSTYPE) {
      if (_argc != 1) {
        pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                             "fetch mode doesn't allow any extra arguments");
      } else {
        stmt->fetch.clsname.clear();
        retval = true;
      }
    } else {
      if (_argc < 2) {
        pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                             "fetch mode requires the classname argument");
      } else if (_argc > 3) {
        pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                             "too many arguments");
      } else if (!_argv[0].isString()) {
        pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                             "classname must be a string");
      } else {
        retval = HHVM_FN(class_exists)(_argv[0].toString());
        if (retval) {
          stmt->fetch.clsname = _argv[0].toString();
        }
      }
    }

    if (retval) {
      stmt->fetch.ctor_args.unset();
      if (_argc == 3) {
        if (!_argv[1].isNull() && !_argv[1].isArray()) {
          pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                               "ctor_args must be either NULL or an array");
          retval = false;
        } else {
          stmt->fetch.ctor_args = _argv[1];
        }
      }

      if (retval) {
        do_fetch_class_prepare(stmt);
      }
    }

    break;

  case PDO_FETCH_INTO:
    if (_argc != 2) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "fetch mode requires the object parameter");
    } else if (!_argv[0].isObject()) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "object must be an object");
    } else {
      retval = true;
    }

    if (retval) {
      stmt->fetch.into = _argv[0];
    }
    break;

  default:
    pdo_raise_impl_error(stmt->dbh, stmt, "22003",
                         "Invalid fetch mode specified");
  }

  if (retval) {
    stmt->default_fetch_type = (PDOFetchType)mode;
  }

  /*
   * PDO error (if any) has already been raised at this point.
   *
   * The error_code is cleared, otherwise the caller will read the
   * last error message from the driver.
   *
   */
  setPDOErrorNone(stmt->error_code);
  return retval;
}

///////////////////////////////////////////////////////////////////////////////
// forward declarations

bool HHVM_METHOD(PDO, setAttribute, int64_t attribute,
                 const Variant& value);

///////////////////////////////////////////////////////////////////////////////
// PDO

namespace {
  using StorageT = std::unordered_map<std::string, sp_PDOConnection>;
  RDS_LOCAL(StorageT, s_connections);
}

const StaticString s_PDO("PDO");

void HHVM_METHOD(PDO, __construct, const OptString& dsn,
                 const OptString& username /* = null_string */,
                 const OptString& password /* = null_string */,
                 const Variant& optionsV /* = null_array */) {
  auto data = Native::data<PDOData>(this_);
  auto options = optionsV.isNull() ? null_array : optionsV.toArray();

  OptString data_source = dsn;

  /* parse the data source name */
  const char *colon = strchr(data_source.data(), ':');
  if (!colon) {
    /* let's see if this string has a matching dsn in the php.ini */
    OptString name = "pdo.dsn."; name += data_source;
    OptString ini_dsn;
    if (!IniSetting::Get(name, ini_dsn)) {
      throw_pdo_exception(uninit_null(), "invalid data source name");
    }
    data_source = ini_dsn;
    colon = strchr(data_source.data(), ':');
    if (!colon) {
      throw_pdo_exception(uninit_null(),
                          "invalid data source name (via INI: %s)",
                          ini_dsn.data());
    }
  }

  if (!strncmp(data_source.data(), "uri:", 4)) {
    /* the specified URI holds connection details */
    auto file = File::Open(data_source.substr(4), "rb");
    if (!file || file->isInvalid()) {
      throw_pdo_exception(uninit_null(), "invalid data source URI");
    }
    data_source = file->readLine(1024);
    colon = strchr(data_source.data(), ':');
    if (!colon) {
      throw_pdo_exception(uninit_null(), "invalid data source name (via URI)");
    }
  }

  const PDODriverMap &drivers = PDODriver::GetDrivers();
  OptString name = data_source.substr(0, colon - data_source.data());
  PDODriverMap::const_iterator iter = drivers.find(name.data());
  if (iter == drivers.end()) {
    /* NB: don't want to include the data_source in the error message as
     * it might contain a password */
    throw_pdo_exception(uninit_null(), "could not find driver");
  }
  PDODriver *driver = iter->second;

  /* is this supposed to be a persistent connection ? */
  bool is_persistent = false;
  bool call_factory = true;
  std::string shashkey;
  if (!options.empty()) {
    StringBuffer hashkey;
    if (options.exists(PDO_ATTR_PERSISTENT)) {
      Variant v = options[PDO_ATTR_PERSISTENT];
      OptString sv = v.toString();
      if (v.isString() && !sv.isNumeric() && !sv.empty()) {
        /* user specified key */
        hashkey.printf("PDO:DBH:DSN=%s:%s:%s:%s",
                       data_source.data(), username.data(),
                       password.data(), sv.data());
        is_persistent = true;
      } else {
        is_persistent = v.toInt64();
        hashkey.printf("PDO:DBH:DSN=%s:%s:%s",
                       data_source.data(), username.data(),
                       password.data());
      }
    }

    if (is_persistent) {
      shashkey = hashkey.detach().toCppString();

      /* let's see if we have one cached.... */
      if (s_connections->count(shashkey)) {
        auto const conn = (*s_connections)[shashkey];
        data->m_dbh = driver->createResource(conn);

        /* is the connection still alive ? */
        if (conn->support(PDOConnection::MethodCheckLiveness) &&
            !conn->checkLiveness()) {
          /* nope... need to kill it */
          data->m_dbh = nullptr;
        }
      }

      if (data->m_dbh) {
        call_factory = false;
      } else {
        /* need a brand new pdbh */
        data->m_dbh = driver->createResource(colon + 1, username,
                                             password, options);
        if (!data->m_dbh) {
          throw_pdo_exception(uninit_null(), "unable to create a connection");
        }
        data->m_dbh->conn()->persistent_id = shashkey;
      }
    }
  }
  if (!data->m_dbh) {
    data->m_dbh = driver->createResource(colon + 1, username,
                                         password, options);
    if (!data->m_dbh) {
      throw_pdo_exception(uninit_null(), "unable to create a connection");
    }
  }

  if (call_factory) {
    data->m_dbh->conn()->default_fetch_type = PDO_FETCH_BOTH;
  }

  data->m_dbh->conn()->auto_commit =
    pdo_attr_lval(options, PDO_ATTR_AUTOCOMMIT, 1);

  if (!call_factory) {
    /* we got a persistent guy from our cache */
    for (ArrayIter iter(options); iter; ++iter) {
      HHVM_MN(PDO, setAttribute)(this_, iter.first().toInt64(),
                                      iter.second());
    }
  } else if (data->m_dbh) {
    if (is_persistent) {
      assertx(!shashkey.empty());
      (*s_connections)[shashkey] = data->m_dbh->conn();
    }

    data->m_dbh->conn()->driver = driver;
    for (ArrayIter iter(options); iter; ++iter) {
      HHVM_MN(PDO, setAttribute)(this_, iter.first().toInt64(),
                                      iter.second());
    }
  }
}

Variant HHVM_METHOD(PDO, prepare, const OptString& statement,
                    const Array& options = null_array) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  OptString clsname;
  Variant ctor_args;
  if (options.exists(PDO_ATTR_STATEMENT_CLASS)) {
    Variant opt = options[PDO_ATTR_STATEMENT_CLASS];
    if (!valid_statement_class(data->m_dbh, opt, clsname, ctor_args)) {
      return false;
    }
  } else {
    clsname = data->m_dbh->conn()->def_stmt_clsname;
    ctor_args = data->m_dbh->def_stmt_ctor_args;
  }

  Object ret = pdo_stmt_instantiate(data->m_dbh, clsname, ctor_args);
  if (ret.isNull()) {
    pdo_raise_impl_error
      (data->m_dbh, nullptr, "HY000",
       "failed to instantiate user-supplied statement class");
    PDO_HANDLE_DBH_ERR(data->m_dbh);
    return false;
  }
  PDOStatementData *pdostmt = Native::data<PDOStatementData>(ret);

  if (data->m_dbh->conn()->preparer(statement, &pdostmt->m_stmt, options)) {
    auto stmt = pdostmt->m_stmt;
    assertx(stmt);

    /* unconditionally keep this for later reference */
    stmt->query_string = statement;
    stmt->default_fetch_type = data->m_dbh->conn()->default_fetch_type;
    stmt->dbh = data->m_dbh;

    pdo_stmt_construct(stmt, ret, clsname, ctor_args);
    return ret;
  }

  PDO_HANDLE_DBH_ERR(data->m_dbh);
  return false;
}

 static bool HHVM_METHOD(PDO, beginTransaction) {
  auto data = Native::data<PDOData>(this_);

  if (data->m_dbh->conn()->in_txn) {
    throw_pdo_exception(uninit_null(),
                        "There is already an active transaction");
  }
  if (data->m_dbh->conn()->begin()) {
    data->m_dbh->conn()->in_txn = 1;
    return true;
  }
  if (strcmp(data->m_dbh->conn()->error_code, PDO_ERR_NONE)) {
    pdo_handle_error(data->m_dbh, nullptr);
  }
  return false;
}

static bool HHVM_METHOD(PDO, commit) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  if (!data->m_dbh->conn()->in_txn) {
    throw_pdo_exception(uninit_null(), "There is no active transaction");
  }
  if (data->m_dbh->conn()->commit()) {
    data->m_dbh->conn()->in_txn = 0;
    return true;
  }
  PDO_HANDLE_DBH_ERR(data->m_dbh);
  return false;
}

static bool HHVM_METHOD(PDO, inTransaction) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  return data->m_dbh->conn()->in_txn;
}

static bool HHVM_METHOD(PDO, rollBack) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  if (!data->m_dbh->conn()->in_txn) {
    throw_pdo_exception(uninit_null(), "There is no active transaction");
  }
  if (data->m_dbh->conn()->rollback()) {
    data->m_dbh->conn()->in_txn = 0;
    return true;
  }
  PDO_HANDLE_DBH_ERR(data->m_dbh);
  return false;
}

bool HHVM_METHOD(PDO, setAttribute, int64_t attribute,
                 const Variant& value) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);

#define PDO_LONG_PARAM_CHECK                                           \
  if (!value.isInteger() && !value.isString() && !value.isBoolean()) { \
    pdo_raise_impl_error(data->m_dbh, nullptr, "HY000",                \
                         "attribute value must be an integer");        \
    PDO_HANDLE_DBH_ERR(data->m_dbh);                                   \
    return false;                                                      \
  }                                                                    \

  switch (attribute) {
  case PDO_ATTR_ERRMODE:
    PDO_LONG_PARAM_CHECK;
    switch (value.toInt64()) {
    case PDO_ERRMODE_SILENT:
    case PDO_ERRMODE_WARNING:
    case PDO_ERRMODE_EXCEPTION:
      data->m_dbh->conn()->error_mode = (PDOErrorMode)value.toInt64();
      return true;
    default:
      pdo_raise_impl_error(data->m_dbh, nullptr, "HY000", "invalid error mode");
      PDO_HANDLE_DBH_ERR(data->m_dbh);
      return false;
    }
    return false;

  case PDO_ATTR_CASE:
    PDO_LONG_PARAM_CHECK;
    switch (value.toInt64()) {
    case PDO_CASE_NATURAL:
    case PDO_CASE_UPPER:
    case PDO_CASE_LOWER:
      data->m_dbh->conn()->desired_case = (PDOCaseConversion)value.toInt64();
      return true;
    default:
      pdo_raise_impl_error(data->m_dbh, nullptr, "HY000",
                           "invalid case folding mode");
      PDO_HANDLE_DBH_ERR(data->m_dbh);
      return false;
    }
    return false;

  case PDO_ATTR_ORACLE_NULLS:
    PDO_LONG_PARAM_CHECK;
    data->m_dbh->conn()->oracle_nulls = value.toInt64();
    return true;

  case PDO_ATTR_DEFAULT_FETCH_MODE:
    if (value.isArray()) {
      if (value.asCArrRef().exists(0)) {
        Variant tmp = value.asCArrRef()[0];
        if (tmp.isInteger() && ((tmp.toInt64() == PDO_FETCH_INTO ||
                                 tmp.toInt64() == PDO_FETCH_CLASS))) {
          pdo_raise_impl_error(data->m_dbh, nullptr, "HY000",
                               "FETCH_INTO and FETCH_CLASS are not yet "
                               "supported as default fetch modes");
          return false;
        }
      }
    } else {
      PDO_LONG_PARAM_CHECK;
    }
    if (value.toInt64() == PDO_FETCH_USE_DEFAULT) {
      pdo_raise_impl_error(data->m_dbh, nullptr,
                           "HY000", "invalid fetch mode type");
      return false;
    }
    data->m_dbh->conn()->default_fetch_type = (PDOFetchType)value.toInt64();
    return true;

  case PDO_ATTR_STRINGIFY_FETCHES:
    PDO_LONG_PARAM_CHECK;
    data->m_dbh->conn()->stringify = value.toInt64() ? 1 : 0;
    return true;

  case PDO_ATTR_STATEMENT_CLASS:
    {
      if (data->m_dbh->conn()->is_persistent) {
        pdo_raise_impl_error(data->m_dbh, nullptr, "HY000",
                             "PDO::ATTR_STATEMENT_CLASS cannot be used "
                             "with persistent PDO instances");
        PDO_HANDLE_DBH_ERR(data->m_dbh);
        return false;
      }
      OptString clsname;
      if (!valid_statement_class(data->m_dbh, value, clsname,
                                 data->m_dbh->def_stmt_ctor_args)) {
        return false;
      }
      data->m_dbh->conn()->def_stmt_clsname = clsname.c_str();
      return true;
    }
  }

  if (data->m_dbh->conn()->support(PDOConnection::MethodSetAttribute)) {
    setPDOErrorNone(data->m_dbh->conn()->error_code);
    data->m_dbh->query_stmt = nullptr;
    if (data->m_dbh->conn()->setAttribute(attribute, value)) {
      return true;
    }
  }

  if (attribute == PDO_ATTR_AUTOCOMMIT) {
    throw_pdo_exception(uninit_null(),
                        "The auto-commit mode cannot be changed for this "
                        "driver");
  } else if (!data->m_dbh->conn()->support(PDOConnection::MethodSetAttribute)) {
    pdo_raise_impl_error(data->m_dbh, nullptr, "IM001",
                         "driver does not support setting attributes");
  } else {
    PDO_HANDLE_DBH_ERR(data->m_dbh);
  }
  return false;
}

Variant HHVM_METHOD(PDO, getAttribute, int64_t attribute) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  /* handle generic PDO-level attributes */
  switch (attribute) {
  case PDO_ATTR_PERSISTENT:
    return (bool)data->m_dbh->conn()->is_persistent;

  case PDO_ATTR_CASE:
    return (int64_t)data->m_dbh->conn()->desired_case;

  case PDO_ATTR_ORACLE_NULLS:
    return (int64_t)data->m_dbh->conn()->oracle_nulls;

  case PDO_ATTR_ERRMODE:
    return (int64_t)data->m_dbh->conn()->error_mode;

  case PDO_ATTR_DRIVER_NAME:
    return OptString(data->m_dbh->conn()->driver->getName());

  case PDO_ATTR_STATEMENT_CLASS: {
    Array ret;
    ret.append(OptString(data->m_dbh->conn()->def_stmt_clsname));
    if (!data->m_dbh->def_stmt_ctor_args.isNull()) {
      ret.append(data->m_dbh->def_stmt_ctor_args);
    }
    return ret;
  }
  case PDO_ATTR_DEFAULT_FETCH_MODE:
    return (int64_t)data->m_dbh->conn()->default_fetch_type;
  }

  if (!data->m_dbh->conn()->support(PDOConnection::MethodGetAttribute)) {
    pdo_raise_impl_error(data->m_dbh, nullptr, "IM001",
                         "driver does not support getting attributes");
    return false;
  }

  Variant ret;
  switch (data->m_dbh->conn()->getAttribute(attribute, ret)) {
  case -1:
    PDO_HANDLE_DBH_ERR(data->m_dbh);
    return false;
  case 0:
    pdo_raise_impl_error(data->m_dbh, nullptr, "IM001",
                         "driver does not support that attribute");
    return false;
  }
  return ret;
}

Variant HHVM_METHOD(PDO, exec, const OptString& query) {
  auto data = Native::data<PDOData>(this_);

  SYNC_VM_REGS_SCOPED();
  if (query.empty()) {
    pdo_raise_impl_error(data->m_dbh, nullptr, "HY000",
                         "trying to execute an empty query");
    return false;
  }

  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  int64_t ret = data->m_dbh->conn()->doer(query);
  if (ret == -1) {
    PDO_HANDLE_DBH_ERR(data->m_dbh);
    return false;
  }
  return ret;
}

static Variant HHVM_METHOD(PDO, lastInsertId,
                           const OptString& seqname /* = null_string */) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  if (!data->m_dbh->conn()->support(PDOConnection::MethodLastId)) {
    pdo_raise_impl_error(data->m_dbh, nullptr, "IM001",
                         "driver does not support lastInsertId()");
    return false;
  }

  OptString ret = data->m_dbh->conn()->lastId(seqname.data());
  if (ret.empty()) {
    PDO_HANDLE_DBH_ERR(data->m_dbh);
    return false;
  }
  return ret;
}

static Variant HHVM_METHOD(PDO, errorCode) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  if (data->m_dbh->query_stmt) {
    return OptString(data->m_dbh->query_stmt->error_code, CopyString);
  }

  if (data->m_dbh->conn()->error_code[0] == '\0') {
    return init_null();
  }

  /**
   * Making sure that we fallback to the default implementation
   * if the dbh->error_code is not null.
   */
  return OptString(data->m_dbh->conn()->error_code, CopyString);
}

static Array HHVM_METHOD(PDO, errorInfo) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);

  Array ret = Array::CreateVec();
  if (data->m_dbh->query_stmt) {
    ret.append(OptString(data->m_dbh->query_stmt->error_code, CopyString));
  } else {
    ret.append(OptString(data->m_dbh->conn()->error_code, CopyString));
  }

  if (data->m_dbh->conn()->support(PDOConnection::MethodFetchErr)) {
    data->m_dbh->conn()->fetchErr(data->m_dbh->query_stmt, ret);
  }

  /**
   * In order to be consistent, we have to make sure we add the good amount
   * of nulls depending on the current number of elements. We make a simple
   * difference and add the needed elements
   */
  int error_count = ret.size();
  int error_expected_count = 3;
  if (error_expected_count > error_count) {
    int error_count_diff = error_expected_count - error_count;
    for (int i = 0; i < error_count_diff; i++) {
      ret.append(init_null_variant);
    }
  }
  return ret;
}

Variant HHVM_METHOD(PDO, query, const OptString& sql, const Array& _argv) {

  auto data = Native::data<PDOData>(this_);
  SYNC_VM_REGS_SCOPED();
  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  Object ret = pdo_stmt_instantiate(data->m_dbh,
                                    data->m_dbh->conn()->def_stmt_clsname,
                                    data->m_dbh->def_stmt_ctor_args);
  if (ret.isNull()) {
    pdo_raise_impl_error
      (data->m_dbh, nullptr, "HY000",
       "failed to instantiate user supplied statement class");
    return init_null();
  }
  PDOStatementData *pdostmt = Native::data<PDOStatementData>(ret);

  if (data->m_dbh->conn()->preparer(sql, &pdostmt->m_stmt, Array())) {
    auto stmt = pdostmt->m_stmt;
    assertx(stmt);

    /* unconditionally keep this for later reference */
    stmt->query_string = sql;
    stmt->default_fetch_type = data->m_dbh->conn()->default_fetch_type;
    stmt->active_query_string = stmt->query_string;
    stmt->dbh = data->m_dbh;
    stmt->lazy_object_ref.unset();

    setPDOErrorNone(stmt->error_code);

    // when we add support for varargs here, we only need to set the stmt if
    // the argument count is > 1
    int argc = _argv.size() + 1;
    Variant argv_variant = _argv;
    if (argc == 1 ||
        pdo_stmt_set_fetch_mode(
          stmt,
          0,
          tvCastToInt64(_argv.lookup(0)),
          Variant::attach(HHVM_FN(array_splice)(argv_variant, 1)).toArray()
        )) {
      /* now execute the statement */
      setPDOErrorNone(stmt->error_code);
      if (stmt->executer()) {
        int ok = 1;
        if (!stmt->executed) {
          if (stmt->dbh->conn()->alloc_own_columns) {
            ok = pdo_stmt_describe_columns(stmt);
          }
          stmt->executed = 1;
        }
        if (ok) {
          pdo_stmt_construct(stmt, ret, data->m_dbh->conn()->def_stmt_clsname,
                             data->m_dbh->def_stmt_ctor_args);
          return ret;
        }
      }
    }
    /* something broke */
    data->m_dbh->query_stmt = stmt.get();
    PDO_HANDLE_STMT_ERR(stmt);
  } else {
    PDO_HANDLE_DBH_ERR(data->m_dbh);
  }

  return false;
}

static Variant HHVM_METHOD(PDO, quote, const OptString& str,
                           int64_t paramtype /* = PDO_PARAM_STR */) {
  auto data = Native::data<PDOData>(this_);

  assertx(data->m_dbh->conn()->driver);
  setPDOErrorNone(data->m_dbh->conn()->error_code);
  data->m_dbh->query_stmt = nullptr;

  if (!data->m_dbh->conn()->support(PDOConnection::MethodQuoter)) {
    pdo_raise_impl_error(data->m_dbh, nullptr, "IM001",
                         "driver does not support quoting");
    return false;
  }

  OptString quoted;
  if (data->m_dbh->conn()->quoter(str, quoted, (PDOParamType)paramtype)) {
    return quoted;
  }
  PDO_HANDLE_DBH_ERR(data->m_dbh);
  return false;
}

static bool HHVM_METHOD(PDO, sqliteCreateFunction, const OptString& name,
                        const Variant& callback, int64_t argcount /* = -1 */) {
#ifdef ENABLE_EXTENSION_PDO_SQLITE
  auto data = Native::data<PDOData>(this_);

  auto res = dynamic_cast<PDOSqliteResource*>(data->m_dbh.get());
  if (res == nullptr) {
    return false;
  }
  return res->createFunction(name, callback, argcount);
#else
  raise_recoverable_error("PDO::sqliteCreateFunction not implemented");
  return false;
#endif
}

static bool HHVM_METHOD(PDO, sqliteCreateAggregate, const OptString& /*name*/,
                        const Variant& /*step*/, const Variant& /*final*/,
                        int64_t /*argcount*/ /* = -1 */) {
  raise_recoverable_error("PDO::sqliteCreateAggregate not implemented");
  return false;
}

static Variant HHVM_METHOD(PDO, __wakeup) {
  throw_pdo_exception(uninit_null(),
                      "You cannot serialize or unserialize PDO instances");
  return init_null();
}

static Variant HHVM_METHOD(PDO, __sleep) {
  throw_pdo_exception(uninit_null(),
                      "You cannot serialize or unserialize PDO instances");
  return init_null();
}

static Array HHVM_STATIC_METHOD(PDO, getAvailableDrivers) {
  return HHVM_FN(pdo_drivers)();
}

///////////////////////////////////////////////////////////////////////////////

static inline bool rewrite_name_to_position(sp_PDOStatement stmt,
                                            sp_PDOBoundParam param) {
  if (!stmt->bound_param_map.empty()) {
    /* rewriting :name to ? style.
     * We need to fixup the parameter numbers on the parameters.
     * If we find that a given named parameter has been used twice,
     * we will raise an error, as we can't be sure that it is safe
     * to bind multiple parameters onto the same zval in the underlying
     * driver */
    if (stmt->named_rewrite_template) {
      /* this is not an error here */
      return true;
    }
    if (param->name.empty()) {
      /* do the reverse; map the parameter number to the name */
      if (stmt->bound_param_map.exists(param->paramno)) {
        param->name = stmt->bound_param_map[param->paramno].toString();
        return true;
      }
      pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                           "parameter was not defined");
      return false;
    }

    int position = 0;
    for (ArrayIter iter(stmt->bound_param_map); iter; ++iter, ++position) {
      if (iter.second().toString() == param->name) {
        if (param->paramno >= 0) {
          pdo_raise_impl_error
            (stmt->dbh, stmt, "IM001",
             "PDO refuses to handle repeating the same :named parameter "
             "for multiple positions with this driver, as it might be "
             "unsafe to do so.  Consider using a separate name for each "
             "parameter instead");
          return true;
        }
        param->paramno = position;
        return true;
      }
    }
    pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                         "parameter was not defined");
    return false;
  }
  return true;
}

/* trigger callback hook for parameters */
static bool dispatch_param_event(sp_PDOStatement stmt,
                                 PDOParamEvent event_type) {
  if (!stmt->support(PDOStatement::MethodParamHook)) {
    return true;
  }
  for (ArrayIter iter(stmt->bound_params); iter; ++iter) {
    auto param = cast<PDOBoundParam>(iter.second());
    if (!stmt->paramHook(param.get(), event_type)) {
      return false;
    }
  }
  for (ArrayIter iter(stmt->bound_columns); iter; ++iter) {
    auto param = cast<PDOBoundParam>(iter.second());
    if (!stmt->paramHook(param.get(), event_type)) {
      return false;
    }
  }
  return true;
}

static void get_lazy_object(sp_PDOStatement stmt, Variant &ret) {
  if (stmt->lazy_object_ref.isNull()) {
    stmt->lazy_object_ref = stmt;
  }
  ret = stmt->lazy_object_ref;
}

static bool really_register_bound_param(sp_PDOBoundParam param,
                                        sp_PDOStatement stmt) {
  Array &hash = stmt->bound_params;

  if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_STR &&
      !param->parameter.isNull()) {
    param->parameter = param->parameter.toString();
  } else if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_INT &&
             param->parameter.isBoolean()) {
    param->parameter = param->parameter.toInt64();
  } else if (PDO_PARAM_TYPE(param->param_type) == PDO_PARAM_BOOL &&
             param->parameter.isInteger()) {
    param->parameter = param->parameter.toBoolean();
  }
  param->stmt = stmt.get();

  if (!param->name.empty() && param->name[0] != ':') {
    param->name = OptString(":") + param->name;
  }

  if (!rewrite_name_to_position(stmt, param)) {
    param->name.reset();
    return false;
  }

  /* ask the driver to perform any normalization it needs on the
   * parameter name.  Note that it is illegal for the driver to take
   * a reference to param, as it resides in transient storage only
   * at this time. */
  if (stmt->support(PDOStatement::MethodParamHook)) {
    if (!stmt->paramHook(param.get(), PDO_PARAM_EVT_NORMALIZE)) {
      param->name.reset();
      return false;
    }
  }

  /* delete any other parameter registered with this number.
   * If the parameter is named, it will be removed and correctly
   * disposed of by the hash_update call that follows */
  if (param->paramno >= 0) {
    hash.remove(param->paramno);
  }

  /* allocate storage for the parameter, keyed by its "canonical" name */
  if (!param->name.empty()) {
    hash.set(param->name, Variant(param));
  } else {
    hash.set(param->paramno, Variant(param));
  }

  /* tell the driver we just created a parameter */
  if (stmt->support(PDOStatement::MethodParamHook)) {
    if (!stmt->paramHook(param.get(), PDO_PARAM_EVT_ALLOC)) {
      /* undo storage allocation; the hash will free the parameter
       * name if required */
      if (!param->name.empty()) {
        hash.remove(param->name);
      } else {
        hash.remove(param->paramno);
      }
      param->parameter.unset();
      return false;
    }
  }
  return true;
}

static inline void fetch_value(sp_PDOStatement stmt, Variant &dest, int colno,
                               int *type_override) {
  if (colno < 0 || colno >= stmt->column_count) {
    return;
  }
  auto col = cast<PDOColumn>(stmt->columns[colno]);
  int type = PDO_PARAM_TYPE(col->param_type);
  int new_type = type_override ? PDO_PARAM_TYPE(*type_override) : type;

  stmt->getColumn(colno, dest);

  if (type != new_type) {
    switch (new_type) {
    case PDO_PARAM_INT:  dest = dest.toInt64();   break;
    case PDO_PARAM_BOOL: dest = dest.toBoolean(); break;
    case PDO_PARAM_STR:  dest = dest.toString();  break;
    case PDO_PARAM_NULL: dest = init_null();      break;
    }
  }
  if (stmt->dbh->conn()->stringify && (dest.isInteger() || dest.isDouble())) {
    dest = dest.toString();
  }
  if (dest.isNull() && stmt->dbh->conn()->oracle_nulls == PDO_NULL_TO_STRING) {
    dest = empty_string_variant();
  }
}

static bool do_fetch_common(sp_PDOStatement stmt, PDOFetchOrientation ori,
                            long offset) {
  if (!stmt->executed) {
    return false;
  }
  if (!dispatch_param_event(stmt, PDO_PARAM_EVT_FETCH_PRE)) {
    return false;
  }
  if (!stmt->fetcher(ori, offset)) {
    return false;
  }
  /* some drivers might need to describe the columns now */
  if (stmt->columns.empty() && !pdo_stmt_describe_columns(stmt)) {
    return false;
  }
  if (!dispatch_param_event(stmt, PDO_PARAM_EVT_FETCH_POST)) {
    return false;
  }

  if (!stmt->bound_columns.empty()) {
    /* update those bound column variables now */
    for (ArrayIter iter(stmt->bound_columns); iter; ++iter) {
      auto param = cast<PDOBoundParam>(iter.second());
      if (param->paramno >= 0) {
        param->parameter.setNull();
        /* set new value */
        fetch_value(stmt, param->parameter, param->paramno,
                    (int *)&param->param_type);
        /* TODO: some smart thing that avoids duplicating the value in the
         * general loop below.  For now, if you're binding output columns,
         * it's better to use LAZY or BOUND fetches if you want to shave
         * off those cycles */
      }
    }
  }

  return true;
}

static bool do_fetch_func_prepare(sp_PDOStatement stmt) {
  if (!is_callable(stmt->fetch.func)) {
    pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                         "user-supplied function must be a valid callback");
    return false;
  }
  return true;
}

/* perform a fetch.  If do_bind is true, update any bound columns.
 * If return_value is not null, store values into it according to HOW. */
static bool do_fetch(sp_PDOStatement stmt,
                     Variant& ret,
                     PDOFetchType how,
                     PDOFetchOrientation ori,
                     long offset,
                     Variant *return_all) {
  if (how == PDO_FETCH_USE_DEFAULT) {
    how = stmt->default_fetch_type;
  }
  int flags = how & PDO_FETCH_FLAGS;
  how = (PDOFetchType)(how & ~PDO_FETCH_FLAGS);

  if (!do_fetch_common(stmt, ori, offset)) {
    return false;
  }

  if (how == PDO_FETCH_BOUND) {
    ret = true;
    return true;
  }

  int colno;
  if ((flags & PDO_FETCH_GROUP) && stmt->fetch.column == -1) {
    colno = 1;
  } else {
    colno = stmt->fetch.column;
  }

  if (how == PDO_FETCH_LAZY) {
    get_lazy_object(stmt, ret);
    return true;
  }

  OptString clsname, old_clsname;
  Variant old_ctor_args;
  ret = false;
  int i = 0;
  switch (how) {
  case PDO_FETCH_USE_DEFAULT:
  case PDO_FETCH_ASSOC:
  case PDO_FETCH_BOTH:
  case PDO_FETCH_NUM:
  case PDO_FETCH_NAMED:
    ret = Array::CreateDict();
    break;

  case PDO_FETCH_KEY_PAIR:
    if (stmt->column_count != 2) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "PDO::FETCH_KEY_PAIR fetch mode requires the "
                           "result set to contain extactly 2 columns.");
      return false;
    }
    if (!return_all) {
      ret = Array::CreateDict();
    }
    break;

  case PDO_FETCH_COLUMN:
    if (colno >= 0 && colno < stmt->column_count) {
      if (flags == PDO_FETCH_GROUP && stmt->fetch.column == -1) {
        fetch_value(stmt, ret, 1, NULL);
      } else if (flags == PDO_FETCH_GROUP && colno) {
        fetch_value(stmt, ret, 0, NULL);
      } else {
        fetch_value(stmt, ret, colno, NULL);
      }
      if (!return_all) {
        return true;
      }
      break;
    } else {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000", "Invalid column index");
    }
    return false;

  case PDO_FETCH_OBJ:
    ret = SystemLib::AllocStdClassObject();
    break;

  case PDO_FETCH_CLASS:
    if (flags & PDO_FETCH_CLASSTYPE) {
      old_clsname = stmt->fetch.clsname;
      old_ctor_args = stmt->fetch.ctor_args;

      Variant val;
      fetch_value(stmt, val, i++, NULL);
      if (!val.isNull()) {
        if (!HHVM_FN(class_exists)(val.toString())) {
          stmt->fetch.clsname = "stdClass";
        } else {
          stmt->fetch.clsname = val.toString();
        }
      }

      do_fetch_class_prepare(stmt);
    }
    clsname = stmt->fetch.clsname;
    if (clsname.empty()) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "No fetch class specified");
      return false;
    }
    if ((flags & PDO_FETCH_SERIALIZE) == 0) {
      ret = create_object_only(clsname);
      if (!do_fetch_class_prepare(stmt)) {
        return false;
      }
      if (!stmt->fetch.constructor.empty() &&
          (flags & PDO_FETCH_PROPS_LATE)) {
        ret.asCObjRef()->o_invoke(stmt->fetch.constructor,
                                  stmt->fetch.ctor_args.toArray());
      }
    }
    break;

  case PDO_FETCH_INTO:
    if (stmt->fetch.into.isNull()) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "No fetch-into object specified.");
      return false;
    }

    ret = stmt->fetch.into;
    if (ret.isObject() &&
        ret.getObjectData()->instanceof(SystemLib::getstdClassClass())) {
      how = PDO_FETCH_OBJ;
    }
    break;

  case PDO_FETCH_FUNC:
    if (stmt->fetch.func.empty()) {
      pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                           "No fetch function specified");
      return false;
    }
    if (!do_fetch_func_prepare(stmt)) {
      return false;
    }
    break;

  default:
    assertx(false);
    return false;
  }

  Variant grp_val;
  if (return_all && how != PDO_FETCH_KEY_PAIR) {
    if (flags == PDO_FETCH_GROUP && how == PDO_FETCH_COLUMN &&
        stmt->fetch.column > 0) {
      fetch_value(stmt, grp_val, colno, NULL);
    } else {
      fetch_value(stmt, grp_val, i, NULL);
    }
    grp_val = grp_val.toString();
    if (how == PDO_FETCH_COLUMN) {
      i = stmt->column_count; /* no more data to fetch */
    } else {
      i++;
    }
  }

  for (int idx = 0; i < stmt->column_count; i++, idx++) {
    const OptString& name = cast<PDOColumn>(stmt->columns[i])->name;
    Variant val;
    fetch_value(stmt, val, i, NULL);

    switch (how) {
    case PDO_FETCH_ASSOC: {
      auto const name_key =
        ret.asArrRef().convertKey<IntishCast::Cast>(name);
      ret.asArrRef().set(name_key, *val.asTypedValue());
      break;
    }
    case PDO_FETCH_KEY_PAIR: {
      Variant tmp;
      fetch_value(stmt, tmp, ++i, NULL);
      if (return_all) {
        auto const val_key_ret =
          return_all->asArrRef().convertKey<IntishCast::Cast>(val);
        return_all->asArrRef().set(val_key_ret, *tmp.asTypedValue());
      } else {
        auto const val_key =
          ret.asArrRef().convertKey<IntishCast::Cast>(val);
        ret.asArrRef().set(val_key, *tmp.asTypedValue());
      }
      return true;
    }
    case PDO_FETCH_USE_DEFAULT:
    case PDO_FETCH_BOTH: {
      auto const name_key =
        ret.asArrRef().convertKey<IntishCast::Cast>(name);
      ret.asArrRef().set(name_key, *val.asTypedValue());
      ret.asArrRef().append(val);
      break;
    }

    case PDO_FETCH_NAMED: {
      auto const name_key =
        ret.asArrRef().convertKey<IntishCast::Cast>(name);
      /* already have an item with this name? */
      forceToDict(ret);
      if (ret.asArrRef().exists(name_key)) {
        auto const curr_val = ret.asArrRef().lval(name_key);
        if (!isArrayLikeType(curr_val.type())) {
          Array arr = Array::CreateVec();
          arr.append(curr_val.tv());
          arr.append(val);
          ret.toArray().set(name_key, make_array_like_tv(arr.get()));
        } else {
          asArrRef(curr_val).append(val);
        }
      } else {
        ret.asArrRef().set(name_key, *val.asTypedValue());
      }
      break;
    }
    case PDO_FETCH_NUM:
      ret.asArrRef().append(val);
      break;

    case PDO_FETCH_OBJ:
    case PDO_FETCH_INTO:
      ret.toObject()->o_set(name, val);
      break;

    case PDO_FETCH_CLASS:
      if ((flags & PDO_FETCH_SERIALIZE) == 0 || idx) {
        ret.toObject()->o_set(name, val);
      } else {
#ifdef MBO_0
        ret = unserialize_from_string(val);
        if (same(ret, false)) {
          pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                               "cannot unserialize data");
          return false;
        }
#endif
        // hzhao: not sure how we support class serialization
        pdo_raise_impl_error(stmt->dbh, stmt, "HY000",
                             "cannot unserialize class");
        return false;
      }
      break;

    case PDO_FETCH_FUNC:
      forceToDict(stmt->fetch.values).set(idx, val);
      break;

    default:
      pdo_raise_impl_error(stmt->dbh, stmt, "22003", "mode is out of range");
      return false;
    }
  }

  switch (how) {
  case PDO_FETCH_CLASS:
    if (!stmt->fetch.constructor.empty() &&
        !(flags & (PDO_FETCH_PROPS_LATE | PDO_FETCH_SERIALIZE))) {
      ret.toObject()->o_invoke(stmt->fetch.constructor,
                               stmt->fetch.ctor_args.toArray());
    }
    if (flags & PDO_FETCH_CLASSTYPE) {
      stmt->fetch.clsname = old_clsname;
      stmt->fetch.ctor_args = old_ctor_args;
    }
    break;

  case PDO_FETCH_FUNC:
    ret = vm_call_user_func(stmt->fetch.func,
                            stmt->fetch.values.toArray());
    break;

  default:
    break;
  }

  if (return_all) {
    auto const grp_key =
      return_all->asArrRef().convertKey<IntishCast::Cast>(grp_val);
    if ((flags & PDO_FETCH_UNIQUE) == PDO_FETCH_UNIQUE) {
      return_all->asArrRef().set(grp_key, *ret.asTypedValue());
    } else {
      auto const lval = return_all->asArrRef().lval(grp_key);
      forceToArray(lval).append(ret);
    }
  }

  return true;
}


bool HHVM_METHOD(PDOStatement, bindValue, const Variant& paramno,
                 const Variant& param,
                 int64_t type /* = PDO_PARAM_STR */) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }
  auto& stmt = data->m_stmt;

  auto p = req::make<PDOBoundParam>();
  // need to make sure this is NULL, in case a fatal errors occurs before it's
  // set inside really_register_bound_param
  p->stmt = NULL;

  if (paramno.isNumeric()) {
    p->paramno = paramno.toInt64();
  } else {
    p->paramno = -1;
    p->name = paramno.toString();
  }

  p->parameter = param;
  p->param_type = (PDOParamType)type;

  if (p->paramno > 0) {
    --p->paramno; /* make it zero-based internally */
  } else if (p->name.empty()) {
    pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                         "Columns/Parameters are 1-based");
    return false;
  }

  if (!really_register_bound_param(p, stmt)) {
    p->parameter.unset();
    return false;
  }
  return true;
}

static bool generic_stmt_attr_get(sp_PDOStatement stmt, Variant &ret,
                                  long attr) {
  if (attr == PDO_ATTR_EMULATE_PREPARES) {
    ret = (bool)(stmt->supports_placeholders == PDO_PLACEHOLDER_NONE);
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// SQL parser

namespace {

#define PDO_PARSER_TEXT 1
#define PDO_PARSER_BIND 2
#define PDO_PARSER_BIND_POS 3
#define PDO_PARSER_EOI 4

#define RET(i) {s->cur = cursor; return i; }
#define SKIP_ONE(i) {s->cur = s->tok + 1; return 1; }

#define YYCTYPE         unsigned char
#define YYCURSOR        cursor
#define YYLIMIT         limit
#define YYMARKER        s->ptr
#define YYFILL(n)       RET(PDO_PARSER_EOI)

struct Scanner {
  char *ptr, *cur, *lim, *tok;
};

static int scan(Scanner *s) {
  char* cursor = s->cur;
  char* limit = s->lim;
  s->tok = cursor;

{
  YYCTYPE yych;

  if ((YYLIMIT - YYCURSOR) < 2) { YYFILL(2); }
  yych = *YYCURSOR;
  switch (yych) {
  case 0x00:  goto yy11;
  case '"':  goto yy2;
  case '\'':  goto yy4;
  case ':':  goto yy5;
  case '?':  goto yy6;
  default:  goto yy8;
  }
yy2:
  yych = *(YYMARKER = ++YYCURSOR);
  if (yych >= 0x01) goto yy26;
yy3:
  { SKIP_ONE(PDO_PARSER_TEXT); }
yy4:
  yych = *(YYMARKER = ++YYCURSOR);
  if (yych <= 0x00) goto yy3;
  goto yy20;
yy5:
  yych = *++YYCURSOR;
  switch (yych) {
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':
  case '_':
  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':  goto yy16;
  case ':':
  case '?':  goto yy13;
  default:  goto yy3;
  }
yy6:
  ++YYCURSOR;
  switch ((yych = *YYCURSOR)) {
  case ':':
  case '?':  goto yy13;
  default:  goto yy7;
  }
yy7:
  { RET(PDO_PARSER_BIND_POS); }
yy8:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
  switch (yych) {
  case 0x00:
  case '"':
  case '\'':
  case ':':
  case '?':  goto yy10;
  default:  goto yy8;
  }
yy10:
  { RET(PDO_PARSER_TEXT); }
yy11:
  ++YYCURSOR;
  { RET(PDO_PARSER_EOI); }
yy13:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
  switch (yych) {
  case ':':
  case '?':  goto yy13;
  default:  goto yy15;
  }
yy15:
  { RET(PDO_PARSER_TEXT); }
yy16:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
  switch (yych) {
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':
  case '_':
  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':  goto yy16;
  default:  goto yy18;
  }
yy18:
  { RET(PDO_PARSER_BIND); }
yy19:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
yy20:
  switch (yych) {
  case 0x00:  goto yy21;
  case '\'':  goto yy23;
  case '\\':  goto yy22;
  default:  goto yy19;
  }
yy21:
  YYCURSOR = YYMARKER;
  goto yy3;
yy22:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
  if (yych <= 0x00) goto yy21;
  goto yy19;
yy23:
  ++YYCURSOR;
  { RET(PDO_PARSER_TEXT); }
yy25:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
yy26:
  switch (yych) {
  case 0x00:  goto yy21;
  case '"':  goto yy28;
  case '\\':  goto yy27;
  default:  goto yy25;
  }
yy27:
  ++YYCURSOR;
  if (YYLIMIT <= YYCURSOR) { YYFILL(1); }
  yych = *YYCURSOR;
  if (yych <= 0x00) goto yy21;
  goto yy25;
yy28:
  ++YYCURSOR;
  { RET(PDO_PARSER_TEXT); }
}

}

}

struct placeholder {
  char *pos;
  int len;
  int bindno;
  OptString quoted;  /* quoted value */
  struct placeholder *next;
};

int pdo_parse_params(sp_PDOStatement stmt, const OptString& in, OptString &out) {
  Scanner s;
  const char *ptr;
  char *newbuffer;
  int t;
  int bindno = 0;
  int ret = 0;
  int newbuffer_len;
  Array params;
  req::ptr<PDOBoundParam> param;
  int query_type = PDO_PLACEHOLDER_NONE;
  struct placeholder *placeholders = NULL, *placetail = NULL, *plc = NULL;

  s.cur = (char*)in.data();
  s.lim = (char*)in.data() + in.size() + 1;

  /* phase 1: look for args */
  while ((t = scan(&s)) != PDO_PARSER_EOI) {
    if (t == PDO_PARSER_BIND || t == PDO_PARSER_BIND_POS) {
      if (t == PDO_PARSER_BIND) {
        int len = s.cur - s.tok;
        if ((in.data() < (s.cur - len)) && isalnum(*(s.cur - len - 1))) {
          continue;
        }
        query_type |= PDO_PLACEHOLDER_NAMED;
      } else {
        query_type |= PDO_PLACEHOLDER_POSITIONAL;
      }

      plc = req::make_raw<placeholder>();
      memset(plc, 0, sizeof(*plc));
      plc->next = NULL;
      plc->pos = s.tok;
      plc->len = s.cur - s.tok;
      plc->bindno = bindno++;

      if (placetail) {
        placetail->next = plc;
      } else {
        placeholders = plc;
      }
      placetail = plc;
    }
  }

  if (bindno == 0) {
    /* nothing to do; good! */
    return 0;
  }

  /* did the query make sense to me? */
  if (query_type == (PDO_PLACEHOLDER_NAMED|PDO_PLACEHOLDER_POSITIONAL)) {
    /* they mixed both types; punt */
    pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                         "mixed named and positional parameters");
    ret = -1;
    goto clean_up;
  }

  if ((int)stmt->supports_placeholders == query_type &&
      !stmt->named_rewrite_template) {
    /* query matches native syntax */
    ret = 0;
    goto clean_up;
  }

  if (stmt->named_rewrite_template) {
    /* magic/hack.
     * We we pretend that the query was positional even if
     * it was named so that we fall into the
     * named rewrite case below.  Not too pretty,
     * but it works. */
    query_type = PDO_PLACEHOLDER_POSITIONAL;
  }

  params = stmt->bound_params;

  /* Do we have placeholders but no bound params */
  if (bindno && params.empty() &&
      stmt->supports_placeholders == PDO_PLACEHOLDER_NONE) {
    pdo_raise_impl_error(stmt->dbh, stmt, "HY093", "no parameters were bound");
    ret = -1;
    goto clean_up;
  }

  if (!params.empty() && bindno != params.size() &&
      stmt->supports_placeholders == PDO_PLACEHOLDER_NONE) {
    /* extra bit of validation for instances when same params are bound
       more then once */
    if (query_type != PDO_PLACEHOLDER_POSITIONAL && bindno > params.size()) {
      int ok = 1;
      for (plc = placeholders; plc; plc = plc->next) {
        if (!params.exists(OptString(plc->pos, plc->len, CopyString))) {
          ok = 0;
          break;
        }
      }
      if (ok) {
        goto safe;
      }
    }
    pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                         "number of bound variables does not match number "
                         "of tokens");
    ret = -1;
    goto clean_up;
  }
safe:
  /* what are we going to do ? */
  if (stmt->supports_placeholders == PDO_PLACEHOLDER_NONE) {
    /* query generation */

    newbuffer_len = in.size();

    /* let's quote all the values */
    for (plc = placeholders; plc; plc = plc->next) {
      Variant vparam;
      if (query_type == PDO_PLACEHOLDER_POSITIONAL) {
        vparam = params[plc->bindno];
      } else {
        OptString str(plc->pos, plc->len, CopyString);
        auto const arrkey = params.convertKey<IntishCast::Cast>(str);
        vparam = params[arrkey];
      }
      if (vparam.isNull()) {
        /* parameter was not defined */
        ret = -1;
        pdo_raise_impl_error(stmt->dbh, stmt, "HY093",
                             "parameter was not defined");
        goto clean_up;
      }
      param = cast<PDOBoundParam>(vparam);
      if (stmt->dbh->conn()->support(PDOConnection::MethodQuoter)) {
        if (param->param_type == PDO_PARAM_LOB &&
            param->parameter.isResource()) {
          Variant buf = HHVM_FN(stream_get_contents)(
                        param->parameter.toResource());
          if (!same(buf, false)) {
            if (!stmt->dbh->conn()->quoter(buf.toString(), plc->quoted,
                                   param->param_type)) {
              /* bork */
              ret = -1;
              setPDOError(stmt->error_code, stmt->dbh->conn()->error_code);
              goto clean_up;
            }
          } else {
            pdo_raise_impl_error(stmt->dbh, stmt, "HY105",
                                 "Expected a stream resource");
            ret = -1;
            goto clean_up;
          }
        } else {
          do {
            switch (param->parameter.getType()) {
              case KindOfUninit:
              case KindOfNull:
                plc->quoted = "NULL";
                continue;

              case KindOfInt64:
              case KindOfDouble:
                plc->quoted = param->parameter.toString();
                continue;

              case KindOfBoolean:
                param->parameter = param->parameter.toInt64();
                [[fallthrough]];
              case KindOfString:
              case KindOfVec:
              case KindOfDict:
              case KindOfKeyset:
              case KindOfObject:
              case KindOfResource:
              case KindOfRFunc:
              case KindOfFunc:
              case KindOfClass:
              case KindOfLazyClass:
              case KindOfClsMeth:
              case KindOfRClsMeth:
              case KindOfEnumClassLabel:
                if (!stmt->dbh->conn()->quoter(
                      param->parameter.toString(),
                      plc->quoted,
                      param->param_type)) {
                  /* bork */
                  ret = -1;
                  setPDOError(stmt->error_code, stmt->dbh->conn()->error_code);
                  goto clean_up;
                }
                continue;
            }
            not_reached();
          } while (0);
        }
      } else {
        plc->quoted = param->parameter.toString();
      }
      newbuffer_len += plc->quoted.size();
    }

rewrite:
    /* allocate output buffer */
    out = OptString(newbuffer_len, ReserveString);
    newbuffer = out.mutableData();

    /* and build the query */
    plc = placeholders;
    ptr = in.data();

    do {
      t = plc->pos - ptr;
      if (t) {
        memcpy(newbuffer, ptr, t);
        newbuffer += t;
      }
      memcpy(newbuffer, plc->quoted.data(), plc->quoted.size());
      newbuffer += plc->quoted.size();
      ptr = plc->pos + plc->len;

      plc = plc->next;
    } while (plc);

    t = (in.data() + in.size()) - ptr;
    if (t) {
      memcpy(newbuffer, ptr, t);
      newbuffer += t;
    }
    out.setSize(newbuffer - out.data());

    ret = 1;
    goto clean_up;

  } else if (query_type == PDO_PLACEHOLDER_POSITIONAL) {
    /* rewrite ? to :pdoX */
    StringBuffer idxbuf;
    const char *tmpl = stmt->named_rewrite_template ?
      stmt->named_rewrite_template : ":pdo%d";
    int bind_no = 1;

    newbuffer_len = in.size();

    for (plc = placeholders; plc; plc = plc->next) {
      int skip_map = 0;
      OptString name(plc->pos, plc->len, CopyString);
      auto const name_key =
        stmt->bound_param_map.convertKey<IntishCast::Cast>(name);

      /* check if bound parameter is already available */
      if (!strcmp(name.c_str(), "?") ||
          !stmt->bound_param_map.exists(name_key)) {
        idxbuf.printf(tmpl, bind_no++);
      } else {
        idxbuf.clear();
        idxbuf.append(stmt->bound_param_map[name_key].toString());
        skip_map = 1;
      }

      plc->quoted = idxbuf.detach();
      newbuffer_len += plc->quoted.size();

      if (!skip_map && stmt->named_rewrite_template) {
        /* create a mapping */
        stmt->bound_param_map.set(name_key,
                                  make_tv<KindOfString>(plc->quoted.get()));
      }

      /* map number to name */
      stmt->bound_param_map.set(plc->bindno, plc->quoted);
    }

    goto rewrite;

  } else {
    /* rewrite :name to ? */

    newbuffer_len = in.size();

    for (plc = placeholders; plc; plc = plc->next) {
      OptString name(plc->pos, plc->len, CopyString);
      stmt->bound_param_map.set(plc->bindno, name);
      plc->quoted = "?";
    }

    goto rewrite;
  }

clean_up:

  while (placeholders) {
    plc = placeholders;
    placeholders = plc->next;
    plc->quoted.reset();
    req::free(plc);
  }

  return ret;
}

///////////////////////////////////////////////////////////////////////////////
// PDOStatement

const StaticString s_PDOStatement("PDOStatement");

PDOStatementData::PDOStatementData() : m_rowIndex(-1) {
}

PDOStatementData::~PDOStatementData() { }

Variant HHVM_METHOD(PDOStatement, execute,
                    const Variant& paramsV /* = null_array */) {
  auto data = Native::data<PDOStatementData>(this_);
  auto params = paramsV.isNull() ? null_array : paramsV.toArray();

  SYNC_VM_REGS_SCOPED();

  if (data->m_stmt == nullptr) {
    return init_null_variant;
  }

  setPDOErrorNone(data->m_stmt->error_code);

  if (!params.empty()) {
    data->m_stmt->bound_params.reset();
    for (ArrayIter iter(params); iter; ++iter) {
      auto param = req::make<PDOBoundParam>();
      param->param_type = PDO_PARAM_STR;
      param->parameter = iter.second();
      param->stmt = NULL;

      if (iter.first().isString()) {
        param->name = iter.first().toString();
        param->paramno = -1;
      } else {
        int64_t num_index = iter.first().toInt64();
        /* we're okay to be zero based here */
        if (num_index < 0) {
          pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt,
                               "HY093", nullptr);
          return false;
        }
        param->paramno = num_index;
      }

      if (!really_register_bound_param(param, data->m_stmt)) {
        return false;
      }
    }
  }

  int ret = 1;
  if (PDO_PLACEHOLDER_NONE == data->m_stmt->supports_placeholders) {
    /* handle the emulated parameter binding, m_stmt->active_query_string
       holds the query with binds expanded and quoted. */
    ret = pdo_parse_params(data->m_stmt, data->m_stmt->query_string,
                           data->m_stmt->active_query_string);
    if (ret == 0) { /* no changes were made */
      data->m_stmt->active_query_string = data->m_stmt->query_string;
      ret = 1;
    } else if (ret == -1) {
      /* something broke */
      PDO_HANDLE_STMT_ERR(data->m_stmt);
      return false;
    }
  } else if (!dispatch_param_event(data->m_stmt, PDO_PARAM_EVT_EXEC_PRE)) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }
  if (data->m_stmt->executer()) {
    data->m_stmt->active_query_string.reset();
    if (!data->m_stmt->executed) {
      /* this is the first execute */

      if (data->m_stmt->dbh->conn()->alloc_own_columns
          && data->m_stmt->columns.empty()) {
        /* for "big boy" drivers, we need to allocate memory to fetch
         * the results into, so lets do that now */
        ret = pdo_stmt_describe_columns(data->m_stmt);
      }

      data->m_stmt->executed = 1;
    }

    if (ret && !dispatch_param_event(data->m_stmt, PDO_PARAM_EVT_EXEC_POST)) {
      return false;
    }

    return (bool)ret;
  }
  data->m_stmt->active_query_string.reset();
  PDO_HANDLE_STMT_ERR(data->m_stmt);
  return false;
}

static Variant HHVM_METHOD(PDOStatement, fetch, int64_t how  = 0,
                           int64_t orientation = PDO_FETCH_ORI_NEXT,
                           int64_t offset = 0) {
  auto data = Native::data<PDOStatementData>(this_);

  SYNC_VM_REGS_SCOPED();

  if (data->m_stmt == nullptr) {
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  if (!pdo_stmt_verify_mode(data->m_stmt, how, false)) {
    return false;
  }

  Variant ret;
  if (!do_fetch(data->m_stmt, ret, (PDOFetchType)how,
                (PDOFetchOrientation)orientation, offset, NULL)) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }
  return ret;
}

static Variant HHVM_METHOD(PDOStatement, fetchObject,
                           const OptString& class_name /* = null_string */,
                           const Variant& ctor_args /* = null */) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  if (!pdo_stmt_verify_mode(data->m_stmt, PDO_FETCH_CLASS, false)) {
    return false;
  }

  OptString old_clsname = data->m_stmt->fetch.clsname;
  Variant old_ctor_args = data->m_stmt->fetch.ctor_args;
  bool error = false;

  data->m_stmt->fetch.clsname = class_name;
  if (class_name.empty()) {
    data->m_stmt->fetch.clsname = "stdClass";
  }
  if (!HHVM_FN(class_exists)(data->m_stmt->fetch.clsname)) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "HY000",
                         "Could not find user-supplied class");
    error = true;
  }
  if (!ctor_args.isNull() && !ctor_args.isArray()) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "HY000",
                         "ctor_args must be either NULL or an array");
    error = true;
  }
  data->m_stmt->fetch.ctor_args = ctor_args;

  Variant ret;
  if (!error && !do_fetch(data->m_stmt, ret, PDO_FETCH_CLASS,
                          PDO_FETCH_ORI_NEXT, 0, NULL)) {
    error = true;
  }
  if (error) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
  }

  data->m_stmt->fetch.clsname = old_clsname;
  data->m_stmt->fetch.ctor_args = old_ctor_args;
  if (error) {
    return false;
  }
  return ret;
}

static Variant HHVM_METHOD(PDOStatement, fetchColumn,
                           int64_t column_numner /* = 0 */) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  if (!do_fetch_common(data->m_stmt, PDO_FETCH_ORI_NEXT, 0)) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }
  Variant ret;
  fetch_value(data->m_stmt, ret, column_numner, nullptr);
  return ret;
}

Variant HHVM_METHOD(PDOStatement, fetchAll, int64_t how /* = 0 */,
                    const Variant& class_name /* = null */,
                    const Variant& ctor_args /* = null */) {
  auto self = Native::data<PDOStatementData>(this_);
  if (self->m_stmt == nullptr) {
    return false;
  }

  if (!pdo_stmt_verify_mode(self->m_stmt, how, true)) {
    return false;
  }

  OptString old_clsname = self->m_stmt->fetch.clsname;
  Variant old_ctor_args = self->m_stmt->fetch.ctor_args;
  int error = 0;

  switch (how & ~PDO_FETCH_FLAGS) {
  case PDO_FETCH_CLASS:
    self->m_stmt->fetch.clsname = class_name.toString();
    if (class_name.isNull()) {
      self->m_stmt->fetch.clsname = "stdClass";
    }
    if (!HHVM_FN(class_exists)(self->m_stmt->fetch.clsname)) {
      pdo_raise_impl_error(self->m_stmt->dbh, self->m_stmt, "HY000",
                           "Could not find user-supplied class");
      error = 1;
    }
    if (!ctor_args.isNull() && !ctor_args.isArray()) {
      pdo_raise_impl_error(self->m_stmt->dbh, self->m_stmt, "HY000",
                           "ctor_args must be either NULL or an array");
      error = 1;
      break;
    }
    self->m_stmt->fetch.ctor_args = ctor_args;

    if (!error) {
      do_fetch_class_prepare(self->m_stmt);
    }
    break;

  case PDO_FETCH_FUNC:
    if (!HHVM_FN(function_exists)(class_name.toString())) {
      pdo_raise_impl_error(self->m_stmt->dbh, self->m_stmt, "HY000",
                           "no fetch function specified");
      error = 1;
    } else {
      self->m_stmt->fetch.func = class_name.toString();
      do_fetch_func_prepare(self->m_stmt);
    }
    break;

  case PDO_FETCH_COLUMN:
    if (class_name.isNull()) {
      self->m_stmt->fetch.column = how & PDO_FETCH_GROUP ? -1 : 0;
    } else {
      self->m_stmt->fetch.column = class_name.toInt64();
    }
    if (!ctor_args.isNull()) {
      pdo_raise_impl_error(self->m_stmt->dbh, self->m_stmt, "HY000",
                           "Third parameter not allowed for "
                           "PDO::FETCH_COLUMN");
      error = 1;
    }
    break;
  }

  int flags = how & PDO_FETCH_FLAGS;

  if ((how & ~PDO_FETCH_FLAGS) == PDO_FETCH_USE_DEFAULT) {
    flags |= self->m_stmt->default_fetch_type & PDO_FETCH_FLAGS;
    how |= self->m_stmt->default_fetch_type & ~PDO_FETCH_FLAGS;
  }

  Variant *return_all = NULL;
  Variant return_value;
  Variant data;
  if (!error)  {
    setPDOErrorNone(self->m_stmt->error_code);

    if ((how & PDO_FETCH_GROUP) || how == PDO_FETCH_KEY_PAIR ||
        (how == PDO_FETCH_USE_DEFAULT &&
         self->m_stmt->default_fetch_type == PDO_FETCH_KEY_PAIR)) {
      return_value = Array::CreateDict();
      return_all = &return_value;
    }
    if (!do_fetch(self->m_stmt, data, (PDOFetchType)(how | flags),
                  PDO_FETCH_ORI_NEXT, 0, return_all)) {
      error = 2;
    }
  }
  if (!error) {
    if ((how & PDO_FETCH_GROUP)) {
      do {
        data.unset();
      } while (do_fetch(self->m_stmt, data, (PDOFetchType)(how | flags),
                        PDO_FETCH_ORI_NEXT, 0, return_all));
    } else if (how == PDO_FETCH_KEY_PAIR ||
               (how == PDO_FETCH_USE_DEFAULT &&
                self->m_stmt->default_fetch_type == PDO_FETCH_KEY_PAIR)) {
      while (do_fetch(self->m_stmt, data, (PDOFetchType)(how | flags),
                      PDO_FETCH_ORI_NEXT, 0, return_all)) {
        continue;
      }
    } else {
      return_value = Array::CreateVec();
      do {
        return_value.asArrRef().append(data);
        data.unset();
      } while (do_fetch(self->m_stmt,  data, (PDOFetchType)(how | flags),
                        PDO_FETCH_ORI_NEXT, 0, NULL));
    }
  }

  self->m_stmt->fetch.clsname = old_clsname;
  self->m_stmt->fetch.ctor_args = old_ctor_args;

  if (error) {
    PDO_HANDLE_STMT_ERR(self->m_stmt);
    if (error != 2) {
      return false;
    }

    /* on no results, return an empty array */
    if (!return_value.isArray()) {
      return_value = Array::CreateDict();
    }
  }
  return return_value;
}

static int64_t HHVM_METHOD(PDOStatement, rowCount) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return 0;
  }

  return data->m_stmt->row_count;
}

static Variant HHVM_METHOD(PDOStatement, errorCode) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }
  if (data->m_stmt->error_code[0] == '\0') {
    return init_null();
  }
  return OptString(data->m_stmt->error_code, CopyString);
}

static Array HHVM_METHOD(PDOStatement, errorInfo) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return null_array;
  }

  Array ret = Array::CreateVec();
  ret.append(OptString(data->m_stmt->error_code, CopyString));

  if (data->m_stmt->dbh->conn()->support(PDOConnection::MethodFetchErr)) {
    data->m_stmt->dbh->conn()->fetchErr(data->m_stmt.get(), ret);
  }

  int error_count = ret.size();
  int error_expected_count = 3;
  if (error_expected_count > error_count) {
    int error_count_diff = error_expected_count - error_count;
    for (int i = 0; i < error_count_diff; i++) {
      ret.append(uninit_null());
    }
  }
  return ret;
}

static Variant HHVM_METHOD(PDOStatement, setAttribute, int64_t attribute,
                           const Variant& value) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  if (!data->m_stmt->support(PDOStatement::MethodSetAttribute)) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "IM001",
                         "This driver doesn't support setting attributes");
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  if (data->m_stmt->setAttribute(attribute, value)) {
    return true;
  }
  PDO_HANDLE_STMT_ERR(data->m_stmt);
  return false;
}

static Variant HHVM_METHOD(PDOStatement, getAttribute, int64_t attribute) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  Variant ret;
  if (!data->m_stmt->support(PDOStatement::MethodGetAttribute)) {
    if (!generic_stmt_attr_get(data->m_stmt, ret, attribute)) {
      pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "IM001",
                           "This driver doesn't support getting attributes");
      return false;
    }
    return ret;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  switch (data->m_stmt->getAttribute(attribute, ret)) {
  case -1:
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  case 0:
    if (!generic_stmt_attr_get(data->m_stmt, ret, attribute)) {
      /* XXX: should do something better here */
      pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "IM001",
                           "driver doesn't support getting that attribute");
      return false;
    }
    break;
  default:
    break;
  }
  return ret;
}

static int64_t HHVM_METHOD(PDOStatement, columnCount) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return 0;
  }

  return data->m_stmt->column_count;
}

const StaticString
  s_name("name"),
  s_len("len"),
  s_precision("precision"),
  s_pdo_type("pdo_type");

static Variant HHVM_METHOD(PDOStatement, getColumnMeta, int64_t column) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  if (column < 0) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "42P10",
                         "column number must be non-negative");
    return false;
  }

  if (!data->m_stmt->support(PDOStatement::MethodGetColumnMeta)) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "IM001",
                         "driver doesn't support meta data");
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  auto ret = Array::CreateDict();
  if (!data->m_stmt->getColumnMeta(column, ret)) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }

  /* add stock items */
  auto col = cast<PDOColumn>(data->m_stmt->columns[column]);
  ret.set(s_name, col->name);
  ret.set(s_len, (int64_t)col->maxlen); /* FIXME: unsigned ? */
  ret.set(s_precision, (int64_t)col->precision);
  if (col->param_type != PDO_PARAM_ZVAL) {
    // if param_type is PDO_PARAM_ZVAL the driver has to provide correct data
    ret.set(s_pdo_type, (int64_t)col->param_type);
  }
  return ret;
}

static bool HHVM_METHOD(PDOStatement, setFetchMode,
                        int64_t mode, const Array& _argv /* = null_array */) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }
  int argc = _argv.size() + 1;

  return pdo_stmt_set_fetch_mode(data->m_stmt, argc, mode, _argv);
}

static bool HHVM_METHOD(PDOStatement, nextRowset) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  if (!data->m_stmt->support(PDOStatement::MethodNextRowset)) {
    pdo_raise_impl_error(data->m_stmt->dbh, data->m_stmt, "IM001",
                         "driver does not support multiple rowsets");
    return false;
  }

  setPDOErrorNone(data->m_stmt->error_code);

  /* un-describe */
  if (!data->m_stmt->columns.empty()) {
    data->m_stmt->columns.clear();
    data->m_stmt->column_count = 0;
  }

  if (!data->m_stmt->nextRowset()) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }

  pdo_stmt_describe_columns(data->m_stmt);
  return true;
}

static bool HHVM_METHOD(PDOStatement, closeCursor) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  if (!data->m_stmt->support(PDOStatement::MethodCursorCloser)) {
    /* emulate it by fetching and discarding rows */
    do {
      while (data->m_stmt->fetcher(PDO_FETCH_ORI_NEXT, 0));
      // if (!data->t_nextrowset()) {
      if (HHVM_MN(PDOStatement, nextRowset)(this_)) {
        break;
      }
    } while (true);
    data->m_stmt->executed = 0;
    return true;
  }

  setPDOErrorNone(data->m_stmt->error_code);
  if (!data->m_stmt->cursorCloser()) {
    PDO_HANDLE_STMT_ERR(data->m_stmt);
    return false;
  }
  data->m_stmt->executed = 0;
  return true;
}

static Variant HHVM_METHOD(PDOStatement, debugDumpParams) {
  auto data = Native::data<PDOStatementData>(this_);
  if (data->m_stmt == nullptr) {
    return false;
  }

  auto f = File::Open("php://output", "w");
  if (!f || f->isInvalid()) {
    return false;
  }

  f->printf(
    "SQL: [%d] %.*s\n",
    make_vec_array(
      data->m_stmt->query_string.size(),
      data->m_stmt->query_string.size(),
      data->m_stmt->query_string.data()
    )
  );

  f->printf("Params:  %d\n",
            make_vec_array(data->m_stmt->bound_params.size()));
  for (ArrayIter iter(data->m_stmt->bound_params); iter; ++iter) {
    if (iter.first().isString()) {
      OptString key = iter.first().toString();
      f->printf(
        "Key: Name: [%d] %.*s\n",
        make_vec_array(key.size(), key.size(), key.data())
      );
    } else {
      f->printf("Key: Position #%ld:\n",
                make_vec_array(iter.first().toInt64()));
    }

    auto param = cast<PDOBoundParam>(iter.second());
    f->printf(
      "paramno=%d\nname=[%d] \"%.*s\"\nparam_type=%d\n",
      make_vec_array(
        param->paramno,
        param->name.size(),
        param->name.size(),
        param->name.data(),
        param->param_type
      )
    );
  }
  return true;
}

static Variant HHVM_METHOD(PDOStatement, current) {
  auto data = Native::data<PDOStatementData>(this_);

  return data->m_row;
}

static Variant HHVM_METHOD(PDOStatement, key) {
  auto data = Native::data<PDOStatementData>(this_);

  return data->m_rowIndex;
}

static Variant HHVM_METHOD(PDOStatement, next) {
  auto data = Native::data<PDOStatementData>(this_);

  data->m_row = HHVM_MN(PDOStatement, fetch)(this_, PDO_FETCH_USE_DEFAULT);
  if (same(data->m_row, false)) {
    data->m_rowIndex = -1;
  } else {
    ++data->m_rowIndex;
  }
  return init_null();
}

static Variant HHVM_METHOD(PDOStatement, rewind) {
  auto data = Native::data<PDOStatementData>(this_);

  data->m_rowIndex = -1;
  HHVM_MN(PDOStatement, next)(this_);
  return init_null();
}

static bool HHVM_METHOD(PDOStatement, valid) {
  auto data = Native::data<PDOStatementData>(this_);

  return data->m_rowIndex >= 0;
}

static Variant HHVM_METHOD(PDOStatement, __wakeup) {
  throw_pdo_exception(uninit_null(),
                      "You cannot serialize or unserialize "
                      "PDOStatement instances");
  return init_null();
}

static Variant HHVM_METHOD(PDOStatement, __sleep) {
  throw_pdo_exception(uninit_null(),
                      "You cannot serialize or unserialize "
                      "PDOStatement instances");
  return init_null();
}

///////////////////////////////////////////////////////////////////////////////


static struct PDOExtension final : Extension {
  PDOExtension() : Extension("pdo", " 1.0.4dev", "hphp_hphpi") {}

#ifdef ENABLE_EXTENSION_PDO_MYSQL
  std::string mysql_default_socket;

  void moduleLoad(const IniSetting::Map& /*ini*/, Hdf /*config*/) override {
    IniSetting::Bind(this, IniSetting::Mode::Config,
                     "pdo_mysql.default_socket", nullptr,
                     &mysql_default_socket);
  }
#endif

  void moduleRegisterNative() override {
    HHVM_FE(pdo_drivers);
    HHVM_ME(PDO, __construct);
    HHVM_ME(PDO, prepare);
    HHVM_ME(PDO, beginTransaction);
    HHVM_ME(PDO, commit);
    HHVM_ME(PDO, inTransaction);
    HHVM_ME(PDO, rollBack);
    HHVM_ME(PDO, setAttribute);
    HHVM_ME(PDO, getAttribute);
    HHVM_ME(PDO, exec);
    HHVM_ME(PDO, lastInsertId);
    HHVM_ME(PDO, errorCode);
    HHVM_ME(PDO, errorInfo);
    HHVM_ME(PDO, query);
    HHVM_ME(PDO, quote);
    HHVM_ME(PDO, sqliteCreateFunction);
    HHVM_ME(PDO, sqliteCreateAggregate);
    HHVM_ME(PDO, __wakeup);
    HHVM_ME(PDO, __sleep);
    HHVM_STATIC_ME(PDO, getAvailableDrivers);
    HHVM_ME(PDOStatement, execute);
    HHVM_ME(PDOStatement, fetch);
    HHVM_ME(PDOStatement, fetchObject);
    HHVM_ME(PDOStatement, fetchColumn);
    HHVM_ME(PDOStatement, fetchAll);
    HHVM_ME(PDOStatement, bindValue);
    HHVM_ME(PDOStatement, rowCount);
    HHVM_ME(PDOStatement, errorCode);
    HHVM_ME(PDOStatement, errorInfo);
    HHVM_ME(PDOStatement, setAttribute);
    HHVM_ME(PDOStatement, getAttribute);
    HHVM_ME(PDOStatement, columnCount);
    HHVM_ME(PDOStatement, getColumnMeta);
    HHVM_ME(PDOStatement, setFetchMode);
    HHVM_ME(PDOStatement, nextRowset);
    HHVM_ME(PDOStatement, closeCursor);
    HHVM_ME(PDOStatement, debugDumpParams);
    HHVM_ME(PDOStatement, current);
    HHVM_ME(PDOStatement, key);
    HHVM_ME(PDOStatement, next);
    HHVM_ME(PDOStatement, rewind);
    HHVM_ME(PDOStatement, valid);
    HHVM_ME(PDOStatement, __wakeup);
    HHVM_ME(PDOStatement, __sleep);

    HHVM_RCC_INT(PDO, PARAM_BOOL, PDO_PARAM_BOOL);
    HHVM_RCC_INT(PDO, PARAM_NULL, PDO_PARAM_NULL);
    HHVM_RCC_INT(PDO, PARAM_INT, PDO_PARAM_INT);
    HHVM_RCC_INT(PDO, PARAM_STR, PDO_PARAM_STR);
    HHVM_RCC_INT(PDO, PARAM_LOB, PDO_PARAM_LOB);
    HHVM_RCC_INT(PDO, PARAM_STMT, PDO_PARAM_STMT);
    HHVM_RCC_INT(PDO, PARAM_INPUT_OUTPUT, PDO_PARAM_INPUT_OUTPUT);
    HHVM_RCC_INT(PDO, PARAM_EVT_ALLOC, PDO_PARAM_EVT_ALLOC);
    HHVM_RCC_INT(PDO, PARAM_EVT_FREE, PDO_PARAM_EVT_FREE);
    HHVM_RCC_INT(PDO, PARAM_EVT_EXEC_PRE, PDO_PARAM_EVT_EXEC_PRE);
    HHVM_RCC_INT(PDO, PARAM_EVT_EXEC_POST, PDO_PARAM_EVT_EXEC_POST);
    HHVM_RCC_INT(PDO, PARAM_EVT_FETCH_PRE, PDO_PARAM_EVT_FETCH_PRE);
    HHVM_RCC_INT(PDO, PARAM_EVT_FETCH_POST, PDO_PARAM_EVT_FETCH_POST);
    HHVM_RCC_INT(PDO, PARAM_EVT_NORMALIZE, PDO_PARAM_EVT_NORMALIZE);
    HHVM_RCC_INT(PDO, FETCH_USE_DEFAULT, PDO_FETCH_USE_DEFAULT);
    HHVM_RCC_INT(PDO, FETCH_LAZY, PDO_FETCH_LAZY);
    HHVM_RCC_INT(PDO, FETCH_ASSOC, PDO_FETCH_ASSOC);
    HHVM_RCC_INT(PDO, FETCH_NUM, PDO_FETCH_NUM);
    HHVM_RCC_INT(PDO, FETCH_BOTH, PDO_FETCH_BOTH);
    HHVM_RCC_INT(PDO, FETCH_OBJ, PDO_FETCH_OBJ);
    HHVM_RCC_INT(PDO, FETCH_BOUND, PDO_FETCH_BOUND);
    HHVM_RCC_INT(PDO, FETCH_COLUMN, PDO_FETCH_COLUMN);
    HHVM_RCC_INT(PDO, FETCH_CLASS, PDO_FETCH_CLASS);
    HHVM_RCC_INT(PDO, FETCH_INTO, PDO_FETCH_INTO);
    HHVM_RCC_INT(PDO, FETCH_FUNC, PDO_FETCH_FUNC);
    HHVM_RCC_INT(PDO, FETCH_GROUP, PDO_FETCH_GROUP);
    HHVM_RCC_INT(PDO, FETCH_UNIQUE, PDO_FETCH_UNIQUE);
    HHVM_RCC_INT(PDO, FETCH_KEY_PAIR, PDO_FETCH_KEY_PAIR);
    HHVM_RCC_INT(PDO, FETCH_CLASSTYPE, PDO_FETCH_CLASSTYPE);
    HHVM_RCC_INT(PDO, FETCH_SERIALIZE, PDO_FETCH_SERIALIZE);
    HHVM_RCC_INT(PDO, FETCH_PROPS_LATE, PDO_FETCH_PROPS_LATE);
    HHVM_RCC_INT(PDO, FETCH_NAMED, PDO_FETCH_NAMED);
    HHVM_RCC_INT(PDO, ATTR_AUTOCOMMIT, PDO_ATTR_AUTOCOMMIT);
    HHVM_RCC_INT(PDO, ATTR_PREFETCH, PDO_ATTR_PREFETCH);
    HHVM_RCC_INT(PDO, ATTR_TIMEOUT, PDO_ATTR_TIMEOUT);
    HHVM_RCC_INT(PDO, ATTR_ERRMODE, PDO_ATTR_ERRMODE);
    HHVM_RCC_INT(PDO, ATTR_SERVER_VERSION, PDO_ATTR_SERVER_VERSION);
    HHVM_RCC_INT(PDO, ATTR_CLIENT_VERSION, PDO_ATTR_CLIENT_VERSION);
    HHVM_RCC_INT(PDO, ATTR_SERVER_INFO, PDO_ATTR_SERVER_INFO);
    HHVM_RCC_INT(PDO, ATTR_CONNECTION_STATUS, PDO_ATTR_CONNECTION_STATUS);
    HHVM_RCC_INT(PDO, ATTR_CASE, PDO_ATTR_CASE);
    HHVM_RCC_INT(PDO, ATTR_CURSOR_NAME, PDO_ATTR_CURSOR_NAME);
    HHVM_RCC_INT(PDO, ATTR_CURSOR, PDO_ATTR_CURSOR);
    HHVM_RCC_INT(PDO, ATTR_ORACLE_NULLS, PDO_ATTR_ORACLE_NULLS);
    HHVM_RCC_INT(PDO, ATTR_PERSISTENT, PDO_ATTR_PERSISTENT);
    HHVM_RCC_INT(PDO, ATTR_STATEMENT_CLASS, PDO_ATTR_STATEMENT_CLASS);
    HHVM_RCC_INT(PDO, ATTR_FETCH_TABLE_NAMES, PDO_ATTR_FETCH_TABLE_NAMES);
    HHVM_RCC_INT(PDO, ATTR_FETCH_CATALOG_NAMES, PDO_ATTR_FETCH_CATALOG_NAMES);
    HHVM_RCC_INT(PDO, ATTR_DRIVER_NAME, PDO_ATTR_DRIVER_NAME);
    HHVM_RCC_INT(PDO, ATTR_STRINGIFY_FETCHES, PDO_ATTR_STRINGIFY_FETCHES);
    HHVM_RCC_INT(PDO, ATTR_MAX_COLUMN_LEN, PDO_ATTR_MAX_COLUMN_LEN);
    HHVM_RCC_INT(PDO, ATTR_EMULATE_PREPARES, PDO_ATTR_EMULATE_PREPARES);
    HHVM_RCC_INT(PDO, ATTR_DEFAULT_FETCH_MODE, PDO_ATTR_DEFAULT_FETCH_MODE);
    HHVM_RCC_INT(PDO, ERRMODE_SILENT, PDO_ERRMODE_SILENT);
    HHVM_RCC_INT(PDO, ERRMODE_WARNING, PDO_ERRMODE_WARNING);
    HHVM_RCC_INT(PDO, ERRMODE_EXCEPTION, PDO_ERRMODE_EXCEPTION);
    HHVM_RCC_INT(PDO, CASE_NATURAL, PDO_CASE_NATURAL);
    HHVM_RCC_INT(PDO, CASE_LOWER, PDO_CASE_LOWER);
    HHVM_RCC_INT(PDO, CASE_UPPER, PDO_CASE_UPPER);
    HHVM_RCC_INT(PDO, NULL_NATURAL, PDO_NULL_NATURAL);
    HHVM_RCC_INT(PDO, NULL_EMPTY_STRING, PDO_NULL_EMPTY_STRING);
    HHVM_RCC_INT(PDO, NULL_TO_STRING, PDO_NULL_TO_STRING);
    HHVM_RCC_INT(PDO, FETCH_ORI_NEXT, PDO_FETCH_ORI_NEXT);
    HHVM_RCC_INT(PDO, FETCH_ORI_PRIOR, PDO_FETCH_ORI_PRIOR);
    HHVM_RCC_INT(PDO, FETCH_ORI_FIRST, PDO_FETCH_ORI_FIRST);
    HHVM_RCC_INT(PDO, FETCH_ORI_LAST, PDO_FETCH_ORI_LAST);
    HHVM_RCC_INT(PDO, FETCH_ORI_ABS, PDO_FETCH_ORI_ABS);
    HHVM_RCC_INT(PDO, FETCH_ORI_REL, PDO_FETCH_ORI_REL);
    HHVM_RCC_INT(PDO, CURSOR_FWDONLY, PDO_CURSOR_FWDONLY);
    HHVM_RCC_INT(PDO, CURSOR_SCROLL, PDO_CURSOR_SCROLL);
#ifdef ENABLE_EXTENSION_PDO_MYSQL
    HHVM_RCC_INT(PDO, MYSQL_ATTR_USE_BUFFERED_QUERY,
                 PDO_MYSQL_ATTR_USE_BUFFERED_QUERY);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_LOCAL_INFILE, PDO_MYSQL_ATTR_LOCAL_INFILE);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_MAX_BUFFER_SIZE,
                 PDO_MYSQL_ATTR_MAX_BUFFER_SIZE);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_INIT_COMMAND, PDO_MYSQL_ATTR_INIT_COMMAND);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_READ_DEFAULT_FILE,
                 PDO_MYSQL_ATTR_READ_DEFAULT_FILE);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_READ_DEFAULT_GROUP,
                 PDO_MYSQL_ATTR_READ_DEFAULT_GROUP);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_COMPRESS, PDO_MYSQL_ATTR_COMPRESS);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_DIRECT_QUERY, PDO_MYSQL_ATTR_DIRECT_QUERY);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_FOUND_ROWS, PDO_MYSQL_ATTR_FOUND_ROWS);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_IGNORE_SPACE, PDO_MYSQL_ATTR_IGNORE_SPACE);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_SSL_CA, PDO_MYSQL_ATTR_SSL_CA);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_SSL_CAPATH, PDO_MYSQL_ATTR_SSL_CAPATH);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_SSL_CERT, PDO_MYSQL_ATTR_SSL_CERT);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_SSL_KEY, PDO_MYSQL_ATTR_SSL_KEY);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_SSL_CIPHER, PDO_MYSQL_ATTR_SSL_CIPHER);
    HHVM_RCC_INT(PDO, MYSQL_ATTR_MULTI_STATEMENTS,
                 PDO_MYSQL_ATTR_MULTI_STATEMENTS);
    HHVM_RCC_INT(PDO, HH_MYSQL_ATTR_READ_TIMEOUT,
                 HH_PDO_MYSQL_ATTR_READ_TIMEOUT);
    HHVM_RCC_INT(PDO, HH_MYSQL_ATTR_WRITE_TIMEOUT,
                 HH_PDO_MYSQL_ATTR_WRITE_TIMEOUT);
#endif
    HHVM_RCC_STR(PDO, ERR_NONE, PDO_ERR_NONE);

    Native::registerNativeDataInfo<PDOData>(
      s_PDO.get(), Native::NDIFlags::NO_SWEEP);
    Native::registerNativeDataInfo<PDOStatementData>(
      s_PDOStatement.get(), Native::NDIFlags::NO_SWEEP);
  }
} s_pdo_extension;

//////////////////////////////////////////////////////////////////////////////
}
