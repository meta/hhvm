/* JSON_parser.c */

/* 2005-12-30 */

/*
Copyright (c) 2005 JSON.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

The Software shall be used for Good, not Evil.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "hphp/runtime/ext/json/JSON_parser.h"

#include <folly/FBVector.h>

#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/utf8-decode.h"
#include "hphp/runtime/ext/json/ext_json.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/fast_strtoll_base10.h"
#include "hphp/zend/zend-strtod.h"

#define MAX_LENGTH_OF_LONG 20
static const char long_min_digits[] = "9223372036854775808";

namespace HPHP {

/*
    Characters are mapped into these 32 symbol classes. This allows for
    significant reductions in the size of the state transition table.
*/

/* error */
#define S_ERR -1

/* space */
#define S_SPA 0

/* other whitespace */
#define S_WSP 1

/* {  */
#define S_LBE 2

/* } */
#define S_RBE 3

/* [ */
#define S_LBT 4

/* ] */
#define S_RBT 5

/* : */
#define S_COL 6

/* , */
#define S_COM 7

/* " */
#define S_QUO 8

/* \ */
#define S_BAC 9

/* / */
#define S_SLA 10

/* + */
#define S_PLU 11

/* - */
#define S_MIN 12

/* . */
#define S_DOT 13

/* 0 */
#define S_ZER 14

/* 123456789 */
#define S_DIG 15

/* a */
#define S__A_ 16

/* b */
#define S__B_ 17

/* c */
#define S__C_ 18

/* d */
#define S__D_ 19

/* e */
#define S__E_ 20

/* f */
#define S__F_ 21

/* l */
#define S__L_ 22

/* n */
#define S__N_ 23

/* r */
#define S__R_ 24

/* s */
#define S__S_ 25

/* t */
#define S__T_ 26

/* u */
#define S__U_ 27

/* ABCDF */
#define S_A_F 28

/* E */
#define S_E   29

/* everything else */
#define S_ETC 30


/*
    This table maps the 128 ASCII characters into the 32 character classes.
    The remaining Unicode characters should be mapped to S_ETC.
*/
alignas(64) static const int8_t ascii_class[128] = {
    S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,
    S_ERR, S_WSP, S_WSP, S_ERR, S_ERR, S_WSP, S_ERR, S_ERR,
    S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,
    S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,

    S_SPA, S_ETC, S_QUO, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,
    S_ETC, S_ETC, S_ETC, S_PLU, S_COM, S_MIN, S_DOT, S_SLA,
    S_ZER, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG,
    S_DIG, S_DIG, S_COL, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,

    S_ETC, S_A_F, S_A_F, S_A_F, S_A_F, S_E  , S_A_F, S_ETC,
    S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,
    S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,
    S_ETC, S_ETC, S_ETC, S_LBT, S_BAC, S_RBT, S_ETC, S_ETC,

    S_ETC, S__A_, S__B_, S__C_, S__D_, S__E_, S__F_, S_ETC,
    S_ETC, S_ETC, S_ETC, S_ETC, S__L_, S_ETC, S__N_, S_ETC,
    S_ETC, S_ETC, S__R_, S__S_, S__T_, S__U_, S_ETC, S_ETC,
    S_ETC, S_ETC, S_ETC, S_LBE, S_ETC, S_RBE, S_ETC, S_ETC
};

/*<fb>*/
alignas(64) static const int8_t loose_ascii_class[128] = {
  S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,
  S_ERR, S_WSP, S_WSP, S_ERR, S_ERR, S_WSP, S_ERR, S_ERR,
  S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,
  S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR, S_ERR,

  S_SPA, S_ETC, S_QUO, S_ETC, S_ETC, S_ETC, S_ETC, S_QUO,
  S_ETC, S_ETC, S_ETC, S_PLU, S_COM, S_MIN, S_DOT, S_SLA,
  S_ZER, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG, S_DIG,
  S_DIG, S_DIG, S_COL, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,

  S_ETC, S_A_F, S_A_F, S_A_F, S_A_F, S_E  , S_A_F, S_ETC,
  S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,
  S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC, S_ETC,
  S_ETC, S_ETC, S_ETC, S_LBT, S_BAC, S_RBT, S_ETC, S_ETC,

  S_ETC, S__A_, S__B_, S__C_, S__D_, S__E_, S__F_, S_ETC,
  S_ETC, S_ETC, S_ETC, S_ETC, S__L_, S_ETC, S__N_, S_ETC,
  S_ETC, S_ETC, S__R_, S__S_, S__T_, S__U_, S_ETC, S_ETC,
  S_ETC, S_ETC, S_ETC, S_LBE, S_ETC, S_RBE, S_ETC, S_ETC
};
/*</fb>*/



/*
    The state transition table takes the current state and the current symbol,
    and returns either a new state or an action. A new state is a number between
    0 and 29. An action is a negative number between -1 and -9. A JSON text is
    accepted if the end of the text is in state 9 and mode is Mode::DONE.
*/
alignas(64) static const int8_t state_transition_table[30][32] = {
/* 0*/ { 0, 0,-8,-1,-6,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/* 1*/ { 1, 1,-1,-9,-1,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/* 2*/ { 2, 2,-8,-1,-6,-5,-1,-1, 3,-1,-1,-1,20,-1,21,22,-1,-1,-1,-1,-1,13,-1,17,-1,-1,10,-1,-1,-1,-1},
/* 3*/ { 3,-1, 3, 3, 3, 3, 3, 3,-4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
/* 4*/ {-1,-1,-1,-1,-1,-1,-1,-1, 3, 3, 3,-1,-1,-1,-1,-1,-1, 3,-1,-1,-1, 3,-1, 3, 3,-1, 3, 5,-1,-1,-1},
/* 5*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 6, 6, 6, 6, 6, 6, 6, 6,-1,-1,-1,-1,-1,-1, 6, 6,-1},
/* 6*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 7, 7, 7, 7, 7, 7, 7, 7,-1,-1,-1,-1,-1,-1, 7, 7,-1},
/* 7*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 8, 8, 8, 8, 8, 8, 8, 8,-1,-1,-1,-1,-1,-1, 8, 8,-1},
/* 8*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 3, 3, 3, 3, 3, 3, 3, 3,-1,-1,-1,-1,-1,-1, 3, 3,-1},
/* 9*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*10*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,11,-1,-1,-1,-1,-1,-1},
/*11*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,12,-1,-1,-1},
/*12*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*13*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,14,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*14*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,15,-1,-1,-1,-1,-1,-1,-1,-1},
/*15*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,16,-1,-1,-1,-1,-1},
/*16*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*17*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,18,-1,-1,-1},
/*18*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,19,-1,-1,-1,-1,-1,-1,-1,-1},
/*19*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1},
/*20*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,21,22,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*21*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,23,-1,-1,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*22*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,23,22,22,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*23*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,23,23,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*24*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,25,25,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*25*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*26*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*27*/ {27,27,-1,-1,-1,-1,-2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*28*/ {28,28,-8,-1,-6,-1,-1,-1, 3,-1,-1,-1,20,-1,21,22,-1,-1,-1,-1,-1,13,-1,17,-1,-1,10,-1,-1,-1,-1},
/*29*/ {29,29,-1,-1,-1,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}
};

/*<fb>*/
/*
  Alternate "loose" transition table to support unquoted keys.

  Note: State 3 has same outgoing transitions in both transition tables. This is
  used below in the fast-case for appending simple characters (3 -> 3).
*/
alignas(64) static const int8_t loose_state_transition_table[31][32] = {
/* 0*/ { 0, 0,-8,-1,-6,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/* 1*/ { 1, 1,-1,-9,-1,-1,-1,-1, 3,-1,-1,-1,-1,-1,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30},
/* 2*/ { 2, 2,-8,-1,-6,-5,-1,-1, 3,-1,-1,-1,20,-1,21,22,-1,-1,-1,-1,-1,13,-1,17,-1,-1,10,-1,-1,-1,-1},
/* 3*/ { 3,-1, 3, 3, 3, 3, 3, 3,-4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
/* 4*/ {-1,-1,-1,-1,-1,-1,-1,-1, 3, 3, 3,-1,-1,-1,-1,-1,-1, 3,-1,-1,-1, 3,-1, 3, 3,-1, 3, 5,-1,-1,-1},
/* 5*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 6, 6, 6, 6, 6, 6, 6, 6,-1,-1,-1,-1,-1,-1, 6, 6,-1},
/* 6*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 7, 7, 7, 7, 7, 7, 7, 7,-1,-1,-1,-1,-1,-1, 7, 7,-1},
/* 7*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 8, 8, 8, 8, 8, 8, 8, 8,-1,-1,-1,-1,-1,-1, 8, 8,-1},
/* 8*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 3, 3, 3, 3, 3, 3, 3, 3,-1,-1,-1,-1,-1,-1, 3, 3,-1},
/* 9*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*10*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,11,-1,-1,-1,-1,-1,-1},
/*11*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,12,-1,-1,-1},
/*12*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*13*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,14,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*14*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,15,-1,-1,-1,-1,-1,-1,-1,-1},
/*15*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,16,-1,-1,-1,-1,-1},
/*16*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*17*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,18,-1,-1,-1},
/*18*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,19,-1,-1,-1,-1,-1,-1,-1,-1},
/*19*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,-1,-1},
/*20*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,21,22,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*21*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,23,-1,-1,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*22*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,23,22,22,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*23*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,23,23,-1,-1,-1,-1,24,-1,-1,-1,-1,-1,-1,-1,-1,24,-1},
/*24*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,25,25,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*25*/ {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*26*/ { 9, 9,-1,-7,-1,-5,-1,-3,-1,-1,-1,-1,-1,-1,26,26,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*27*/ {27,27,-1,-1,-1,-1,-2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
/*28*/ {28,28,-8,-1,-6,-5,-1,-1, 3,-1,-1,-1,20,-1,21,22,-1,-1,-1,-1,-1,13,-1,17,-1,-1,10,-1,-1,-1,-1},
/*29*/ {29,29,-1,-7,-1,-1,-1,-7, 3,-1,-1,-1,-1,-1,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30},
/*30*/ {30,-1,30,30,30,30,-10,30,-4,4,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30}
};
/*</fb>*/

/**
 * These modes can be pushed on the PDA stack.
 */
enum class Mode {
  INVALID = 0,
  DONE = 1,
  KEY = 2,
  OBJECT = 3,
  ARRAY = 4
};

namespace {

int dehexchar(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - ('A' - 10);
  if (c >= 'a' && c <= 'f') return c - ('a' - 10);
  return -1;
}

bool isSpace(char ch) {
  return ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
}

const char* skipSpace(const char* p) {
  while (isSpace(*p)) p++;
  return p;
}

/*
 * Skip whitespace, then if next char is 'ch', consume it and return true,
 * otherwise let it be and return false.
 */
bool matchSeparator(const char** pp, char ch) {
  const char* p = *pp;
  if (LIKELY(*p == ch)) { *pp = p + 1; return true; };
  p = skipSpace(p);
  if (LIKELY(*p == ch)) { *pp = p + 1; return true; }
  return false;
}

NEVER_INLINE
void tvDecRefRange(TypedValue* begin, TypedValue* end) {
  assertx(begin <= end);
  for (auto tv = begin; tv != end; ++tv) {
    tvDecRefGen(tv);
  }
}

void appendToContainer(JSONContainerType container_type,
                       Variant& base, Variant& value) {
  if (container_type == JSONContainerType::COLLECTIONS) {
    collections::append(base.getObjectData(), value.asTypedValue());
  } else {
    auto& arr = base.asArrRef();
    if (arr.isDict()) {
      arr.set(safe_cast<int64_t>(arr.size()), value);
    } else {
      arr.append(value);
    }
  }
}

/* Holds a pair indicating success of parsing and the new position of the
 * buffer pointer. */
struct ParseResult {
  bool ok;
  const char* p;
};

/* Holds a pair with the length of the parsed string and the new position of
 * the buffer pointer. Length will be set to -1 in case of failure. */
struct ParseStringResult {
  int len;
  const char* p;
};

/*
 * Parses a subset of JSON. Currently unsupported:
 * - Non-ASCII
 * - Character escape sequences
 * - Non-string array keys
 * - Arrays nested > 255 levels
 */
struct SimpleParser {
  static constexpr int kMaxArrayDepth = 255;

  /*
   * Returns buffer size in bytes needed to handle any input up to given length.
   */
  static size_t BufferBytesForLength(int length) {
    return (length + 1) * sizeof(TypedValue) / 2;  // Worst case: "[0,0,...,0]"
  }

  /*
   * Returns false for unsupported or malformed input (does not distinguish).
   */
  static bool TryParse(const char* inp, int length,
                       TypedValue* buf, Variant& out,
                       JSONContainerType container_type, bool is_tsimplejson) {
    assertx(inp[length] == 0);  // Parser relies on sentinel to avoid checks.
    SimpleParser parser(buf, container_type, is_tsimplejson);
    ParseResult result = parser.parseValue(inp);
    const char* p = result.p;
    if (!result.ok || (p = skipSpace(p), p != inp + length)) {
      // Unsupported, malformed, or trailing garbage. Release entire stack.
      tvDecRefRange(buf, parser.top);
      return false;
    }
    out = Variant::attach(*--parser.top);
    return true;
  }

 private:
  SimpleParser(TypedValue* buffer, JSONContainerType container_type,
               bool is_tsimplejson)
    : top(buffer)
    , container_type(container_type)
    , is_tsimplejson(is_tsimplejson)
  { }

  /*
   * Variant parser.
   */
  ParseResult parseValue(const char* p) {
    for (;;) {
      char ch = *p++;
      switch (ch) {
        case '{': {
          if (matchSeparator(&p, '}')) {
            if (!pushMixed(top)) return {false, p};  // push empty.
            break;
          }
          if (!beginMixed()) return {false, p};
          ParseResult result = parseMixedKey(p);
          p = result.p;
          if (!result.ok) return result;
          continue;
        }
        case '[': {
          if (matchSeparator(&p, ']')) {
            pushPacked(top);
            break;
          }
          if (!beginPacked()) return {false, p};
          continue;
        }
        case '\"': {
          ParseResult result = parseString(p);
          p = result.p;
          if (!result.ok) return result;
          break;
        }
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
          ParseResult result = parseNumber(p, ch);
          p = result.p;
          if (!result.ok) return result;
          break;
        }
        case 't': {
          ParseResult result = parseRue(p);
          p = result.p;
          if (!result.ok) return result;
          break;
        }
        case 'f': {
          ParseResult result = parseAlse(p);
          p = result.p;
          if (!result.ok) return result;
          break;
        }
        case 'n': {
          ParseResult result = parseUll(p);
          p = result.p;
          if (!result.ok) return result;
          break;
        }
        case ' ':
        case '\t':
        case '\n':
        case '\r':
          p = skipSpace(p);
          continue;
        default:
          return {false, p};
      }

      // We parsed a JSON element at this point, add it to the surrounding list,
      // dictionary or return.
      for (;;) {
        if (stack_top == 0) {
          return {true, p};
        } else if (stack[stack_top - 1].is_packed) {
          if (matchSeparator(&p, ',')) break;
          if (!matchSeparator(&p, ']')) return {false, p};
          finishPacked();
          continue;
        } else {
          if (matchSeparator(&p, ',')) {
            ParseResult result = parseMixedKey(p);
            p = result.p;
            if (!result.ok) return {false, p};
            break;
          }
          if (!matchSeparator(&p, '}')) return {false, p};
          if (!finishMixed()) return {false, p};
          continue;
        }
      }
    }
  }

  ParseResult parseRue(const char* p) {
    if (*p++ != 'r') return {false, p};
    if (*p++ != 'u') return {false, p};
    if (*p++ != 'e') return {false, p};
    auto const tv = top++;
    tv->m_type = KindOfBoolean;
    tv->m_data.num = true;
    return {true, p};
  }

  ParseResult parseAlse(const char* p) {
    if (*p++ != 'a') return {false, p};
    if (*p++ != 'l') return {false, p};
    if (*p++ != 's') return {false, p};
    if (*p++ != 'e') return {false, p};
    auto const tv = top++;
    tv->m_type = KindOfBoolean;
    tv->m_data.num = false;
    return {true, p};
  }

  ParseResult parseUll(const char* p) {
    if (*p++ != 'u') return {false, p};
    if (*p++ != 'l') return {false, p};
    if (*p++ != 'l') return {false, p};
    top++->m_type = KindOfNull;
    return {true, p};
  }

  ParseResult handleBackslash(const char* p, signed char& out) {
    char ch = *p++;
    switch (ch) {
      case 0: return {false, p};
      case '"': out = ch; return {true, p};
      case '\\': out = ch; return {true, p};
      case '/': out = ch; return {true, p};
      case 'b': out = '\b'; return {true, p};
      case 'f': out = '\f'; return {true, p};
      case 'n': out = '\n'; return {true, p};
      case 'r': out = '\r'; return {true, p};
      case 't': out = '\t'; return {true, p};
      case 'u': {
        if (UNLIKELY(is_tsimplejson)) {
          auto const ch1 = *p++;
          if (UNLIKELY(ch1 != '0')) return {false, p};
          auto const ch2 = *p++;
          if (UNLIKELY(ch2 != '0')) return {false, p};
          auto const dch3 = dehexchar(*p++);
          if (UNLIKELY(dch3 < 0)) return {false, p};
          auto const dch4 = dehexchar(*p++);
          if (UNLIKELY(dch4 < 0)) return {false, p};
          out = (dch3 << 4) | dch4;
          return {true, p};
        } else {
          uint16_t u16cp = 0;
          for (int i = 0; i < 4; i++) {
            auto const hexv = dehexchar(*p++);
            if (hexv < 0) return {false, p}; // includes check for end of string
            u16cp <<= 4;
            u16cp |= hexv;
          }
          if (u16cp > 0x7f) {
            return {false, p};
          } else {
            out = u16cp;
            return {true, p};
          }
        }
      }
      default: return {false, p};
    }
  }

  ParseStringResult parseRawString(const char* p) {
    assertx(p[-1] == '"'); // SimpleParser only handles "-quoted strings
    int len = 0;
    auto const charTop = reinterpret_cast<signed char*>(top);
    for (signed char ch = *p++; ch != '\"'; ch = *p++) {
      charTop[len++] = ch; // overwritten later if `ch == '\\'`
      if (ch < ' ') {
        // `ch < ' '` catches null and also non-ASCII (since signed char)
        return {-1, p};
      } else if (ch == '\\') {
        ParseResult result = handleBackslash(p, charTop[len - 1]);
        p = result.p;
        if (!result.ok) return {-1, p};
      }
    }
    return {len, p};
  }

  ParseResult parseString(const char* p) {
    ParseStringResult result = parseRawString(p);
    p = result.p;
    if (result.len < 0) return {false, p};
    auto const start = reinterpret_cast<char*>(top);
    pushStringData(StringData::Make(start, result.len, CopyString));
    return {true, p};
  }

  ParseResult parseMixedKey(const char* p) {
    if (!matchSeparator(&p, '\"')) return {false, p};
    ParseStringResult result = parseRawString(p);
    p = result.p;
    int len = result.len;
    if (len < 0) return {false, p};
    auto const start = reinterpret_cast<char*>(top);
    auto const slice = folly::StringPiece(start, len);
    start[len] = '\0';
    int64_t num;
    if (container_type != JSONContainerType::HACK_ARRAYS &&
        is_strictly_integer(start, len, num)) {
      pushInt64(num);
    } else if (auto const str = lookupStaticString(slice)) {
      auto const tv = top++;
      tv->m_type = KindOfString;
      tv->m_data.pstr = str;
    } else {
      pushStringData(StringData::Make(start, len, CopyString));
    }
    if (!matchSeparator(&p, ':')) return {false, p};
    return {true, p};
  }

  void pushPacked(TypedValue* fp) {
    auto arr = [&] {
      if (container_type == JSONContainerType::HACK_ARRAYS) {
        return top == fp
          ? staticEmptyVec()
          : VanillaVec::MakeVecNatural(top - fp, fp);
      }
      if (container_type == JSONContainerType::DARRAYS_AND_VARRAYS) {
        return top == fp
          ? staticEmptyVec()
          : VanillaVec::MakeVecNatural(top - fp, fp);
      }
      assertx(container_type == JSONContainerType::DARRAYS);
      return top == fp
        ? staticEmptyDictArray()
        : VanillaDict::MakeDictNatural(top - fp, fp);
    }();
    top = fp;
    pushArrayData(arr);
    check_non_safepoint_surprise();
  }

  bool beginPacked() {
    if (stack_top >= kMaxArrayDepth) return false;
    stack[stack_top++] = StackEntry{top, true};
    return true;
  }

  void finishPacked() {
    assertx(stack_top > 0 && stack[stack_top - 1].is_packed);
    TypedValue* fp = stack[--stack_top].values;
    pushPacked(fp);
  }

  bool pushMixed(TypedValue* fp) {
    auto arr = [&] {
      if (container_type == JSONContainerType::HACK_ARRAYS) {
        return top == fp
          ? staticEmptyDictArray()
          : VanillaDict::MakeDict((top - fp) >> 1, fp)->asArrayData();
      }
      assertx(container_type == JSONContainerType::DARRAYS ||
              container_type == JSONContainerType::DARRAYS_AND_VARRAYS);
      return top == fp
        ? staticEmptyDictArray()
        : VanillaDict::MakeDict((top - fp) >> 1, fp)->asArrayData();
    }();
    // VanillaDict::MakeMixed can return nullptr if there are duplicate keys
    if (!arr) return false;
    top = fp;
    pushArrayData(arr);
    check_non_safepoint_surprise();
    return true;
  }

  bool beginMixed() {
    // Register on stack.
    if (stack_top >= kMaxArrayDepth) return false;
    stack[stack_top++] = StackEntry{top, false};
    return true;
  }

  bool finishMixed() {
    assertx(stack_top > 0 && !stack[stack_top - 1].is_packed);
    TypedValue* fp = stack[--stack_top].values;
    return pushMixed(fp);
  }

  /*
   * Parse remainder of number after initial character firstChar (maybe '-').
   */
  ParseResult parseNumber(const char* p, char firstChar) {
    uint64_t x = 0;
    bool neg = false;
    const char* begin = p - 1;
    if (firstChar == '-') {
      neg = true;
      firstChar = *p;
    } else {
      x = firstChar - '0';  // first digit
    }
    // Parse maximal digit sequence into x (non-negative).
    while (*p >= '0' && *p <= '9') {
      x = (x * 10) + (*p - '0');
      ++p;
    }
    if (*p == '.' || *p == 'e' || *p == 'E') {
      pushDouble(zend_strtod(begin, &p));
      return {true, p};
    }

    auto len = p - begin;

    // JSON does not permit leading 0's in numbers.
    if (UNLIKELY(len > 1 && firstChar == '0')) {
      return {false, p};
    }

    // Now 'x' is the usigned absolute value of a naively parsed integer, but
    // potentially overflowed mod 2^64.
    if (LIKELY(len < 19) || (len == 19 && firstChar <= '8')) {
      int64_t sx = x;
      pushInt64(neg ? -sx : sx);
    } else {
      p = parseBigInt(p, len);
    }
    return {true, p};
  }

  /*
   * Assuming 'len' characters ('0'-'9', maybe prefix '-') have been read,
   * re-parse and push as an int64_t if possible, otherwise as a double.
   */
  const char* parseBigInt(const char* p, int len) {
    assertx(*p > '9' || *p < '0');  // Aleady read maximal digit sequence.
    errno = 0;
    const int64_t sx = strtoll(p - len, nullptr, 10);
    if (errno == ERANGE) {
      const double dval = zend_strtod(p - len, nullptr);
      assertx(dval == floor(dval));
      pushDouble(dval);
    } else {
      pushInt64(sx);
    }
    return p;
  }

  void pushDouble(double data) {
    auto const tv = top++;
    tv->m_type = KindOfDouble;
    tv->m_data.dbl = data;
  }

  void pushInt64(int64_t data) {
    auto const tv = top++;
    tv->m_type = KindOfInt64;
    tv->m_data.num = data;
  }

  void pushStringData(StringData* data) {
    auto const tv = top++;
    tv->m_type = KindOfString;
    tv->m_data.pstr = data;
  }

  void pushArrayData(ArrayData* data) {
    auto const tv = top++;
    tv->m_type = data->toDataType();
    tv->m_data.parr = data;
  }

  TypedValue* top;
  JSONContainerType container_type;
  bool is_tsimplejson;

  int stack_top = 0;
  struct StackEntry {
    TypedValue *values;
    bool is_packed;
  };
  StackEntry stack[kMaxArrayDepth];
};

/*
 * String buffer wrapper that does NOT check its capacity in release mode. User
 * supplies the allocation and must ensure to never append past the end.
 */
struct UncheckedBuffer {
  void clear() { p = begin; }
  // Use given buffer with space for 'cap' chars, including '\0'.
  void setBuf(char* buf, size_t cap) {
    begin = p = buf;
#ifndef NDEBUG
    end = begin + cap;
#endif
  }
  void append(char c) {
    assertx(p < end);
    *p++ = c;
  }
  void shrinkBy(int decrease) {
    p -= decrease;
    assertx(p >= begin);
  }
  int size() { return p - begin; }
  // NUL-terminates the output before returning it, for backward-compatibility.
  char* data() {
    assertx(p < end);
    *p = 0;
    return begin;
  }
  OptString copy() { return OptString(data(), size(), CopyString); }

  char* p{nullptr};
  char* begin{nullptr};
#ifndef NDEBUG
  char* end{nullptr};
#endif
};

}

/**
 * A stack maintains the states of nested structures.
 */
struct json_parser {
  struct json_state {
    Mode mode;
    OptString key;
    Variant val;
  };
  req::vector<json_state> stack;
  int top;
  int mark; // the watermark
  int depth;
  json_error_codes error_code;
  // Thread-local buffer; reused on each call. JSON parsing cannot lead to code
  // execution and is not re-entrant. SimpleParser assumes no surprise checks.
  union {
    TypedValue* tv{nullptr};  // SimpleParser's stack.
    char* raw;                // sb_buf/key
  } tl_buffer;
  TYPE_SCAN_IGNORE_FIELD(tv);
  UncheckedBuffer sb_buf;
  UncheckedBuffer sb_key;
  int sb_cap{0};  // Capacity of each of sb_buf/key.

  void initSb(int length) {
    if (UNLIKELY(length >= sb_cap)) {
      // No decoded string in the output can use more bytes than input size.
      const auto new_cap = length + 1;
      size_t bufSize = length <= Cfg::Eval::SimpleJsonMaxLength ?
        SimpleParser::BufferBytesForLength(length) :
        new_cap * 2;
      if (tl_buffer.raw) {
        json_free(tl_buffer.raw);
        tl_buffer.raw = nullptr;
      }
      sb_cap = 0;
      if (!tl_heap->preAllocOOM(bufSize)) {
        tl_buffer.raw = (char*)json_malloc(bufSize);
        if (!tl_buffer.raw) tl_heap->forceOOM();
      }
      check_non_safepoint_surprise();
      always_assert(tl_buffer.raw);
      sb_buf.setBuf(tl_buffer.raw, new_cap);
      sb_key.setBuf(tl_buffer.raw + new_cap, new_cap);
      // Set new capacity if and ony if allocations succeed.
      sb_cap = new_cap;
    } else {
      sb_buf.clear();
      sb_key.clear();
    }
  }
  void flushSb() {
    if (tl_buffer.raw) {
      json_free(tl_buffer.raw);
      tl_buffer.raw = nullptr;
    }
    sb_cap = 0;
    sb_buf.setBuf(nullptr, 0);
    sb_key.setBuf(nullptr, 0);
  }
 private:
  static void* json_malloc(size_t size) {
    if (Cfg::Eval::JsonParserUseLocalArena) {
      return local_malloc(size);
    } else {
      return malloc(size);
    }
  }
  static void json_free(void* ptr) {
    if (Cfg::Eval::JsonParserUseLocalArena) {
      return local_free(ptr);
    } else {
      return free(ptr);
    }
  }
};

namespace {

RDS_LOCAL(json_parser, s_json_parser);

InitFiniNode r_shutdown(
  [] {
    // Ensure request memory is released
    s_json_parser->stack = decltype(s_json_parser->stack){};
  },
  InitFiniNode::When::RequestFini
);

}

// In Zend, the json_parser struct is publicly
// accessible. Thus the fields could be accessed
// directly. Just using setter/accessor functions
// to get around that.
json_error_codes json_get_last_error_code() {
  return s_json_parser->error_code;
}

void json_set_last_error_code(json_error_codes ec) {
  s_json_parser->error_code = ec;
}

const char *json_get_last_error_msg() {
  switch (s_json_parser->error_code) {
    case JSON_ERROR_NONE:
      return "No error";
    case JSON_ERROR_DEPTH:
      return "Maximum stack depth exceeded";
    case JSON_ERROR_STATE_MISMATCH:
      return "State mismatch (invalid or malformed JSON)";
    case JSON_ERROR_CTRL_CHAR:
      return "Control character error, possibly incorrectly encoded";
    case JSON_ERROR_SYNTAX:
      return "Syntax error";
    case JSON_ERROR_UTF8:
      return "Malformed UTF-8 characters, possibly incorrectly encoded";
    case json_error_codes::JSON_ERROR_RECURSION:
      return "Recursion detected";
    case json_error_codes::JSON_ERROR_INF_OR_NAN:
      return "Inf and NaN cannot be JSON encoded";
    case json_error_codes::JSON_ERROR_UNSUPPORTED_TYPE:
      return "Type is not supported";
    default:
      return "Unknown error";
  }
}

// For each request, make sure we start with the default error code.
void json_parser_init() {
  s_json_parser->error_code = JSON_ERROR_NONE;
}

void json_parser_flush_caches() {
  s_json_parser->flushSb();
}

/**
 * Push a mode onto the stack. Return false if there is overflow.
 */
static int push(json_parser *json, Mode mode) {
  if (json->top + 1 >= json->depth) {
    return false;
  }
  json->top += 1;
  json->stack[json->top].mode = mode;
  if (json->top > json->mark) {
    json->mark = json->top;
  }
  return true;
}


/**
 * Pop the stack, assuring that the current mode matches the expectation.
 * Return false if there is underflow or if the modes mismatch.
 */
static int pop(json_parser *json, Mode mode) {
  if (json->top < 0 || json->stack[json->top].mode != mode) {
    return false;
  }
  json->stack[json->top].mode = Mode::INVALID;
  json->top -= 1;
  return true;
}

static OptString copy_and_clear(UncheckedBuffer &buf) {
  auto ret = buf.size() > 0 ? buf.copy() : String::Empty();
  buf.clear();
  return ret;
}

static Variant to_double(UncheckedBuffer &buf) {
  auto data = buf.data();
  auto ret = data ? zend_strtod(data, nullptr) : 0.0;
  buf.clear();
  return ret;
}

static void json_create_zval(Variant &z, UncheckedBuffer &buf, DataType type,
                             int64_t options) {
  switch (type) {
    case KindOfBoolean:
      z = (buf.data() && (*buf.data() == 't'));
      return;

    case KindOfInt64: {
      bool bigint = false;
      const char *p = buf.data();
      assertx(p);
      if (p == NULL) {
        z = int64_t(0);
        return;
      }

      bool neg = *buf.data() == '-';

      int len = buf.size();
      if (neg) len--;
      if (len >= MAX_LENGTH_OF_LONG - 1) {
        if (len == MAX_LENGTH_OF_LONG - 1) {
          int cmp = strcmp(p + (neg ? 1 : 0), long_min_digits);
          if (!(cmp < 0 || (cmp == 0 && neg))) {
            bigint = true;
          }
        } else {
          bigint = true;
        }
      }

      if (bigint) {
        if (!(options & k_JSON_BIGINT_AS_STRING)) {
          // See KindOfDouble (below)
          z = to_double(buf);
        } else {
          z = copy_and_clear(buf);
        }
      } else {
        z = fast_strtoll_base10(buf.data());
      }
      return;
    }

    case KindOfDouble:
      // Use zend_strtod() instead of strtod() here since JSON specifies using
      // a '.' for decimal separators regardless of locale.
      z = to_double(buf);
      return;

    case KindOfString:
      z = copy_and_clear(buf);
      return;

    case KindOfUninit:
    case KindOfNull:
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
      z = uninit_null();
      return;
  }
  not_reached();
}

NEVER_INLINE
void utf16_to_utf8_tail(UncheckedBuffer &buf, unsigned short utf16) {
  if (utf16 < 0x800) {
    buf.append((char)(0xc0 | (utf16 >> 6)));
    buf.append((char)(0x80 | (utf16 & 0x3f)));
  } else if ((utf16 & 0xfc00) == 0xdc00
             && buf.size() >= 3
             && ((unsigned char)buf.data()[buf.size() - 3]) == 0xed
             && ((unsigned char)buf.data()[buf.size() - 2] & 0xf0) == 0xa0
             && ((unsigned char)buf.data()[buf.size() - 1] & 0xc0) == 0x80) {
    /* found surrogate pair */
    unsigned long utf32;

    utf32 = (((buf.data()[buf.size() - 2] & 0xf) << 16)
             | ((buf.data()[buf.size() - 1] & 0x3f) << 10)
             | (utf16 & 0x3ff)) + 0x10000;
    buf.shrinkBy(3);

    buf.append((char)(0xf0 | (utf32 >> 18)));
    buf.append((char)(0x80 | ((utf32 >> 12) & 0x3f)));
    buf.append((char)(0x80 | ((utf32 >> 6) & 0x3f)));
    buf.append((char)(0x80 | (utf32 & 0x3f)));
  } else {
    buf.append((char)(0xe0 | (utf16 >> 12)));
    buf.append((char)(0x80 | ((utf16 >> 6) & 0x3f)));
    buf.append((char)(0x80 | (utf16 & 0x3f)));
  }
}

ALWAYS_INLINE
void utf16_to_utf8(UncheckedBuffer &buf, unsigned short utf16) {
  if (LIKELY(utf16 < 0x80)) {
    buf.append((char)utf16);
    return;
  }
  return utf16_to_utf8_tail(buf, utf16);
}

StaticString s__empty_("_empty_");

static void object_set(const json_parser* json,
                       Variant &var,
                       const OptString& key,
                       const Variant& value,
                       int assoc,
                       JSONContainerType container_type) {
  if (!assoc) {
    // We know it is stdClass, and everything is public (and dynamic).
    if (key.empty()) {
      var.getObjectData()->setProp(nullctx, s__empty_.get(), *value.asTypedValue());
    } else {
      var.getObjectData()->o_set(key, value);
    }
  } else {
    if (container_type == JSONContainerType::COLLECTIONS) {
      auto keyTV = make_tv<KindOfString>(key.get());
      collections::set(var.getObjectData(), &keyTV, value.asTypedValue());
    } else if (container_type == JSONContainerType::HACK_ARRAYS) {
      forceToDict(var).set(key, value);
    } else {
      int64_t i;
      if (key.get()->isStrictlyInteger(i)) {
        forceToDict(var).set(i, value);
      } else {
        forceToDict(var).set(key, value);
      }
    }
  }
}

static void attach_zval(json_parser *json,
                        const OptString& key,
                        int assoc,
                        JSONContainerType container_type) {
  if (json->top < 1) {
    return;
  }

  auto& root = json->stack[json->top - 1].val;
  auto& child =  json->stack[json->top].val;
  auto up_mode = json->stack[json->top - 1].mode;

  if (up_mode == Mode::ARRAY) {
    appendToContainer(container_type, root, child);
  } else if (up_mode == Mode::OBJECT) {
    object_set(json, root, key, child, assoc, container_type);
  }
}

JSONContainerType get_container_type_from_options(int64_t options) {
  if ((options & k_JSON_FB_STABLE_MAPS) ||
      (options & k_JSON_FB_COLLECTIONS)) {
    return JSONContainerType::COLLECTIONS;
  }

  if (options & k_JSON_FB_HACK_ARRAYS) {
    return JSONContainerType::HACK_ARRAYS;
  }

  if (options & k_JSON_FB_DARRAYS) {
    return JSONContainerType::DARRAYS;
  }

  if (options & k_JSON_FB_DARRAYS_AND_VARRAYS) {
    return JSONContainerType::DARRAYS_AND_VARRAYS;
  }

  return JSONContainerType::DARRAYS;
}

/**
 * The JSON_parser takes a UTF-8 encoded string and determines if it is a
 * syntactically correct JSON text. Along the way, it creates a PHP variable.
 *
 * It is implemented as a Pushdown Automaton; that means it is a finite state
 * machine with a stack.
 *
 * The behavior is as follows:
 * Container Type       | is_assoc | JSON input => output type
 *
 * COLLECTIONS          | true     | "{}"       => c_Map
 * COLLECTIONS          | false    | "{}"       => c_Map
 * COLLECTIONS          | true     | "[]"       => c_Vector
 * COLLECTIONS          | false    | "[]"       => c_Vector
 *
 * HACK_ARRAYS          | true     | "{}"       => dict
 * HACK_ARRAYS          | false    | "{}"       => stdClass
 * HACK_ARRAYS          | true     | "[]"       => vec
 * HACK_ARRAYS          | false    | "[]"       => stdClass
 *
 * DARRAYS              | true     | "{}"       => darray
 * DARRAYS              | false    | "{}"       => stdClass
 * DARRAYS              | true     | "[]"       => darray
 * DARRAYS              | false    | "[]"       => stdClass
 *
 * DARRAYS_AND_VARRAYS  | true     | "{}"       => darray
 * DARRAYS_AND_VARRAYS  | false    | "{}"       => stdClass
 * DARRAYS_AND_VARRAYS  | true     | "[]"       => varray
 * DARRAYS_AND_VARRAYS  | false    | "[]"       => stdClass
 */
bool JSON_parser(Variant &z, const char *p, int length, bool const assoc,
                 int depth, int64_t options) {
  // No GC safepoints during JSON parsing, please. Code is not re-entrant.
  NoHandleSurpriseScope no_surprise(SafepointFlags);

  json_parser *json = s_json_parser.get(); /* the parser state */
  // Clear and reuse the thread-local string buffers. They are only freed if
  // they exceed kMaxPersistentStringBufferCapacity at exit or if the thread
  // is explicitly flushed (e.g., due to being idle).
  json->initSb(length);
  if (depth <= 0) {
    json->error_code = json_error_codes::JSON_ERROR_DEPTH;
    return false;
  }
  SCOPE_EXIT {
    constexpr int kMaxPersistentStringBufferCapacity = 256 * 1024;
    if (json->sb_cap > kMaxPersistentStringBufferCapacity) json->flushSb();
  };
  // SimpleParser only handles the most common set of options. Also, only use it
  // if its array nesting depth check is *more* restrictive than what the user
  // asks for, to ensure that the precise semantics of the general case is
  // applied for all nesting overflows.
  if (assoc &&
      options == (options & (k_JSON_FB_LOOSE |
                             k_JSON_FB_DARRAYS |
                             k_JSON_FB_DARRAYS_AND_VARRAYS |
                             k_JSON_FB_HACK_ARRAYS |
                             k_JSON_FB_THRIFT_SIMPLE_JSON)) &&
      depth >= SimpleParser::kMaxArrayDepth &&
      length <= Cfg::Eval::SimpleJsonMaxLength &&
      SimpleParser::TryParse(p, length, json->tl_buffer.tv, z,
                             get_container_type_from_options(options),
                             options & k_JSON_FB_THRIFT_SIMPLE_JSON)) {
    return true;
  }

  int b;  /* the next character */
  int c;  /* the next character class */
  int s;  /* the next state */
  int state = 0;

  /*<fb>*/
  bool const loose = options & k_JSON_FB_LOOSE;
  JSONContainerType const container_type =
    get_container_type_from_options(options);
  int qchr = 0;
  int8_t const *byte_class;
  int8_t const (*next_state_table)[32];
  if (loose) {
    byte_class = loose_ascii_class;
    next_state_table = loose_state_transition_table;
  } else {
    byte_class = ascii_class;
    next_state_table = state_transition_table;
  }
  /*</fb>*/

  UncheckedBuffer *buf = &json->sb_buf;
  UncheckedBuffer *key = &json->sb_key;

  DataType type = kInvalidDataType;
  unsigned short escaped_bytes = 0;

  auto reset_type = [&] { type = kInvalidDataType; };

  json->depth = depth;
  // Since the stack is maintainined on a per request basis, for performance
  // reasons, it only makes sense to expand if necessary and cycles are wasted
  // contracting. Calls with a depth other than default should be rare.
  if (depth > json->stack.size()) {
    auto const newSize = sizeof(json_parser::json_state) * depth;
    if (newSize > kMaxSmallSize && tl_heap->preAllocOOM(newSize)) {
      check_non_safepoint_surprise();
    }
    json->stack.resize(depth);
  }
  SCOPE_EXIT {
    if (json->stack.empty()) return;
    for (int i = 0; i <= json->mark; i++) {
      json->stack[i].key.reset();
      json->stack[i].val.unset();
    }
    json->mark = -1;
  };

  json->mark = json->top = -1;
  push(json, Mode::DONE);

  UTF8To16Decoder decoder(p, length, loose);
  for (;;) {
    b = decoder.decode();
    // Fast-case most common transition: append a simple string character.
    if (state == 3 && type == KindOfString) {
      while (b != '\"' &&  b != '\\' && b != '\'' && b <= 127 && b >= ' ') {
        buf->append((char)b);
        b = decoder.decode();
      }
    }
    if (b == UTF8_END) break; // UTF-8 decoding finishes successfully.
    if (b == UTF8_ERROR) {
      s_json_parser->error_code = JSON_ERROR_UTF8;
      return false;
    }
    assertx(b >= 0);

    if ((b & 127) == b) {
      /*<fb>*/
      c = byte_class[b];
      /*</fb>*/
      if (c <= S_ERR) {
        s_json_parser->error_code = JSON_ERROR_CTRL_CHAR;
        return false;
      }
    } else {
      c = S_ETC;
    }
    /*
      Get the next state from the transition table.
    */

    /*<fb>*/
    s = next_state_table[state][c];

    if (s == -4) {
      if (b != qchr) {
        s = 3;
      } else {
        qchr = 0;
      }
    }
    /*</fb>*/

    if (s < 0) {
      /*
        Perform one of the predefined actions.
      */
      switch (s) {
        /*
          empty }
        */
      case -9:
        /*<fb>*/
        if (json->top == 1) z = json->stack[json->top].val;
        else {
        /*</fb>*/
          attach_zval(json, json->stack[json->top].key, assoc, container_type);
        /*<fb>*/
        }
        /*</fb>*/
        if (!pop(json, Mode::KEY)) {
          return false;
        }
        state = 9;
        break;
        /*
          {
        */
      case -8:
        if (!push(json, Mode::KEY)) {
          s_json_parser->error_code = JSON_ERROR_DEPTH;
          return false;
        }

        state = 1;
        if (json->top > 0) {
          Variant &top = json->stack[json->top].val;
          /*<fb>*/
          if (container_type == JSONContainerType::COLLECTIONS) {
            // stable_maps is meaningless
            top = req::make<c_Map>();
          } else {
          /*</fb>*/
            if (!assoc) {
              top = SystemLib::AllocStdClassObject();
            /* <fb> */
            } else if (container_type == JSONContainerType::HACK_ARRAYS) {
              top = Array::CreateDict();
            } else if (container_type == JSONContainerType::DARRAYS ||
                       container_type == JSONContainerType::DARRAYS_AND_VARRAYS)
            {
              top = Array::CreateDict();
            /* </fb> */
            } else {
              top = Array::CreateDict();
            }
          /*<fb>*/
          }
          /*</fb>*/
          json->stack[json->top].key = copy_and_clear(*key);
          reset_type();
        }
        break;
        /*
          }
        */
      case -7:
        /*** BEGIN Facebook: json_utf8_loose ***/
        /*
          If this is a trailing comma in an object definition,
          we're in Mode::KEY. In that case, throw that off the
          stack and restore Mode::OBJECT so that we pretend the
          trailing comma just didn't happen.
        */
        if (loose) {
          if (pop(json, Mode::KEY)) {
            push(json, Mode::OBJECT);
          }
        }
        /*** END Facebook: json_utf8_loose ***/

        if (type != kInvalidDataType &&
            json->stack[json->top].mode == Mode::OBJECT) {
          Variant mval;
          json_create_zval(mval, *buf, type, options);
          Variant &top = json->stack[json->top].val;
          object_set(json, top, copy_and_clear(*key),
                     mval, assoc, container_type);
          buf->clear();
          reset_type();
        }

        /*<fb>*/
        if (json->top == 1) z = json->stack[json->top].val;
        else {
        /*</fb>*/
          attach_zval(json, json->stack[json->top].key,
            assoc, container_type);
        /*<fb>*/
        }
        /*</fb>*/
        if (!pop(json, Mode::OBJECT)) {
          s_json_parser->error_code = JSON_ERROR_STATE_MISMATCH;
          return false;
        }
        state = 9;
        break;
        /*
          [
        */
      case -6:
        if (!push(json, Mode::ARRAY)) {
          s_json_parser->error_code = JSON_ERROR_DEPTH;
          return false;
        }
        state = 2;

        if (json->top > 0) {
          Variant &top = json->stack[json->top].val;
          /*<fb>*/
          if (container_type == JSONContainerType::COLLECTIONS) {
            top = req::make<c_Vector>();
          } else if (container_type == JSONContainerType::HACK_ARRAYS) {
            top = Array::CreateVec();
          } else if (container_type == JSONContainerType::DARRAYS_AND_VARRAYS) {
            top = Array::CreateVec();
          } else if (container_type == JSONContainerType::DARRAYS) {
            top = Array::CreateDict();
          } else {
            top = Array::CreateDict();
          }
          /*</fb>*/
          json->stack[json->top].key = copy_and_clear(*key);
          reset_type();
        }
        break;
        /*
          ]
        */
      case -5:
        {
          if (type != kInvalidDataType &&
               json->stack[json->top].mode == Mode::ARRAY) {
            Variant mval;
            json_create_zval(mval, *buf, type, options);
            auto& top = json->stack[json->top].val;
            appendToContainer(container_type, top, mval);
            buf->clear();
            reset_type();
          }

          /*<fb>*/
          if (json->top == 1) z = json->stack[json->top].val;
          else {
          /*</fb>*/
            attach_zval(json, json->stack[json->top].key, assoc,
              container_type);
          /*<fb>*/
          }
          /*</fb>*/
          if (!pop(json, Mode::ARRAY)) {
            s_json_parser->error_code = JSON_ERROR_STATE_MISMATCH;
            return false;
          }
          state = 9;
        }
        break;
        /*
          "
        */
      case -4:
        switch (json->stack[json->top].mode) {
        case Mode::KEY:
          state = 27;
          std::swap(buf, key);
          reset_type();
          break;
        case Mode::ARRAY:
        case Mode::OBJECT:
          state = 9;
          break;
        case Mode::DONE:
          if (type == KindOfString) {
            z = copy_and_clear(*buf);
            state = 9;
            break;
          }
          [[fallthrough]];
        default:
          s_json_parser->error_code = JSON_ERROR_SYNTAX;
          return false;
        }
        break;
        /*
          ,
        */
      case -3:
        {
          Variant mval;
          if (type != kInvalidDataType &&
              (json->stack[json->top].mode == Mode::OBJECT ||
               json->stack[json->top].mode == Mode::ARRAY)) {
            json_create_zval(mval, *buf, type, options);
          }

          switch (json->stack[json->top].mode) {
          case Mode::OBJECT:
            if (pop(json, Mode::OBJECT) &&
                push(json, Mode::KEY)) {
              if (type != kInvalidDataType) {
                Variant &top = json->stack[json->top].val;
                object_set(
                  json,
                  top,
                  copy_and_clear(*key),
                  mval,
                  assoc,
                  container_type
                );
              }
              state = 29;
            }
            break;
          case Mode::ARRAY:
            if (type != kInvalidDataType) {
              auto& top = json->stack[json->top].val;
              appendToContainer(container_type, top, mval);
            }
            state = 28;
            break;
          default:
            s_json_parser->error_code = JSON_ERROR_SYNTAX;
            return false;
          }
          buf->clear();
          reset_type();
          check_non_safepoint_surprise();
        }
        break;

        /*<fb>*/
        /*
          : (after unquoted string)
        */
      case -10:
        if (json->stack[json->top].mode == Mode::KEY) {
          state = 27;
          std::swap(buf, key);
          reset_type();
          s = -2;
        } else {
          s = 3;
          break;
        }
        /*</fb>*/
        [[fallthrough]];

        /*
          :
        */
      case -2:
        if (pop(json, Mode::KEY) && push(json, Mode::OBJECT)) {
          state = 28;
          break;
        }
        [[fallthrough]];
        /*
          syntax error
        */
      case -1:
        s_json_parser->error_code = JSON_ERROR_SYNTAX;
        return false;
      }
    } else {
      /*
        Change the state and iterate.
      */
      bool is_tsimplejson = options & k_JSON_FB_THRIFT_SIMPLE_JSON;
      if (type == KindOfString) {
        if (/*<fb>*/(/*</fb>*/s == 3/*<fb>*/ || s == 30)/*</fb>*/ &&
            state != 8) {
          if (state != 4) {
            utf16_to_utf8(*buf, b);
          } else {
            switch (b) {
            case 'b': buf->append('\b'); break;
            case 't': buf->append('\t'); break;
            case 'n': buf->append('\n'); break;
            case 'f': buf->append('\f'); break;
            case 'r': buf->append('\r'); break;
            default:
              utf16_to_utf8(*buf, b);
              break;
            }
          }
        } else if (s == 6) {
          if (UNLIKELY(is_tsimplejson)) {
            if (UNLIKELY(b != '0'))  {
              s_json_parser->error_code = JSON_ERROR_SYNTAX;
              return false;
            }
            escaped_bytes = 0;
          } else {
            escaped_bytes = dehexchar(b) << 12;
          }
        } else if (s == 7) {
          if (UNLIKELY(is_tsimplejson)) {
            if (UNLIKELY(b != '0'))  {
              s_json_parser->error_code = JSON_ERROR_SYNTAX;
              return false;
            }
          } else {
            escaped_bytes += dehexchar(b) << 8;
          }
        } else if (s == 8) {
          escaped_bytes += dehexchar(b) << 4;
        } else if (s == 3 && state == 8) {
          escaped_bytes += dehexchar(b);
          if (UNLIKELY(is_tsimplejson)) {
            buf->append((char)escaped_bytes);
          } else {
            utf16_to_utf8(*buf, escaped_bytes);
          }
        }
      } else if ((type == kInvalidDataType || type == KindOfNull) &&
                 (c == S_DIG || c == S_ZER)) {
        type = KindOfInt64;
        buf->append((char)b);
      } else if (type == KindOfInt64 && s == 24) {
        type = KindOfDouble;
        buf->append((char)b);
      } else if ((type == kInvalidDataType || type == KindOfNull ||
                  type == KindOfInt64) &&
                 c == S_DOT) {
        type = KindOfDouble;
        buf->append((char)b);
      } else if (type != KindOfString && c == S_QUO) {
        type = KindOfString;
        /*<fb>*/qchr = b;/*</fb>*/
      } else if ((type == kInvalidDataType || type == KindOfNull ||
                  type == KindOfInt64 || type == KindOfDouble) &&
                 ((state == 12 && s == 9) ||
                  (state == 16 && s == 9))) {
        type = KindOfBoolean;
      } else if (type == kInvalidDataType && state == 19 && s == 9) {
        type = KindOfNull;
      } else if (type != KindOfString && c > S_WSP) {
        utf16_to_utf8(*buf, b);
      }

      state = s;
    }
  }

  if (state == 9 && pop(json, Mode::DONE)) {
    s_json_parser->error_code = JSON_ERROR_NONE;
    return true;
  }

  s_json_parser->error_code = JSON_ERROR_SYNTAX;
  return false;
}
}
