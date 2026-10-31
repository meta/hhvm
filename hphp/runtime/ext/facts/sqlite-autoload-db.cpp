/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/ext/facts/sqlite-autoload-db.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <folly/logging/LogStreamProcessor.h>
#include <folly/logging/xlog.h>

#include <fmt/format.h>
#include "hphp/runtime/ext/facts/autoload-db.h"
#include "hphp/runtime/ext/facts/file-facts.h"
#include "hphp/runtime/ext/facts/sqlite-key.h"
#include "hphp/util/assertions.h"
#include "hphp/util/optional.h"
#include "hphp/util/sqlite-wrapper-helpers.h"
#include "hphp/util/sqlite-wrapper.h"

namespace fs = std::filesystem;

namespace HPHP {
namespace Facts {

namespace {

/**
 * Create the given file if it doesn't exist, setting its group ownership and
 * permissions along the way.
 */
bool createFileWithPerms(const fs::path& path, ::gid_t gid, ::mode_t perms) {
  bool succeeded = true;

  XLOGF(
      DBG1,
      "Creating {} with gid={} and perms={:04o}",
      path.native(),
      gid,
      perms);
  int dbFd = ::open(path.native().c_str(), O_CREAT, perms);
  if (dbFd == -1) {
    XLOGF(ERR, "Could not open DB at {}: errno={}", path.native(), errno);
    return false;
  }
  SCOPE_EXIT {
    ::close(dbFd);
  };
  if (::fchown(dbFd, static_cast<uid_t>(-1), gid) == -1) {
    XLOGF(
        ERR,
        "Could not chown({}, -1, {}): errno={}",
        path.native(),
        gid,
        errno);
    succeeded = false;
  }
  if (::fchmod(dbFd, perms) == -1) {
    XLOGF(
        ERR,
        "Could not chmod({}, {:04o}): errno={}",
        path.native(),
        perms,
        errno);
    succeeded = false;
  }

  return succeeded;
}

// Representation of inheritance kinds in the DB

// `extends`, `implements`, or `use`
const int kDeriveKindExtends = 0;
const int kDeriveKindRequireExtends = 1;
const int kDeriveKindRequireImplements = 2;
const int kDeriveKindRequireClass = 3;
const int kDeriveKindRequireThisAs = 4;

constexpr int toDBEnum(DeriveKind kind) {
  switch (kind) {
    case DeriveKind::Extends:
      return kDeriveKindExtends;
    case DeriveKind::RequireExtends:
      return kDeriveKindRequireExtends;
    case DeriveKind::RequireImplements:
      return kDeriveKindRequireImplements;
    case DeriveKind::RequireClass:
      return kDeriveKindRequireClass;
    case DeriveKind::RequireThisAs:
      return kDeriveKindRequireThisAs;
  }
  return -1;
}

void createSchema(
    SQLiteTxn& txn,
    bool excludePackageMembership,
    bool includeFunctionAttributes) {
  // Basically copied wholesale from FlibAutoloadMapSQL.php in WWW.

  // Common DB

  // Parent tables
  txn.exec(
      "CREATE TABLE IF NOT EXISTS all_paths ("
      " pathid INTEGER PRIMARY KEY,"
      " path TEXT NOT NULL UNIQUE"
      ")");

  // Table storing data about Classes, Interfaces, Enums, and Traits
  txn.exec(
      "CREATE TABLE IF NOT EXISTS type_details ("
      " typeid INTEGER PRIMARY KEY,"
      " name TEXT NOT NULL COLLATE NOCASE,"
      " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
      " kind_of TEXT NOT NULL,"
      " flags INTEGER NOT NULL,"
      " UNIQUE (pathid, name)"
      ")");

  // Path assocs

  txn.exec(
      "CREATE TABLE IF NOT EXISTS path_sha1sum ("
      " pathid INTEGER NOT NULL UNIQUE REFERENCES all_paths ON DELETE CASCADE,"
      " sha1sum TEXT NOT NULL"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS function_paths ("
      " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
      " function TEXT NOT NULL,"
      " UNIQUE (pathid, function)"
      ")");

  if (includeFunctionAttributes) {
    txn.exec(
        "CREATE TABLE IF NOT EXISTS function_attributes ("
        " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
        " function TEXT NOT NULL,"
        " attribute_name TEXT NOT NULL,"
        " attribute_position INTEGER NULL,"
        " attribute_value TEXT NULL,"
        " UNIQUE (pathid, function, attribute_name, attribute_position)"
        ")");
  }

  txn.exec(
      "CREATE TABLE IF NOT EXISTS constant_paths ("
      " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
      " constant TEXT NOT NULL,"
      " UNIQUE (pathid, constant)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS derived_types ("
      " derived_id INTEGER NOT NULL REFERENCES type_details ON DELETE CASCADE,"
      " base_name TEXT NOT NULL COLLATE NOCASE,"
      " kind INTEGER NOT NULL,"
      " UNIQUE (derived_id, base_name, kind)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS watchman ("
      " id INTEGER PRIMARY KEY CHECK (id = 0),"
      " clock TEXT NULL,"
      " mergebase TEXT NULL"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS type_attributes ("
      " typeid INTEGER NOT NULL REFERENCES type_details ON DELETE CASCADE,"
      " attribute_name TEXT NOT NULL,"
      " attribute_position INTEGER NULL,"
      " attribute_value TEXT NULL,"
      " UNIQUE (typeid, attribute_name, attribute_position)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS method_attributes ("
      " typeid INTEGER NOT NULL REFERENCES type_details ON DELETE CASCADE,"
      " method TEXT NOT NULL,"
      " attribute_name TEXT NOT NULL,"
      " attribute_position INTEGER NULL,"
      " attribute_value TEXT NULL,"
      " UNIQUE (typeid, method, attribute_name, attribute_position)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS file_attributes ("
      " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
      " attribute_name TEXT NOT NULL,"
      " attribute_position INTEGER NULL,"
      " attribute_value TEXT NULL,"
      " UNIQUE (pathid, attribute_name, attribute_position)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS file_modules ("
      " pathid INTEGER NOT NULL REFERENCES all_paths ON DELETE CASCADE,"
      " module_name TEXT NOT NULL,"
      " UNIQUE (pathid, module_name)"
      ")");

  txn.exec(
      "CREATE TABLE IF NOT EXISTS file_module_membership ("
      " pathid INTEGER NOT NULL UNIQUE REFERENCES all_paths ON DELETE CASCADE,"
      " module_name TEXT NOT NULL"
      ")");

  if (!excludePackageMembership) {
    txn.exec(
        "CREATE TABLE IF NOT EXISTS packages ("
        " package_id INTEGER PRIMARY KEY, "
        " package_name TEXT NOT NULL"
        ")");

    txn.exec(
        "CREATE TABLE IF NOT EXISTS file_package_membership ("
        " pathid INTEGER NOT NULL PRIMARY KEY REFERENCES all_paths ON DELETE CASCADE,"
        " package_id INTEGER NOT NULL REFERENCES packages"
        ")");
  }
}

void rebuildIndices(SQLiteTxn& txn, bool includeFunctionAttributes) {
  // Basically copied wholesale from FlibAutoloadMapSQL.php in WWW.

  // all_paths
  txn.exec(
      "CREATE INDEX IF NOT EXISTS all_paths__path"
      " ON all_paths (path)");

  // type_details
  txn.exec(
      "CREATE INDEX IF NOT EXISTS type_details__name"
      " ON type_details (name)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS type_details__pathid"
      " ON type_details (pathid)");

  // function_paths
  txn.exec(
      "CREATE INDEX IF NOT EXISTS function_paths__pathid"
      " ON function_paths (pathid)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS function_paths__function"
      " ON function_paths (function)");

  if (includeFunctionAttributes) {
    // function_attributes
    txn.exec(
        "CREATE INDEX IF NOT EXISTS "
        "function_attributes__attribute_name__function__attribute_position"
        " ON function_attributes ("
        "  attribute_name,"
        "  function,"
        "  attribute_position"
        ")");
  }

  // constant_paths
  txn.exec(
      "CREATE INDEX IF NOT EXISTS constant_paths__pathid"
      " ON constant_paths (pathid)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS constant_paths__constant"
      " ON constant_paths (constant)");

  // derived_types
  txn.exec(
      "CREATE INDEX IF NOT EXISTS derived_types__base_name"
      " ON derived_types (base_name)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS derived_types__derived_id"
      " ON derived_types (derived_id)");

  // type_attributes
  txn.exec(
      "CREATE INDEX IF NOT EXISTS "
      "type_attributes__attribute_name__attribute_value"
      " ON type_attributes (attribute_name, attribute_value)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS "
      "type_attributes__attribute_name__typeid__attribute_position"
      " ON type_attributes (attribute_name, typeid, attribute_position)");

  // method_attributes
  txn.exec(
      "CREATE INDEX IF NOT EXISTS "
      "method_attributes__attribute_name__typeid__method__attribute_position"
      " ON method_attributes ("
      "  attribute_name,"
      "  typeid,"
      "  method,"
      "  attribute_position"
      ")");

  // file_attributes
  txn.exec(
      "CREATE INDEX IF NOT EXISTS "
      "file_attributes__attribute_name__pathid__attribute_position"
      " ON file_attributes (attribute_name, pathid, attribute_position)");
  txn.exec(
      "CREATE INDEX IF NOT EXISTS "
      "file_attributes__attribute_name__attribute_value"
      " ON file_attributes (attribute_name, attribute_value)");

  // file_modules
  txn.exec(
      "CREATE INDEX IF NOT EXISTS file_modules__module_name"
      " ON file_modules (module_name)");

  // file_module_membership
  txn.exec(
      "CREATE INDEX IF NOT EXISTS file_module_membership__module_name"
      " ON file_module_membership (module_name)");
}

TypeKind toTypeKind(const std::string_view kind) {
  if (kind == kTypeKindClass) {
    return TypeKind::Class;
  } else if (kind == kTypeKindInterface) {
    return TypeKind::Interface;
  } else if (kind == kTypeKindEnum) {
    return TypeKind::Enum;
  } else if (kind == kTypeKindTrait) {
    return TypeKind::Trait;
  } else if (kind == kTypeKindTypeAlias) {
    return TypeKind::TypeAlias;
  } else {
    return TypeKind::Unknown;
  }
}

inline std::string_view typeKindToSlice(TypeKind kind) {
  switch (kind) {
    case TypeKind::Class:
      return kTypeKindClass;
    case TypeKind::Enum:
      return kTypeKindEnum;
    case TypeKind::Interface:
      return kTypeKindInterface;
    case TypeKind::Trait:
      return kTypeKindTrait;
    case TypeKind::TypeAlias:
      return kTypeKindTypeAlias;
    case TypeKind::Unknown:
      return "";
  }
  not_reached();
}

struct PathStmts {
  explicit PathStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO all_paths (path) VALUES (@path)")},
        m_erase{db.prepare("DELETE FROM all_paths WHERE path = @path")},
        m_getAllPaths{db.prepare(
            "SELECT path, sha1sum FROM path_sha1sum"
            " JOIN all_paths USING (pathid)")},
        m_getPathCount{db.prepare("SELECT COUNT(*) FROM all_paths")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_erase;
  SQLiteStmt m_getAllPaths;
  SQLiteStmt m_getPathCount;
};

struct Sha1HexStmts {
  explicit Sha1HexStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR REPLACE INTO path_sha1sum VALUES ("
            " (SELECT pathid FROM all_paths WHERE path=@path),"
            " @sha1sum"
            ")")},
        m_get{db.prepare(
            "SELECT sha1sum FROM path_sha1sum"
            " JOIN all_paths USING (pathid)"
            " WHERE path = @path")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_get;
};

struct TypeStmts {
  explicit TypeStmts(SQLite& db)
      : m_insertDetails{db.prepare(
            "INSERT OR IGNORE INTO type_details (pathid, name, kind_of, "
            "flags)"
            " VALUES ("
            "  (SELECT pathid FROM all_paths WHERE path=@path),"
            "  @name,"
            "  @kind_of,"
            "  @flags"
            " )")},
        m_getTypePath{db.prepare(
            "SELECT path FROM type_details"
            " JOIN all_paths USING (pathid)"
            " WHERE name=@type")},
        m_getPathTypes{db.prepare(
            "SELECT name FROM type_details"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")},
        m_getKindAndFlags{db.prepare(
            "SELECT kind_of, flags FROM type_details"
            " JOIN all_paths USING (pathid)"
            " WHERE name=@type"
            " AND path=@path")},
        m_insertBaseType{db.prepare(
            "INSERT OR IGNORE INTO derived_types (base_name, derived_id, kind)"
            " VALUES ("
            "  @base,"
            "  (SELECT typeid FROM type_details JOIN all_paths USING (pathid)"
            "   WHERE name=@derived AND path=@path),"
            "  @kind"
            " )")},
        m_getBaseTypes{db.prepare(
            "SELECT base_name FROM derived_types"
            " JOIN type_details AS derived_type ON "
            "(derived_type.typeid=derived_id)"
            " JOIN all_paths USING (pathid)"
            " WHERE derived_type.name = @derived"
            " AND path = @path"
            " AND kind = @kind")},
        m_getDerivedTypes{db.prepare(
            "SELECT path, derived_type.name FROM derived_types"
            " JOIN type_details AS derived_type ON "
            "(derived_type.typeid=derived_id)"
            " JOIN all_paths USING (pathid)"
            " WHERE base_name = @base"
            " AND kind = @kind")},
        m_insertTypeAttribute{db.prepare(
            "INSERT OR IGNORE INTO type_attributes ("
            " typeid,"
            " attribute_name,"
            " attribute_position,"
            " attribute_value"
            ")"
            " VALUES ("
            "  (SELECT typeid FROM type_details JOIN all_paths USING (pathid)"
            "   WHERE name=@type AND path=@path),"
            "  @attribute_name,"
            "  @attribute_position,"
            "  @attribute_value"
            " )")},
        m_insertMethodAttribute{db.prepare(
            "INSERT OR IGNORE INTO method_attributes ("
            " typeid,"
            " method,"
            " attribute_name,"
            " attribute_position,"
            " attribute_value"
            ")"
            " VALUES ("
            "  (SELECT typeid FROM type_details JOIN all_paths USING (pathid)"
            "   WHERE name=@type AND path=@path),"
            "  @method,"
            "  @attribute_name,"
            "  @attribute_position,"
            "  @attribute_value"
            " )")},
        m_getTypeAttributes{db.prepare(
            "SELECT DISTINCT attribute_name"
            " FROM type_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name=@type AND path = @path")},
        m_getMethodAttributes{db.prepare(
            "SELECT DISTINCT attribute_name"
            " FROM method_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name=@type AND method=@method AND path = @path")},
        m_getTypeAttributeArgs{db.prepare(
            "SELECT attribute_value"
            " FROM type_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name = @type"
            " AND kind_of <> 'typeAlias'"
            " AND path = @path"
            " AND attribute_name = @attribute_name")},
        m_getTypeAliasAttributeArgs{db.prepare(
            "SELECT attribute_value"
            " FROM type_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name = @type"
            " AND kind_of = 'typeAlias'"
            " AND path = @path"
            " AND attribute_name = @attribute_name")},
        m_getMethodAttributeArgs{db.prepare(
            "SELECT attribute_value"
            " FROM method_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name = @type"
            " AND method = @method"
            " AND path = @path"
            " AND attribute_name = @attribute_name"
            " ORDER BY attribute_position")},
        m_getTypesWithAttribute{db.prepare(
            "SELECT name, path FROM ("
            " SELECT DISTINCT typeid FROM type_attributes"
            " WHERE attribute_name = @attribute_name"
            ")"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE kind_of <> 'typeAlias'")},
        m_getTypeAliasesWithAttribute{db.prepare(
            "SELECT name, path FROM ("
            " SELECT DISTINCT typeid FROM type_attributes"
            " WHERE attribute_name = @attribute_name"
            ")"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE kind_of = 'typeAlias'")},
        m_getMethodsInPath{db.prepare(
            "SELECT name, method, path FROM type_details"
            " JOIN method_attributes USING (typeid)"
            " JOIN all_paths USING (pathid) WHERE path = @path")},
        m_getMethodsWithAttribute{db.prepare(
            "SELECT name, method, path"
            " FROM type_details"
            "  JOIN method_attributes USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE attribute_name = @attribute_name")},
        m_getTypeMethodAttributes{db.prepare(
            "SELECT DISTINCT method, attribute_name"
            " FROM method_attributes"
            "  JOIN type_details USING (typeid)"
            "  JOIN all_paths USING (pathid)"
            " WHERE name = @type"
            " AND path = @path")} {}

  SQLiteStmt m_insertDetails;
  SQLiteStmt m_getTypePath;
  SQLiteStmt m_getPathTypes;
  SQLiteStmt m_getKindAndFlags;
  SQLiteStmt m_insertBaseType;
  SQLiteStmt m_getBaseTypes;
  SQLiteStmt m_getDerivedTypes;
  SQLiteStmt m_insertTypeAttribute;
  SQLiteStmt m_insertMethodAttribute;
  SQLiteStmt m_getTypeAttributes;
  SQLiteStmt m_getMethodAttributes;
  SQLiteStmt m_getTypeAttributeArgs;
  SQLiteStmt m_getTypeAliasAttributeArgs;
  SQLiteStmt m_getMethodAttributeArgs;
  SQLiteStmt m_getTypesWithAttribute;
  SQLiteStmt m_getTypeAliasesWithAttribute;
  SQLiteStmt m_getMethodsInPath;
  SQLiteStmt m_getMethodsWithAttribute;
  SQLiteStmt m_getTypeMethodAttributes;
};

struct FileStmts {
  explicit FileStmts(SQLite& db)
      : m_insertFileAttribute{db.prepare(
            "INSERT OR IGNORE INTO file_attributes ("
            " pathid,"
            " attribute_name,"
            " attribute_position,"
            " attribute_value"
            ")"
            " VALUES ("
            "  (SELECT pathid FROM all_paths WHERE path=@path),"
            "  @attribute_name,"
            "  @attribute_position,"
            "  @attribute_value"
            " )")},
        m_getFileAttributes{db.prepare(
            "SELECT DISTINCT attribute_name"
            " FROM file_attributes"
            "  JOIN all_paths USING (pathid)"
            " WHERE path = @path")},
        m_getFileAttributeArgs{db.prepare(
            "SELECT attribute_value"
            " FROM file_attributes"
            "  JOIN all_paths USING (pathid)"
            " WHERE path = @path"
            " AND attribute_name = @attribute_name")},
        m_getFilesWithAttribute{db.prepare(
            "SELECT path from all_paths"
            " JOIN file_attributes USING (pathid)"
            " WHERE attribute_name = @attribute_name")},
        m_getFilesWithAttributeAndAnyValue{db.prepare(
            "SELECT path FROM all_paths"
            " JOIN file_attributes USING (pathid)"
            " WHERE attribute_name = @attribute_name"
            " AND attribute_value = @attribute_value")},
        m_getFilesAndAttrValsWithAttribute{db.prepare(
            "SELECT path, attribute_value FROM all_paths"
            " JOIN file_attributes USING (pathid)"
            " WHERE attribute_name = @attribute_name")} {}

  SQLiteStmt m_insertFileAttribute;
  SQLiteStmt m_getFileAttributes;
  SQLiteStmt m_getFileAttributeArgs;
  SQLiteStmt m_getFilesWithAttribute;
  SQLiteStmt m_getFilesWithAttributeAndAnyValue;
  SQLiteStmt m_getFilesAndAttrValsWithAttribute;
};

struct FunctionAttributeStmts {
  explicit FunctionAttributeStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO function_attributes ("
            " pathid,"
            " function,"
            " attribute_name,"
            " attribute_position,"
            " attribute_value"
            ") VALUES ("
            " (SELECT pathid FROM all_paths WHERE path=@path),"
            " @function,"
            " @attribute_name,"
            " @attribute_position,"
            " @attribute_value"
            ")")},
        m_getAttributes{db.prepare(
            "SELECT DISTINCT attribute_name"
            " FROM function_attributes"
            " JOIN all_paths USING (pathid)"
            " WHERE function=@function AND path=@path")},
        m_getAttributeArgs{db.prepare(
            "SELECT attribute_value"
            " FROM function_attributes"
            " JOIN all_paths USING (pathid)"
            " WHERE function=@function"
            " AND path=@path"
            " AND attribute_name=@attribute_name"
            " ORDER BY attribute_position")},
        m_getFunctionsWithAttribute{db.prepare(
            "SELECT DISTINCT function, path"
            " FROM function_attributes"
            " JOIN all_paths USING (pathid)"
            " WHERE attribute_name=@attribute_name")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_getAttributes;
  SQLiteStmt m_getAttributeArgs;
  SQLiteStmt m_getFunctionsWithAttribute;
};

struct FunctionStmts {
  explicit FunctionStmts(SQLite& db, bool includeFunctionAttributes)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO function_paths (function, pathid) VALUES ("
            " @function,"
            " (SELECT pathid FROM all_paths WHERE path=@path)"
            ")")},
        m_getFunctionPath{db.prepare(
            "SELECT path, function FROM function_paths"
            " JOIN all_paths USING (pathid)"
            " WHERE function=@function")},
        m_getPathFunctions{db.prepare(
            "SELECT function FROM function_paths"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")},
        m_attributes{
            includeFunctionAttributes
                ? std::make_unique<FunctionAttributeStmts>(db)
                : nullptr} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_getFunctionPath;
  SQLiteStmt m_getPathFunctions;
  std::unique_ptr<FunctionAttributeStmts> m_attributes;
};

struct ConstantStmts {
  explicit ConstantStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO constant_paths (constant, pathid) VALUES("
            " @constant,"
            " (SELECT pathid FROM all_paths"
            "  WHERE path=@path)"
            ")")},
        m_getConstantPath{db.prepare(
            "SELECT path FROM constant_paths"
            " JOIN all_paths USING (pathid)"
            " WHERE constant=@constant")},
        m_getPathConstants{db.prepare(
            "SELECT constant FROM constant_paths"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_getConstantPath;
  SQLiteStmt m_getPathConstants;
};

struct ValidateStmts {
  explicit ValidateStmts(SQLite& db)
      : m_validateNoDuplicateTypeEntries{db.prepare(
            "SELECT name from type_details"
            " GROUP BY name having count(*) > 1")} {}

  SQLiteStmt m_validateNoDuplicateTypeEntries;
};

// Module Definitions
struct ModuleStmts {
  explicit ModuleStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO file_modules (pathid, module_name) VALUES("
            " (SELECT pathid FROM all_paths WHERE path=@path),"
            " @module_name"
            ")")},
        m_getModulePath{db.prepare(
            "SELECT path FROM file_modules"
            " JOIN all_paths USING (pathid)"
            " WHERE module_name=@module_name")},
        m_getPathModules{db.prepare(
            "SELECT module_name FROM file_modules"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")},
        m_getAllModules{
            db.prepare("SELECT DISTINCT module_name FROM file_modules")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_getModulePath;
  SQLiteStmt m_getPathModules;
  SQLiteStmt m_getAllModules;
};

struct ModuleMembershipStmts {
  explicit ModuleMembershipStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR IGNORE INTO file_module_membership (pathid, module_name) VALUES("
            " (SELECT pathid FROM all_paths WHERE path=@path),"
            " @module_name"
            ")")},
        m_getModuleMembers{db.prepare(
            "SELECT path FROM file_module_membership"
            " JOIN all_paths USING (pathid)"
            " WHERE module_name=@module_name")},
        m_getPathModuleMembership{db.prepare(
            "SELECT module_name FROM file_module_membership"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_getModuleMembers;
  SQLiteStmt m_getPathModuleMembership;
};

struct PackageMembershipStmts {
  explicit PackageMembershipStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR REPLACE INTO file_package_membership (pathid, package_id) "
            "VALUES ("
            " (SELECT pathid FROM all_paths WHERE path=@path),"
            " (SELECT package_id FROM packages WHERE package_name=@package_name)"
            ")")},
        m_insertPackage{db.prepare(
            "INSERT OR IGNORE INTO packages (package_name) VALUES("
            " @package_name"
            ")")},
        m_getPathPackageMembership{db.prepare(
            "SELECT package_name FROM file_package_membership"
            " JOIN packages USING (package_id)"
            " JOIN all_paths USING (pathid)"
            " WHERE path=@path")} {}

  SQLiteStmt m_insert;
  SQLiteStmt m_insertPackage;
  SQLiteStmt m_getPathPackageMembership;
};

struct ClockStmts {
  explicit ClockStmts(SQLite& db)
      : m_insert{db.prepare(
            "INSERT OR REPLACE INTO watchman (OID, clock, mergebase)"
            " VALUES (0, @clock, @mergebase)")},
        m_get{db.prepare("SELECT clock, mergebase FROM watchman WHERE OID=0")} {
  }
  SQLiteStmt m_insert;
  SQLiteStmt m_get;
};

struct SQLiteAutoloadDBImpl final : public SQLiteAutoloadDB {
  explicit SQLiteAutoloadDBImpl(
      SQLite db,
      bool excludePackageMembership,
      bool includeFunctionAttributes)
      : m_db{std::move(db)},
        m_txn{m_db.begin()},
        m_pathStmts{m_db},
        m_sha1HexStmts{m_db},
        m_typeStmts{m_db},
        m_fileStmts{m_db},
        m_functionStmts{m_db, includeFunctionAttributes},
        m_constantStmts{m_db},
        m_validateSmts{m_db},
        m_moduleStmts{m_db},
        m_moduleMembershipStmts{m_db},
        m_packageMembershipStmts{
            excludePackageMembership
                ? nullptr
                : std::make_unique<PackageMembershipStmts>(m_db)},
        m_clockStmts{m_db} {}

  // We can't move `m_db` unless it has no outstanding
  // transactions. I've just chosen to solve this by making
  // `SQLiteAutoloadDBImpl` unmoveable.
  SQLiteAutoloadDBImpl(const SQLiteAutoloadDBImpl&) = delete;
  SQLiteAutoloadDBImpl(SQLiteAutoloadDBImpl&&) noexcept = delete;
  SQLiteAutoloadDBImpl& operator=(const SQLiteAutoloadDBImpl&) = delete;
  SQLiteAutoloadDBImpl& operator=(SQLiteAutoloadDBImpl&&) noexcept = delete;

  ~SQLiteAutoloadDBImpl() override = default;

  static std::shared_ptr<SQLiteAutoloadDB> get(const SQLiteKey& key) {
    assertx(key.m_path.is_absolute());

    // Opening the DB, putting it into WAL mode, and creating its schema can all
    // surface a SQLiteExc. Handle them together so every setup failure produces
    // a message that names the specific problem.
    try {
      auto db = SQLite::connect(key.m_path.native(), key.m_mode);

      switch (key.m_mode) {
        case SQLite::OpenMode::ReadOnly:
        case SQLite::OpenMode::ReadWrite:
          break;
        case SQLite::OpenMode::ReadWriteCreate:
          // Create the DB (and its -shm/-wal siblings) with the requested owner
          // and permissions if they don't already exist.
          std::vector<fs::path> paths_to_create = {
              key.m_path,
              fs::path{key.m_path} += "-shm",
              fs::path{key.m_path} += "-wal",
          };
          for (const auto& path : paths_to_create) {
            if (!createFileWithPerms(path, key.m_gid, key.m_perms)) {
              XLOGF(
                  ERR,
                  "Failed createFileWithPerms for {}:  Debug data: {}",
                  path.native(),
                  key.toDebugString());
            }
          }
          break;
      }

      if (!db.isReadOnly()) {
        try {
          db.setJournalMode(SQLite::JournalMode::WAL);
        } catch (const SQLiteExc& e) {
          // Another connection may put the DB into WAL mode first; only one
          // connection needs to succeed, so a BUSY here is benign.
          if (e.code() != SQLiteExc::Code::BUSY) {
            throw;
          }
        }
      }

      db.setSynchronousLevel(SQLite::SynchronousLevel::OFF);
      // If the database is read only, and we don't have both requisite tables,
      // we can't do package overrides.
      bool excludePackageMembership = db.isReadOnly() &&
          (!db.hasTable("packages") || !db.hasTable("file_package_membership"));
      bool includeFunctionAttributes = db.hasTable("function_attributes") ||
          key.m_mode == SQLite::OpenMode::ReadWriteCreate;

      {
        XLOGF(INFO, "Trying to open SQLite DB at {}", key.m_path.native());
        auto txn = db.begin();

        createSchema(txn, excludePackageMembership, includeFunctionAttributes);
        if (!db.isReadOnly()) {
          rebuildIndices(txn, includeFunctionAttributes);
        }
        db.setBusyTimeout(60'000);
        txn.commit();
        XLOGF(INFO, "Connected to SQLite DB at {}", key.m_path.native());
      }

      return std::make_shared<SQLiteAutoloadDBImpl>(
          std::move(db), excludePackageMembership, includeFunctionAttributes);
    } catch (const SQLiteExc& e) {
      auto action = (key.m_mode == SQLite::OpenMode::ReadWriteCreate)
          ? "open or create"
          : "open";

      // Name the specific failure, and only point at a rebuild when the DB
      // itself is corrupt -- the other conditions need a different remedy.
      std::string explanation;
      switch (e.code()) {
        case SQLiteExc::Code::CORRUPT:
        case SQLiteExc::Code::NOTADB:
          explanation =
              "The native Facts DB is corrupt or not a valid database file.\n"
#ifdef HHVM_FACEBOOK
              "Rebuild it by running 'arc fix facts'.\n"
#endif
              ;
          break;
        case SQLiteExc::Code::FULL:
          explanation =
              "The disk or filesystem holding the native Facts DB is full.\n";
          break;
        case SQLiteExc::Code::IOERR:
          explanation =
              "An I/O error occurred accessing the native Facts DB; the "
              "underlying disk/filesystem may be unhealthy or out of space.\n";
          break;
        case SQLiteExc::Code::CANTOPEN:
          explanation =
              "The native Facts DB file could not be opened; check that its "
              "directory exists and is readable/writable.\n";
          break;
        case SQLiteExc::Code::PERM:
        case SQLiteExc::Code::READONLY:
          explanation =
              "Insufficient permissions to access the native Facts DB.\n";
          break;
        case SQLiteExc::Code::BUSY:
        case SQLiteExc::Code::LOCKED:
          explanation = "The native Facts DB is locked by another process.\n";
          break;
        default:
          break;
      }

      auto exception_str = fmt::format(
          "Couldn't {} native Facts DB.\n{}Key: {}\nReason: {}\n",
          action,
          explanation,
          key.toDebugString(),
          e.what());
      XLOG(ERR, exception_str);
      throw std::runtime_error{exception_str};
    }
  }

  void commit() override {
    m_txn.commit();
    m_txn = m_db.begin();
  }

  void insertPath(const fs::path& path) override {
    assertx(path.is_relative());
    XLOGF(DBG9, "Registering path {} in the DB", path.native());
    auto query = m_txn.query(m_pathStmts.m_insert);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  void insertSha1Hex(const fs::path& path, const Optional<std::string>& sha1hex)
      override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_sha1HexStmts.m_insert);
    query.bindString("@path", path.native());
    if (sha1hex) {
      query.bindString("@sha1sum", *sha1hex);
    } else {
      query.bindNull("@sha1sum");
    }
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::string getSha1Hex(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_sha1HexStmts.m_get);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
    return std::string{query.getString(0)};
  }

  void erasePath(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_pathStmts.m_erase);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  void insertType(
      std::string_view type,
      const fs::path& path,
      TypeKind kind,
      int flags) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_typeStmts.m_insertDetails);
    query.bindString("@name", type);
    query.bindString("@path", path.native());
    query.bindString("@kind_of", typeKindToSlice(kind));
    query.bindInt("@flags", flags);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<fs::path> getTypePath(std::string_view type) override {
    auto query = m_txn.query(m_typeStmts.m_getTypePath);
    query.bindString("@type", type);
    std::vector<fs::path> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(std::string{query.getString(0)});
    }
    return results;
  }

  std::vector<std::string> getPathTypes(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_typeStmts.m_getPathTypes);
    query.bindString("@path", path.native());
    std::vector<std::string> types;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      types.emplace_back(query.getString(0));
    }
    return types;
  }

  KindAndFlags getKindAndFlags(
      const std::string_view type,
      const fs::path& path) override {
    auto query = m_txn.query(m_typeStmts.m_getKindAndFlags);
    query.bindString("@type", type);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      return KindAndFlags{
          .m_kind = toTypeKind(query.getString(0)), .m_flags = query.getInt(1)};
    }
    return {.m_kind = TypeKind::Unknown, .m_flags = 0};
  }

  void insertBaseType(
      const fs::path& path,
      const std::string_view derived,
      DeriveKind kind,
      const std::string_view base) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_typeStmts.m_insertBaseType);
    query.bindString("@path", path.native());
    query.bindString("@derived", derived);
    query.bindInt("@kind", toDBEnum(kind));
    query.bindString("@base", base);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<std::string> getBaseTypes(
      const fs::path& path,
      const std::string_view derived,
      DeriveKind kind) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_typeStmts.m_getBaseTypes);
    query.bindString("@derived", derived);
    query.bindString("@path", path.native());
    query.bindInt("@kind", toDBEnum(kind));
    std::vector<std::string> types;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      types.emplace_back(query.getString(0));
    }
    return types;
  }

  std::vector<SymbolPath> getDerivedTypes(
      const std::string_view base,
      DeriveKind kind) override {
    auto query = m_txn.query(m_typeStmts.m_getDerivedTypes);
    query.bindString("@base", base);
    query.bindInt("@kind", toDBEnum(kind));
    std::vector<SymbolPath> derivedTypes;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      derivedTypes.push_back(
          {.m_symbol = std::string{query.getString(1)},
           .m_path = std::string{query.getString(0)}});
    }
    return derivedTypes;
  }

  void insertTypeAttribute(
      const fs::path& path,
      const std::string_view type,
      const std::string_view attributeName,
      Optional<int> attributePosition,
      const folly::dynamic* attributeValue) override {
    std::string attrValueJson;
    auto query = m_txn.query(m_typeStmts.m_insertTypeAttribute);

    auto const attributeValueKey = "@attribute_value";
    if (attributeValue) {
      attrValueJson = folly::toJson(*attributeValue);
      query.bindString(attributeValueKey, attrValueJson);
    } else {
      query.bindNull(attributeValueKey);
    }

    auto const attributePositionKey = "@attribute_position";
    if (attributePosition) {
      query.bindInt(attributePositionKey, *attributePosition);
    } else {
      query.bindNull(attributePositionKey);
    }

    query.bindString("@type", type);
    query.bindString("@path", path.native());
    query.bindString("@attribute_name", attributeName);

    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  void insertMethodAttribute(
      const fs::path& path,
      std::string_view type,
      std::string_view method,
      std::string_view attributeName,
      Optional<int> attributePosition,
      const folly::dynamic* attributeValue) override {
    std::string attrValueJson;
    auto query = m_txn.query(m_typeStmts.m_insertMethodAttribute);

    auto const attributeValueKey = "@attribute_value";
    if (attributeValue) {
      attrValueJson = folly::toJson(*attributeValue);
      query.bindString(attributeValueKey, attrValueJson);
    } else {
      query.bindNull(attributeValueKey);
    }

    auto const attributePositionKey = "@attribute_position";
    if (attributePosition) {
      query.bindInt(attributePositionKey, *attributePosition);
    } else {
      query.bindNull(attributePositionKey);
    }

    query.bindString("@type", type);
    query.bindString("@method", method);
    query.bindString("@path", path.native());
    query.bindString("@attribute_name", attributeName);

    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  void insertFileAttribute(
      const fs::path& path,
      std::string_view attributeName,
      Optional<int> attributePosition,
      const folly::dynamic* attributeValue) override {
    std::string attrValueJson;
    auto query = m_txn.query(m_fileStmts.m_insertFileAttribute);

    auto const attributeValueKey = "@attribute_value";
    if (attributeValue) {
      attrValueJson = folly::toJson(*attributeValue);
      query.bindString(attributeValueKey, attrValueJson);
    } else {
      query.bindNull(attributeValueKey);
    }

    auto const attributePositionKey = "@attribute_position";
    if (attributePosition) {
      query.bindInt(attributePositionKey, *attributePosition);
    } else {
      query.bindNull(attributePositionKey);
    }

    query.bindString("@path", path.native());
    query.bindString("@attribute_name", attributeName);

    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<std::string> getAttributesOfType(
      const std::string_view type,
      const fs::path& path) override {
    auto query = m_txn.query(m_typeStmts.m_getTypeAttributes);
    query.bindString("@type", type);
    query.bindString("@path", path.native());
    std::vector<std::string> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(query.getString(0));
    }
    return results;
  }

  std::vector<std::string> getAttributesOfMethod(
      std::string_view type,
      std::string_view method,
      const fs::path& path) override {
    auto query = m_txn.query(m_typeStmts.m_getMethodAttributes);
    query.bindString("@type", type);
    query.bindString("@method", method);
    query.bindString("@path", path.native());
    std::vector<std::string> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(query.getString(0));
    }
    return results;
  }

  std::vector<std::string> getAttributesOfFile(const fs::path& path) override {
    auto query = m_txn.query(m_fileStmts.m_getFileAttributes);
    query.bindString("@path", path.native());
    std::vector<std::string> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(query.getString(0));
    }
    return results;
  }

  std::vector<SymbolPath> getTypesWithAttribute(
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getTypesWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<SymbolPath> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(
          SymbolPath{
              .m_symbol = std::string{query.getString(0)},
              .m_path = std::string{query.getString(1)}});
    }
    return results;
  }

  std::vector<SymbolPath> getTypeAliasesWithAttribute(
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getTypeAliasesWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<SymbolPath> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(
          SymbolPath{
              .m_symbol = std::string{query.getString(0)},
              .m_path = std::string{query.getString(1)}});
    }
    return results;
  }

  std::vector<MethodPath> getPathMethods(std::string_view path) override {
    auto query = m_txn.query(m_typeStmts.m_getMethodsInPath);
    query.bindString("@path", path);
    std::vector<MethodPath> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(
          MethodPath{
              .m_type = std::string{query.getString(0)},
              .m_method = std::string{query.getString(1)},
              .m_path = std::string{query.getString(2)}});
    }
    return results;
  }

  std::vector<MethodPath> getMethodsWithAttribute(
      std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getMethodsWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<MethodPath> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(
          MethodPath{
              .m_type = std::string{query.getString(0)},
              .m_method = std::string{query.getString(1)},
              .m_path = std::string{query.getString(2)}});
    }
    return results;
  }

  std::vector<std::pair<std::string, std::string>> getTypeMethodAttributes(
      std::string_view type,
      const fs::path& path) override {
    auto query = m_txn.query(m_typeStmts.m_getTypeMethodAttributes);
    query.bindString("@type", type);
    query.bindString("@path", path.native());
    std::vector<std::pair<std::string, std::string>> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(query.getString(0), query.getString(1));
    }
    return results;
  }

  std::vector<fs::path> getFilesWithAttribute(
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_fileStmts.m_getFilesWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<fs::path> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(fs::path{std::string{query.getString(0)}});
    }
    return results;
  }

  std::vector<fs::path> getFilesWithAttributeAndAnyValue(
      const std::string_view attributeName,
      const folly::dynamic& attributeValue) override {
    auto query = m_txn.query(m_fileStmts.m_getFilesWithAttributeAndAnyValue);
    std::string jsonValue = folly::toJson(attributeValue);
    query.bindString("@attribute_name", attributeName);
    query.bindString("@attribute_value", jsonValue);
    std::vector<fs::path> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(fs::path{std::string{query.getString(0)}});
    }
    return results;
  }

  std::vector<PathAndAttrVal> getFilesAndAttrValsWithAttribute(
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_fileStmts.m_getFilesAndAttrValsWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<PathAndAttrVal> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      auto maybeVal = query.getNullableString(1);
      auto path = fs::path{std::string{query.getString(0)}};
      if (maybeVal) {
        results.push_back(PathAndAttrVal{path, folly::parseJson(*maybeVal)});
      } else {
        results.push_back(PathAndAttrVal{path, std::nullopt});
      }
    }
    return results;
  }

  std::vector<folly::dynamic> getTypeAttributeArgs(
      const std::string_view type,
      const std::string_view path,
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getTypeAttributeArgs);
    query.bindString("@type", type);
    query.bindString("@path", path);
    query.bindString("@attribute_name", attributeName);
    XLOGF(DBG9, "Running {}", query.sql());
    std::vector<folly::dynamic> args;
    for (query.step(); query.row(); query.step()) {
      auto arg = query.getNullableString(0);
      if (arg) {
        args.push_back(folly::parseJson(*arg));
      }
    }
    return args;
  }

  std::vector<folly::dynamic> getTypeAliasAttributeArgs(
      const std::string_view typeAlias,
      const std::string_view path,
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getTypeAliasAttributeArgs);
    query.bindString("@type", typeAlias);
    query.bindString("@path", path);
    query.bindString("@attribute_name", attributeName);
    XLOGF(DBG9, "Running {}", query.sql());
    std::vector<folly::dynamic> args;
    for (query.step(); query.row(); query.step()) {
      auto arg = query.getNullableString(0);
      if (arg) {
        args.push_back(folly::parseJson(*arg));
      }
    }
    return args;
  }

  std::vector<folly::dynamic> getMethodAttributeArgs(
      std::string_view type,
      std::string_view method,
      std::string_view path,
      std::string_view attributeName) override {
    auto query = m_txn.query(m_typeStmts.m_getMethodAttributeArgs);
    query.bindString("@type", type);
    query.bindString("@method", method);
    query.bindString("@path", path);
    query.bindString("@attribute_name", attributeName);
    XLOGF(DBG9, "Running {}", query.sql());
    std::vector<folly::dynamic> args;
    for (query.step(); query.row(); query.step()) {
      auto arg = query.getNullableString(0);
      if (arg) {
        args.push_back(folly::parseJson(*arg));
      }
    }
    return args;
  }

  std::vector<folly::dynamic> getFileAttributeArgs(
      const std::string_view path,
      const std::string_view attributeName) override {
    auto query = m_txn.query(m_fileStmts.m_getFileAttributeArgs);
    query.bindString("@path", path);
    query.bindString("@attribute_name", attributeName);
    XLOGF(DBG9, "Running {}", query.sql());
    std::vector<folly::dynamic> args;
    for (query.step(); query.row(); query.step()) {
      auto arg = query.getNullableString(0);
      if (arg) {
        args.push_back(folly::parseJson(*arg));
      }
    }
    return args;
  }

  void insertFunction(std::string_view function, const fs::path& path)
      override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_functionStmts.m_insert);
    query.bindString("@function", function);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  void insertFunctionAttribute(
      const fs::path& path,
      std::string_view function,
      std::string_view attributeName,
      Optional<int> attributePosition,
      const folly::dynamic* attributeValue) override {
    if (!m_functionStmts.m_attributes) {
      return;
    }
    std::string attrValueJson;
    auto query = m_txn.query(m_functionStmts.m_attributes->m_insert);
    if (attributeValue) {
      attrValueJson = folly::toJson(*attributeValue);
      query.bindString("@attribute_value", attrValueJson);
    } else {
      query.bindNull("@attribute_value");
    }
    if (attributePosition) {
      query.bindInt("@attribute_position", *attributePosition);
    } else {
      query.bindNull("@attribute_position");
    }
    query.bindString("@function", function);
    query.bindString("@path", path.native());
    query.bindString("@attribute_name", attributeName);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<std::string> getAttributesOfFunction(
      std::string_view function,
      const fs::path& path) override {
    if (!m_functionStmts.m_attributes) {
      return {};
    }
    auto query = m_txn.query(m_functionStmts.m_attributes->m_getAttributes);
    query.bindString("@function", function);
    query.bindString("@path", path.native());
    std::vector<std::string> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(query.getString(0));
    }
    return results;
  }

  std::vector<folly::dynamic> getFunctionAttributeArgs(
      std::string_view function,
      std::string_view path,
      std::string_view attributeName) override {
    if (!m_functionStmts.m_attributes) {
      return {};
    }
    auto query = m_txn.query(m_functionStmts.m_attributes->m_getAttributeArgs);
    query.bindString("@function", function);
    query.bindString("@path", path);
    query.bindString("@attribute_name", attributeName);
    std::vector<folly::dynamic> args;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      auto arg = query.getNullableString(0);
      if (arg) {
        args.push_back(folly::parseJson(*arg));
      }
    }
    return args;
  }

  std::vector<SymbolPath> getFunctionsWithAttribute(
      std::string_view attributeName) override {
    if (!m_functionStmts.m_attributes) {
      return {};
    }
    auto query =
        m_txn.query(m_functionStmts.m_attributes->m_getFunctionsWithAttribute);
    query.bindString("@attribute_name", attributeName);
    std::vector<SymbolPath> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.push_back(
          {.m_symbol = std::string{query.getString(0)},
           .m_path = std::string{query.getString(1)}});
    }
    return results;
  }

  std::vector<std::pair<fs::path, std::string>> getFunctionPath(
      std::string_view function) override {
    auto query = m_txn.query(m_functionStmts.m_getFunctionPath);
    query.bindString("@function", function);
    XLOGF(DBG9, "Running {}", query.sql());
    std::vector<std::pair<fs::path, std::string>> results;
    for (query.step(); query.row(); query.step()) {
      auto path = std::string{query.getString(0)};
      auto actual_func = std::string{query.getString(1)};
      results.emplace_back(path, actual_func);
    }
    return results;
  }

  std::vector<std::string> getPathFunctions(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_functionStmts.m_getPathFunctions);
    query.bindString("@path", path.native());
    std::vector<std::string> functions;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      functions.emplace_back(query.getString(0));
    }
    return functions;
  }

  void insertConstant(std::string_view constant, const fs::path& path)
      override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_constantStmts.m_insert);
    query.bindString("@constant", constant);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<fs::path> getConstantPath(std::string_view constant) override {
    auto query = m_txn.query(m_constantStmts.m_getConstantPath);
    query.bindString("@constant", constant);
    std::vector<fs::path> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(std::string{query.getString(0)});
    }
    return results;
  }

  std::vector<std::string> getPathConstants(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_constantStmts.m_getPathConstants);
    query.bindString("@path", path.native());
    std::vector<std::string> constants;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      constants.emplace_back(query.getString(0));
    }
    return constants;
  }

  void validate(const std::set<std::string>& types_to_ignore) override {
    auto query = m_txn.query(m_validateSmts.m_validateNoDuplicateTypeEntries);
    XLOGF(DBG9, "Running Validation query {}", query.sql());
    std::vector<std::string> invalidRows;
    for (query.step(); query.row(); query.step()) {
      if (!types_to_ignore.contains(std::string(query.getString(0)))) {
        invalidRows.emplace_back(query.getString(0));
      }
    }
    if (!invalidRows.empty()) {
      std::string joined = std::accumulate(
          invalidRows.begin(), invalidRows.end(), std::string(" "));
      std::string exception_str = fmt::format(
          "Native Facts DB has duplicate entries!\n"
#ifdef HHVM_FACEBOOK
          "You may be able to fix this by running 'arc fix facts'\n"
#endif
          "Offending Types: {}\n",
          joined);
      XLOG(ERR, exception_str);
      throw std::logic_error{exception_str};
    }
  }

  void insertModule(std::string_view module, const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_moduleStmts.m_insert);
    query.bindString("@module_name", module);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::vector<fs::path> getModulePath(std::string_view module) override {
    auto query = m_txn.query(m_moduleStmts.m_getModulePath);
    query.bindString("@module_name", module);
    std::vector<fs::path> results;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(std::string{query.getString(0)});
    }
    return results;
  }

  std::vector<std::string> getPathModules(const fs::path& path) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_moduleStmts.m_getPathModules);
    query.bindString("@path", path.native());
    std::vector<std::string> modules;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      modules.emplace_back(query.getString(0));
    }
    return modules;
  }

  std::vector<std::string> getAllModules() override {
    auto query = m_txn.query(m_moduleStmts.m_getAllModules);
    std::vector<std::string> modules;
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      modules.emplace_back(query.getString(0));
    }
    return modules;
  }

  void insertModuleMembership(
      const std::filesystem::path& path,
      std::string_view module) override {
    assertx(path.is_relative());
    auto query = m_txn.query(m_moduleMembershipStmts.m_insert);
    query.bindString("@path", path.native());
    query.bindString("@module_name", module);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  std::optional<std::string> getPathModuleMembership(
      const std::filesystem::path& path) override {
    std::optional<std::string> result;
    auto query = m_txn.query(m_moduleMembershipStmts.m_getPathModuleMembership);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      assertx(!result.has_value());
      result.emplace(std::string{query.getString(0)});
    }
    return result;
  }

  std::vector<std::filesystem::path> getModuleMembers(
      std::string_view module) override {
    std::vector<fs::path> results;
    auto query = m_txn.query(m_moduleMembershipStmts.m_getModuleMembers);
    query.bindString("@module_name", module);
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      results.emplace_back(std::string{query.getString(0)});
    }
    return results;
  }

  void insertPackageMembership(
      const std::filesystem::path& path,
      std::string_view package) override {
    if (!hasPackageMembership()) {
      return;
    }

    assertx(path.is_relative());
    {
      auto query = m_txn.query(m_packageMembershipStmts->m_insertPackage);
      query.bindString("@package_name", package);
      XLOGF(DBG9, "Running {}", query.sql());
      query.step();
    }
    {
      auto query = m_txn.query(m_packageMembershipStmts->m_insert);
      query.bindString("@path", path.native());
      query.bindString("@package_name", package);
      XLOGF(DBG9, "Running {}", query.sql());
      query.step();
    }
  }

  std::optional<std::string> getPathPackageMembership(
      const std::filesystem::path& path) override {
    if (!hasPackageMembership()) {
      return std::nullopt;
    }

    std::optional<std::string> result;
    auto query =
        m_txn.query(m_packageMembershipStmts->m_getPathPackageMembership);
    query.bindString("@path", path.native());
    XLOGF(DBG9, "Running {}", query.sql());
    for (query.step(); query.row(); query.step()) {
      assertx(!result.has_value());
      result.emplace(std::string{query.getString(0)});
    }
    return result;
  }

  MultiResult<PathAndHash> getAllPathsAndHashes() override {
    auto query = m_txn.query(m_pathStmts.m_getAllPaths);
    XLOGF(DBG9, "Running {}", query.sql());
    return MultiResult<PathAndHash>{
        [q = std::move(query)]() mutable -> Optional<PathAndHash> {
          q.step();
          if (!q.row()) {
            return {};
          }
          return PathAndHash{
              .m_path = std::string{q.getString(0)},
              .m_hash = std::string{q.getString(1)}};
        }};
  }

  size_t getPathCount() override {
    auto query = m_txn.query(m_pathStmts.m_getPathCount);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
    return static_cast<size_t>(query.getInt(0));
  }

  void insertClock(const Clock& clock) override {
    auto query = m_txn.query(m_clockStmts.m_insert);
    query.bindString("@clock", clock.m_clock);
    query.bindString("@mergebase", clock.m_mergebase);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
  }

  Clock getClock() override {
    auto query = m_txn.query(m_clockStmts.m_get);
    XLOGF(DBG9, "Running {}", query.sql());
    query.step();
    if (!query.row()) {
      return {};
    }
    return {
        .m_clock = std::string{query.getString(0)},
        .m_mergebase = std::string{query.getString(1)}};
  }

  bool isReadOnly() const noexcept override {
    return m_db.isReadOnly();
  }

  bool hasPackageMembership() {
    return m_packageMembershipStmts != nullptr;
  }

  void runPostBuildOptimizations() override {
    try {
      m_db.analyze();
    } catch (const SQLiteExc& e) {
      XLOGF(ERR, "Error while running ANALYZE: {}", e.what());
    } catch (std::exception& e) {
      XLOGF(ERR, "Error while running ANALYZE: {}", e.what());
    }
  }

 private:
  SQLite m_db;
  SQLiteTxn m_txn;
  PathStmts m_pathStmts;
  Sha1HexStmts m_sha1HexStmts;
  TypeStmts m_typeStmts;
  FileStmts m_fileStmts;
  FunctionStmts m_functionStmts;
  ConstantStmts m_constantStmts;
  ValidateStmts m_validateSmts;
  ModuleStmts m_moduleStmts;
  ModuleMembershipStmts m_moduleMembershipStmts;
  std::unique_ptr<PackageMembershipStmts> m_packageMembershipStmts;
  ClockStmts m_clockStmts;
};

} // namespace

std::shared_ptr<SQLiteAutoloadDB> SQLiteAutoloadDB::get(const SQLiteKey& key) {
  return SQLiteAutoloadDBImpl::get(key);
}

} // namespace Facts
} // namespace HPHP
