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
#pragma once

#include <string>

#include <folly/Range.h>

#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/string-data.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct Array;
struct OptString;
struct TypedValue;

//////////////////////////////////////////////////////////////////////

/*
 * Process-lifetime strings are allocated using a table managed
 * through this api.
 *
 * We refer to these strings as "static strings"---they may be passed
 * around like request local strings, but have a bit set in their
 * reference count which indicates they should not actually be
 * incref'd or decref'd, and therefore are never freed. Furthermore,
 * any string marked static must be in the table and therefore can
 * be compared by pointer.
 *
 * Because all constants defined in hhvm programs create a
 * process-lifetime string for the constant name, this module also
 * manages a mapping from constant names to rds::Handles.
 */

//////////////////////////////////////////////////////////////////////

extern StringData** precomputed_chars;

inline bool is_static_string(const StringData* s) {
  return is_low_mem(reinterpret_cast<void*>(const_cast<StringData*>(s)));
}

/*
 * Attempt to lookup a string (specified in various ways) in the
 * static string table.  If it's not there, create a new static string
 * and return it.
 */
StringData* makeStaticString(const StringData* str);
StringData* makeStaticString(folly::StringPiece);
StringData* makeStaticString(const std::string& str);
StringData* makeStaticString(const OptString& str);
StringData* makeStaticString(const char* str, size_t len);
StringData* makeStaticString(const char* str);

#define LAZY_STATIC_STRING(x) \
  []{ \
    static StaticString result(x); \
    return result.get(); \
  }()

/*
 * Insert an already initialized static StringData into the static string table.
 * If the same string is already present, invoke the deleter to take appropriate
 * action. By default, the deleter tries to free the memory; thus we need to
 * pass a custom deleter if we manipulate pre-allocated memory.
 */
StringData* insertStaticString(StringData*,
                               void (*deleter)(StringData*) = nullptr);

/*
 * Lookup static strings for single character strings.  (We pre-create
 * static strings for all 256 characters at process startup.)
 */
StringData* makeStaticString(char c);

/*
 * Attempt to look up a static string for `str' if it exists, without
 * inserting it if not. Requires the input string to be known non-static.
 *
 * Returns: a string that isStatic(), or nullptr if there was none.
 */
StringData* lookupStaticString(const StringData* str);
StringData* lookupStaticString(folly::StringPiece);

/*
 * Return the number of static strings in the process.
 */
size_t makeStaticStringCount();

/*
 * Return total size of static strings in bytes
 */
size_t makeStaticStringSize();

/*
 * Functions mapping constants to RDS handles to their values in a
 * given request.
 */
rds::Handle lookupCnsHandle(const StringData* cnsName);
rds::Handle makeCnsHandle(const StringData* cnsName);

/*
 * Bind a persistent constant if its not yet been bound.
 *
 * Returns true iff the constant has a persistent handle.
 */
bool bindPersistentCns(const StringData* cnsName, const TypedValue& value);

/*
 * Return an array of all the static strings in the current
 * execution context.
 */
std::vector<StringData*> lookupDefinedStaticStrings();

/*
 * Return an array of all the defined constants in the current
 * execution context.
 */
Array lookupDefinedConstants(bool categorize = false);

/*
 * Return the number of static strings that correspond to defined
 * constants.
 */
size_t countStaticStringConstants();

/*
 * Initialize the static string table. This needs to happen before any static
 * strings are materialized.
 */
void create_string_data_map();
/*
 * The static string table is generally initially created before main
 * by global constructors (StaticString objects).  After we've parsed
 * options, we may find out a different size was requested for the
 * table.
 *
 * This function is called after runtime option parsing to
 * conditionally resize the table if its size was wrong.  We must
 * still be in a single-threaded environment.
 */
void refineStaticStringTableSize();

void log_static_strings();

//////////////////////////////////////////////////////////////////////

}
