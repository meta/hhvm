/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1998-2010 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
*/
#include "hphp/zend/zend-html.h"

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include "hphp/util/lock.h"
#include "hphp/util/functional.h"
#include "hphp/util/hash-map.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////
// UTF-8 entity tables

using namespace entity_charset_enum;

static entity_table_t ent_cp_866[] = {
  "Acy", "Bcy", "Vcy", "Gcy", "Dcy", "IEcy", "ZHcy", "Zcy",
  "Icy", "Jcy", "Kcy", "Lcy", "Mcy", "Ncy", "Ocy", "Pcy",
  "Rcy", "Scy", "Tcy", "Ucy", "Fcy", "KHcy", "TScy", "CHcy",
  "SHcy", "SHCHcy", "HARDcy", "Ycy", "SOFTcy", "Ecy", "YUcy", "YAcy",
  "acy", "bcy", "vcy", "gcy", "dcy", "iecy", "zhcy", "zcy",
  "icy", "jcy", "kcy", "lcy", "mcy", "ncy", "ocy", "pcy",
  "blk14", "blk12", "blk34", "boxv", "boxvl", "boxvL", "boxVl", "boxDl",
  "boxdL", "boxVL", "boxV", "boxDL", "boxUL", "boxUl", "boxuL", "boxdl",
  "boxur", "boxhu", "boxhd", "boxvr", "boxh", "boxvh", "boxvR", "boxVr",
  "boxUR", "boxDR", "boxHU", "boxHD", "boxVR", "boxH", "boxVH", "boxHu",
  "boxhU", "boxHd", "boxhD", "boxUr", "boxuR", "boxdR", "boxDr", "boxVh",
  "boxvH", "boxul", "boxdr", "block", "lhblk", nullptr, nullptr, "uhblk",
  "rcy", "scy", "tcy", "ucy", "fcy", "khcy", "tscy", "chcy",
  "shcy", "shchcy", "hardcy", "ycy", "softcy", "ecy", "yucy", "yacy",
  "IOcy", "iocy", "Jukcy", "jukcy", "YIcy", "yicy", "Ubrcy", "ubrcy",
  "deg", nullptr, "middot", "Sqrt", "numero", "curren", nullptr, "nbsp"
};

static entity_table_t ent_cp_1251[] = {
  "DJcy", "GJcy", "sbquo", "gjcy", "bdquo", "hellip", "dagger", "Dagger",
  "euro", "permil", "LJcy", "lsaquo", "NJcy", "KJcy", "TSHcy", "DZcy",
  "djcy", "lsquo", "rsquo", "ldquo", "rdquo", "bull", "ndash", "mdash",
  nullptr, "trade", "ljcy", "rsaquo", "njcy", "kjcy", "tshcy", "dzcy",
  "nbsp", "Ubrcy", "ubrcy", "Jsercy", "curren", nullptr, "brvbar", "sect",
  "IOcy", "copy", "Jukcy", "laquo", "not", "shy", "reg", "YIcy",
  "deg", "pm", "Iukcy", "iukcy", nullptr, "micro", "para", "middot",
  "iocy", "numero", "jukcy", "raquo", "jsercy", "DScy", "dscy", "yicy",
  "Acy", "Bcy", "Vcy", "Gcy", "Dcy", "IEcy", "ZHcy", "Zcy",
  "Icy", "Jcy", "Kcy", "Lcy", "Mcy", "Ncy", "Ocy", "Pcy",
  "Rcy", "Scy", "Tcy", "Ucy", "Fcy", "KHcy", "TScy", "CHcy",
  "SHcy", "SHCHcy", "HARDcy", "Ycy", "SOFTcy", "Ecy", "YUcy", "YAcy",
  "acy", "bcy", "vcy", "gcy", "dcy", "iecy", "zhcy", "zcy",
  "icy", "jcy", "kcy", "lcy", "mcy", "ncy", "ocy", "pcy",
  "rcy", "scy", "tcy", "ucy", "fcy", "khcy", "tscy", "chcy",
  "shcy", "shchcy", "hardcy", "ycy", "softcy", "ecy", "yucy", "yacy"
};

/* codepage 1252 is a Windows extension to iso-8859-1. */
static entity_table_t ent_cp_1252[] = {
  "euro", nullptr, "sbquo", "fnof", "bdquo", "hellip", "dagger",
  "Dagger", "circ", "permil", "Scaron", "lsaquo", "OElig",
  nullptr, nullptr, nullptr, nullptr, "lsquo", "rsquo", "ldquo", "rdquo",
  "bull", "ndash", "mdash", "tilde", "trade", "scaron", "rsaquo",
  "oelig", nullptr, nullptr, "Yuml"
};

static entity_table_t ent_iso_8859_1[] = {
  "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar",
  "sect", "uml", "copy", "ordf", "laquo", "not", "shy", "reg",
  "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro",
  "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14",
  "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",
  "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave",
  "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc",
  "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde",
  "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml",
  "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc",
  "atilde", "auml", "aring", "aelig", "ccedil", "egrave",
  "eacute", "ecirc", "euml", "igrave", "iacute", "icirc",
  "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde",
  "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",
  "uuml", "yacute", "thorn", "yuml"
};

static entity_table_t ent_iso_8859_5[] = {
  "nbsp", "IOcy", "DJcy", "GJcy", "Jukcy", "DScy", "Iukcy", "YIcy",
  "Jsercy", "LJcy", "NJcy", "TSHcy", "KJcy", "shy", "Ubrcy", "DZcy",
  "Acy", "Bcy", "Vcy", "Gcy", "Dcy", "IEcy", "ZHcy", "Zcy",
  "Icy", "Jcy", "Kcy", "Lcy", "Mcy", "Ncy", "Ocy", "Pcy",
  "Rcy", "Scy", "Tcy", "Ucy", "Fcy", "KHcy", "TScy", "CHcy",
  "SHcy", "SHCHcy", "HARDcy", "Ycy", "SOFTcy", "Ecy", "YUcy", "YAcy",
  "acy", "bcy", "vcy", "gcy", "dcy", "iecy", "zhcy", "zcy",
  "icy", "jcy", "kcy", "lcy", "mcy", "ncy", "ocy", "pcy",
  "rcy", "scy", "tcy", "ucy", "fcy", "khcy", "tscy", "chcy",
  "shcy", "shchcy", "hardcy", "ycy", "softcy", "ecy", "yucy", "yacy",
  "numero", "iocy", "djcy", "gjcy", "jukcy", "dscy", "iukcy", "yicy",
  "jsercy", "ljcy", "njcy", "tshcy", "kjcy", "sect", "ubrcy", "dzcy"
};

static entity_table_t ent_iso_8859_15[] = {
  "nbsp", "iexcl", "cent", "pound", "euro", "yen", "Scaron",
  "sect", "scaron", "copy", "ordf", "laquo", "not", "shy", "reg",
  "macr", "deg", "plusmn", "sup2", "sup3", nullptr, /* Zcaron */
  "micro", "para", "middot", nullptr, /* zcaron */ "sup1", "ordm",
  "raquo", "OElig", "oelig", "Yuml", "iquest", "Agrave", "Aacute",
  "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave",
  "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc",
  "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde",
  "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml",
  "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc",
  "atilde", "auml", "aring", "aelig", "ccedil", "egrave",
  "eacute", "ecirc", "euml", "igrave", "iacute", "icirc",
  "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde",
  "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",
  "uuml", "yacute", "thorn", "yuml"
};

static entity_table_t ent_uni_338_402[] = {
  /* 338 (0x0152) */
  "OElig", "oelig", nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 352 (0x0160) */
  "Scaron", "scaron", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 376 (0x0178) */
  "Yuml", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 400 (0x0190) */
  nullptr, nullptr, "fnof"
};

static entity_table_t ent_uni_spacing[] = {
  /* 710 */
  "circ",
  /* 711 - 730 */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 731 - 732 */
  nullptr, "tilde"
};

static entity_table_t ent_uni_greek[] = {
  /* 913 */
  "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta",
  "Iota", "Kappa", "Lambda", "Mu", "Nu", "Xi", "Omicron", "Pi", "Rho",
  nullptr, "Sigma", "Tau", "Upsilon", "Phi", "Chi", "Psi", "Omega",
  /* 938 - 944 are not mapped */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
  "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi", "rho",
  "sigmaf", "sigma", "tau", "upsilon", "phi", "chi", "psi", "omega",
  /* 970 - 976 are not mapped */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  "thetasym", "upsih",
  nullptr, nullptr, nullptr,
  "piv"
};

static entity_table_t ent_uni_punct[] = {
  /* 8194 */
  "ensp", "emsp", nullptr, nullptr, nullptr, nullptr, nullptr,
  "thinsp", nullptr, nullptr, "zwnj", "zwj", "lrm", "rlm",
  nullptr, nullptr, nullptr, "ndash", "mdash", nullptr, nullptr, nullptr,
  /* 8216 */
  "lsquo", "rsquo", "sbquo", nullptr, "ldquo", "rdquo", "bdquo", nullptr,
  "dagger", "Dagger", "bull", nullptr, nullptr, nullptr, "hellip",
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "permil", nullptr,
  /* 8242 */
  "prime", "Prime", nullptr, nullptr, nullptr, nullptr, nullptr, "lsaquo", "rsaquo", nullptr,
  nullptr, nullptr, "oline", nullptr, nullptr, nullptr, nullptr, nullptr,
  "frasl"
};

static entity_table_t ent_uni_euro[] = {
  "euro"
};

static entity_table_t ent_uni_8465_8501[] = {
  /* 8465 */
  "image", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8472 */
  "weierp", nullptr, nullptr, nullptr,
  /* 8476 */
  "real", nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8482 */
  "trade", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8501 */
  "alefsym",
};

static entity_table_t ent_uni_8592_9002[] = {
  /* 8592 (0x2190) */
  "larr", "uarr", "rarr", "darr", "harr", nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8608 (0x21a0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8624 (0x21b0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, "crarr", nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8640 (0x21c0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8656 (0x21d0) */
  "lArr", "uArr", "rArr", "dArr", "hArr", "vArr", nullptr, nullptr,
  nullptr, nullptr, "lAarr", "rAarr", nullptr, "rarrw", nullptr, nullptr,
  /* 8672 (0x21e0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8704 (0x2200) */
  "forall", "comp", "part", "exist", "nexist", "empty", nullptr, "nabla",
  "isin", "notin", "epsis", "ni", "notni", "bepsi", nullptr, "prod",
  /* 8720 (0x2210) */
  "coprod", "sum", "minus", "mnplus", "plusdo", nullptr, "setmn", "lowast",
  "compfn", nullptr, "radic", nullptr, nullptr, "prop", "infin", "ang90",
  /* 8736 (0x2220) */
  "ang", "angmsd", "angsph", "mid", "nmid", "par", "npar", "and",
  "or", "cap", "cup", "int", nullptr, nullptr, "conint", nullptr,
  /* 8752 (0x2230) */
  nullptr, nullptr, nullptr, nullptr, "there4", "becaus", nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, "sim", "bsim", nullptr, nullptr,
  /* 8768 (0x2240) */
  "wreath", "nsim", nullptr, "sime", "nsime", "cong", nullptr, "ncong",
  "asymp", "nap", "ape", nullptr, "bcong", "asymp", "bump", "bumpe",
  /* 8784 (0x2250) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8800 (0x2260) */
  "ne", "equiv", nullptr, nullptr, "le", "ge", "lE", "gE",
  "lnE", "gnE", "Lt", "Gt", "twixt", nullptr, "nlt", "ngt",
  /* 8816 (0x2270) */
  "nles", "nges", "lsim", "gsim", nullptr, nullptr, "lg", "gl",
  nullptr, nullptr, "pr", "sc", "cupre", "sscue", "prsim", "scsim",
  /* 8832 (0x2280) */
  "npr", "nsc", "sub", "sup", "nsub", "nsup", "sube", "supe",
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8848 (0x2290) */
  nullptr, nullptr, nullptr, nullptr, nullptr, "oplus", nullptr, "otimes",
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8864 (0x22a0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, "perp", nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8880 (0x22b0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8896 (0x22c0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, "sdot", nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8912 (0x22d0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8928 (0x22e0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8944 (0x22f0) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8960 (0x2300) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  "lceil", "rceil", "lfloor", "rfloor", nullptr, nullptr, nullptr, nullptr,
  /* 8976 (0x2310) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  /* 8992 (0x2320) */
  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
  nullptr, "lang", "rang"
};

static entity_table_t ent_uni_9674[] = {
  /* 9674 */
  "loz"
};

static entity_table_t ent_uni_9824_9830[] = {
  /* 9824 */
  "spades", nullptr, nullptr, "clubs", nullptr, "hearts", "diams"
};

static const struct html_entity_map entity_map[] = {
  { cs_cp866,     0x80, 0xff, ent_cp_866 },
  { cs_cp1251,    0x80, 0xff, ent_cp_1251 },
  { cs_cp1252,    0x80, 0x9f, ent_cp_1252 },
  { cs_cp1252,    0xa0, 0xff, ent_iso_8859_1 },
  { cs_8859_1,    0xa0, 0xff, ent_iso_8859_1 },
  { cs_8859_5,    0xa0, 0xff, ent_iso_8859_5 },
  { cs_8859_15,   0xa0, 0xff, ent_iso_8859_15 },
  { cs_utf_8,     0xa0, 0xff, ent_iso_8859_1 },
  { cs_utf_8,     338,  402,  ent_uni_338_402 },
  { cs_utf_8,     710,  732,  ent_uni_spacing },
  { cs_utf_8,     913,  982,  ent_uni_greek },
  { cs_utf_8,     8194, 8260, ent_uni_punct },
  { cs_utf_8,     8364, 8364, ent_uni_euro },
  { cs_utf_8,     8465, 8501, ent_uni_8465_8501 },
  { cs_utf_8,     8592, 9002, ent_uni_8592_9002 },
  { cs_utf_8,     9674, 9674, ent_uni_9674 },
  { cs_utf_8,     9824, 9830, ent_uni_9824_9830 },
  { cs_big5,      0xa0, 0xff, ent_iso_8859_1 },
  { cs_gb2312,    0xa0, 0xff, ent_iso_8859_1 },
  { cs_big5hkscs, 0xa0, 0xff, ent_iso_8859_1 },
  { cs_sjis,      0xa0, 0xff, ent_iso_8859_1 },
  { cs_eucjp,     0xa0, 0xff, ent_iso_8859_1 },
  /* Missing support for these at the moment
  { cs_koi8r,     0xa3, 0xff, ent_koi8r },
  { cs_macroman,  0x0b, 0xff, ent_macroman },
  */
  { cs_terminator }
};

static const struct {
  const char *codeset;
  entity_charset charset;
} charset_map[] = {
  { "ISO-8859-1",     cs_8859_1 },
  { "ISO8859-1",      cs_8859_1 },
  { "ISO-8859-5",     cs_8859_5 },
  { "ISO8859-5",      cs_8859_5 },
  { "ISO-8859-15",    cs_8859_15 },
  { "ISO8859-15",     cs_8859_15 },
  { "utf-8",          cs_utf_8 },
  { "cp866",          cs_cp866 },
  { "866",            cs_cp866 },
  { "ibm866",         cs_cp866 },
  { "cp1251",         cs_cp1251 },
  { "Windows-1251",   cs_cp1251 },
  { "win-1251",       cs_cp1251 },
  { "cp1252",         cs_cp1252 },
  { "Windows-1252",   cs_cp1252 },
  { "1252",           cs_cp1252 },
  { "BIG5",           cs_big5 },
  { "950",            cs_big5 },
  { "GB2312",         cs_gb2312 },
  { "936",            cs_gb2312 },
  { "BIG5-HKSCS",     cs_big5hkscs },
  { "Shift_JIS",      cs_sjis },
  { "SJIS",           cs_sjis },
  { "932",            cs_sjis },
  { "EUCJP",          cs_eucjp },
  /* Missing support for these at the moment
  { "EUC-JP",         cs_eucjp },
  { "KOI8-R",         cs_koi8r },
  { "koi8-ru",        cs_koi8r },
  { "koi8r",          cs_koi8r },
  { "MacRoman",       cs_macroman },
  */
  { nullptr }
};

///////////////////////////////////////////////////////////////////////////////

entity_charset determine_charset(const char *charset_hint) {
  entity_charset charset = cs_unknown;

  if (charset_hint == nullptr) {
    // default to utf-8
    return cs_utf_8;
  }

  size_t len = strlen(charset_hint);

  /* now walk the charset map and look for the codeset */
  for (int i = 0; charset_map[i].codeset; i++) {
    if (len == strlen(charset_map[i].codeset) &&
      strncasecmp(charset_hint, charset_map[i].codeset, len) == 0) {
      charset = charset_map[i].charset;
      break;
    }
  }

  return charset;
}

static int utf32_to_utf8(unsigned char *buf, int k) {
  int retval = 0;

  if (k < 0x80) {
    buf[0] = k;
    retval = 1;
  } else if (k < 0x800) {
    buf[0] = 0xc0 | (k >> 6);
    buf[1] = 0x80 | (k & 0x3f);
    retval = 2;
  } else if (k < 0x10000) {
    buf[0] = 0xe0 | (k >> 12);
    buf[1] = 0x80 | ((k >> 6) & 0x3f);
    buf[2] = 0x80 | (k & 0x3f);
    retval = 3;
  } else if (k < 0x200000) {
    buf[0] = 0xf0 | (k >> 18);
    buf[1] = 0x80 | ((k >> 12) & 0x3f);
    buf[2] = 0x80 | ((k >> 6) & 0x3f);
    buf[3] = 0x80 | (k & 0x3f);
    retval = 4;
  } else if (k < 0x4000000) {
    buf[0] = 0xf8 | (k >> 24);
    buf[1] = 0x80 | ((k >> 18) & 0x3f);
    buf[2] = 0x80 | ((k >> 12) & 0x3f);
    buf[3] = 0x80 | ((k >> 6) & 0x3f);
    buf[4] = 0x80 | (k & 0x3f);
    retval = 5;
  } else {
    buf[0] = 0xfc | (k >> 30);
    buf[1] = 0x80 | ((k >> 24) & 0x3f);
    buf[2] = 0x80 | ((k >> 18) & 0x3f);
    buf[3] = 0x80 | ((k >> 12) & 0x3f);
    buf[4] = 0x80 | ((k >> 6) & 0x3f);
    buf[5] = 0x80 | (k & 0x3f);
    retval = 6;
  }
  buf[retval] = '\0';

  return retval;
}

using HtmlEntityMap = hphp_const_char_map<std::string>;

static volatile bool EntityMapInited = false;
static Mutex EntityMapMutex;
static HtmlEntityMap EntityMap[cs_end];
static HtmlEntityMap XHPEntityMap[cs_end];

static void init_entity_table() {
  for (unsigned int i = 0; entity_map[i].charset != cs_terminator; i++) {
    const html_entity_map &em = entity_map[i];
    const entity_charset charset = entity_map[i].charset;

    int index = 0;
    for (int ch = em.basechar; ch <= em.endchar; ch++, index++) {
      const char *entity = em.table[index];
      if (entity == nullptr) {
        continue;
      }
      unsigned char buf[10];
      switch (charset) {
        case cs_8859_1:
        case cs_cp1252:
        case cs_8859_15:
        case cs_cp1251:
        case cs_8859_5:
        case cs_cp866:
        case cs_koi8r:
          buf[0] = ch;
          buf[1] = '\0';
          break;

        case cs_utf_8:
          utf32_to_utf8(buf, ch);
          break;

        default:
          continue;
      }
      EntityMap[charset][entity] = (const char *)buf;
      XHPEntityMap[charset][entity] = (const char *)buf;
    }

    EntityMap[charset]["quot"] = "\"";
    EntityMap[charset]["lt"] = "<";
    EntityMap[charset]["gt"] = ">";
    EntityMap[charset]["amp"] = "&";

    XHPEntityMap[charset]["quot"] = "\"";
    XHPEntityMap[charset]["lt"] = "<";
    XHPEntityMap[charset]["gt"] = ">";
    XHPEntityMap[charset]["amp"] = "&";
    // XHP-specific entities
    XHPEntityMap[charset]["apos"] = "\'";
    XHPEntityMap[charset]["cloud"] = (const char *)u8"\u2601";
    XHPEntityMap[charset]["umbrella"] = (const char *)u8"\u2602";
    XHPEntityMap[charset]["snowman"] = (const char *)u8"\u2603";
    XHPEntityMap[charset]["snowflake"] = (const char *)u8"\u2745";
    XHPEntityMap[charset]["comet"] = (const char *)u8"\u2604";
    XHPEntityMap[charset]["thunderstorm"] = (const char *)u8"\u2608";
  }

  // the first element is an empty table
  EntityMap[cs_terminator]["quot"] = "\"";
  EntityMap[cs_terminator]["lt"] = "<";
  EntityMap[cs_terminator]["gt"] = ">";
  EntityMap[cs_terminator]["amp"] = "&";
  // XHP-specific entities
  XHPEntityMap[cs_terminator]["apos"] = "\'";
  XHPEntityMap[cs_terminator]["cloud"] = (const char *)u8"\u2601";
  XHPEntityMap[cs_terminator]["umbrella"] = (const char *)u8"\u2602";
  XHPEntityMap[cs_terminator]["snowman"] = (const char *)u8"\u2603";
  XHPEntityMap[cs_terminator]["snowflake"] = (const char *)u8"\u2745";
  XHPEntityMap[cs_terminator]["comet"] = (const char *)u8"\u2604";
  XHPEntityMap[cs_terminator]["thunderstorm"] = (const char *)u8"\u2608";
}

///////////////////////////////////////////////////////////////////////////////
inline static bool decode_entity(char *entity, int *len,
                                 bool decode_double_quote,
                                 bool decode_single_quote,
                                 entity_charset charset, bool all,
                                 bool xhp = false) {
  // entity is 16 bytes, allocated statically below
  // default in PHP
  assert(entity && *entity);
  if (entity[0] == '#') {
    int code;
    if (entity[1] == 'x' || entity[1] == 'X') {
      if (!isxdigit(entity[2])) return false;
      code = strtol(entity + 2, nullptr, 16);
    } else {
      if (!isdigit(entity[1])) return false;
      code = strtol(entity + 1, nullptr, 10);
    }

    // since we don't support multibyte chars other than utf-8
    int l = 1;

    if (code == 39 && decode_single_quote) {
      entity[0] = code;
      entity[1] = '\0';
      *len = l;
      return true;
    }

    if (!all          && (code != '&') &&
        (code != '<') && (code != '>') &&
        (code != '"') && (code != '\'')) {
      // htmlspecialchars_decode() does not parse numeric
      // entities other than & < > " '
      return false;
    }

    switch (charset) {
      case cs_utf_8:
      {
        unsigned char buf[10];
        int size = utf32_to_utf8(buf, code);
        memcpy(entity, buf, size + 1);
        l = size;
        break;
      }

      case cs_8859_1:
      case cs_8859_5:
      case cs_8859_15:
        if ((code >= 0x80 && code < 0xa0) || code > 0xff) {
          return false;
        } else {
          if (code == 39) {
            return false;
          }
          entity[0] = code;
          entity[1] = '\0';
        }
        break;

      case cs_cp1252:
      case cs_cp1251:
      case cs_cp866:
        if (code > 0xff) {
          return false;
        }
        entity[0] = code;
        entity[1] = '\0';
        break;

      case cs_big5:
      case cs_big5hkscs:
      case cs_sjis:
      case cs_eucjp:
        if (code >= 0x80) {
          return false;
        }
        entity[0] = code;
        entity[1] = '\0';
        break;

      case cs_gb2312:
        if (code >= 0x81) {
          return false;
        }
        entity[0] = code;
        entity[1] = '\0';
        break;

      default:
        return false;
    }
    *len = l;
    return true;
  } else {
    HtmlEntityMap *entityMap;

    if (strncasecmp(entity, "quot", 4) == 0 && !decode_double_quote) {
      return false;
    }

    if (all) {
      entityMap = xhp ? &XHPEntityMap[charset] : &EntityMap[charset];
    } else {
      entityMap = xhp ? &XHPEntityMap[cs_terminator]
                      : &EntityMap[cs_terminator];
    }
    HtmlEntityMap::const_iterator iter = entityMap->find(entity);
    if (iter != entityMap->end()) {
      memcpy(entity, iter->second.c_str(), iter->second.length() + 1);
      *len = iter->second.length();
      return true;
    }
  }

  return false;
}

inline static bool encode_entity(char* buf, int* buflen,
                                 const char* entity, bool utf8) {
  entity_charset charset = cs_utf_8;
  if (!utf8){ charset = cs_8859_1; }

  HtmlEntityMap *entityMap = &EntityMap[charset];

  for(HtmlEntityMap::const_iterator iter = entityMap->begin();
      iter != entityMap->end(); iter++) {
    if (strcmp(iter->second.c_str(), entity) == 0) {
      memcpy(buf, iter->first, strlen(iter->first));
      *buflen = strlen(iter->first);
      return true;
    }
  }
  return false;
}

char *string_html_encode(const char *input, int &len,
                         const int64_t qsBitmask, bool utf8,
                         bool dEncode, bool htmlEnt) {
  assert(input);
  /**
   * Though seems to be wasting memory a lot, we have to realize most of the
   * time this function is called with small strings, or fragments of HTMLs.
   * Allocating/deallocating anything less than 1K is trivial these days, and
   * we want avoid string copying as much as possible. Of course, the return
   * char * is really sent back at large, occupying unnecessary space for
   * potentially longer time than we need, we have to realize the two closest
   * solutions are not that much better, either:
   *
   * 1. pre-calculate size by iterating through the string once: too time
   *    consuming;
   * 2. take a guess and double buffer size when over: still wasting, and
   *    it may not save that much.
   *
   * Note: Amount of  allocation per character to be encoded may have to be
   * increased as larger HTML Entities are implemented.
   */
  char *ret = (char *)malloc(len * 14uL + 1);
  if (!ret) {
    return nullptr;
  }
  char *q = ret;
  for (const char *p = input, *end = input + len; p < end; p++) {
    unsigned char c = *p;
    char entity[5];
    int codeLength = 0;
    switch (c) {
    case '"':
      if (qsBitmask & static_cast<int64_t>(EntBitmask::ENT_BM_DOUBLE)) {
        *q++ = '&'; *q++ = 'q'; *q++ = 'u'; *q++ = 'o'; *q++ = 't'; *q++ = ';';
      } else {
        *q++ = c;
      }
      break;
    case '\'':
      if (qsBitmask & static_cast<int64_t>(EntBitmask::ENT_BM_SINGLE)) {
        *q++ = '&';
        if ((qsBitmask & static_cast<int64_t>(EntBitmask::ENT_BM_XML1))) {
          *q++ = 'a'; *q++ = 'p'; *q++ = 'o'; *q++ = 's';
        } else {
          *q++ = '#'; *q++ = '0'; *q++ = '3'; *q++ = '9';
        }
        *q++ = ';';
      } else {
        *q++ = c;
      }
      break;
    case '<':
      *q++ = '&'; *q++ = 'l'; *q++ = 't'; *q++ = ';';
      break;
    case '>':
      *q++ = '&'; *q++ = 'g'; *q++ = 't'; *q++ = ';';
      break;
    case '&':
      if (!dEncode) {
        p++;

        html_get_entity_map();

        bool found = false;
        for (const char *t = p; *t; t++) {
          if (*t == ';') {
            int l = t - p;
            if (l > 0) {
              char sbuf[16] = {0};
              char *buf;
              if (l > 10) {
                buf = (char* )malloc(l + 1);
              } else {
                buf = sbuf;
              }
              memcpy(buf, p, l);
              buf[l] = '\0';
              if (decode_entity(buf, &l, true, true,
                cs_utf_8, true)) {
                found = true;
                *q++ = '&';
                for(const char *s = p; s <= t; s++) {
                  *q++ = *s;
                }
                p = t;
              }
              if (buf != sbuf) {
                free(buf);
              }
            }
            break;
          }
        }
        if (!found) {
          p--;
          *q++ = '&'; *q++ = 'a'; *q++ = 'm'; *q++ = 'p'; *q++ = ';';
        }
      } else {
        *q++ = '&'; *q++ = 'a'; *q++ = 'm'; *q++ = 'p'; *q++ = ';';
      }
      break;
    case static_cast<unsigned char>('\xc2'):
      if (htmlEnt && utf8 && p != end && *(p+1) == '\xa0') {
        *q++ = '&'; *q++ = 'n'; *q++ = 'b'; *q++ = 's'; *q++ = 'p'; *q++ = ';';
        p++;
        break;
      }
      [[fallthrough]];
    default: {
      if (LIKELY(c < 0x80)) {
        *q++ = c;
        break;
      } else if (htmlEnt && !utf8 && (c - 160) < sizeof(ent_iso_8859_1) - 1) {
        /**
          * https://github.com/facebook/hhvm/issues/2186
          * If not UTF8, and we are converting to HTML entities, use known
          * entity equivalent of the character, if possible.
          * Since we only support ISO-8859-1 or UTF8 right now, and they use
          * the same mapping array, use it.
          * Start at 0xA0 = 160
          */
        *q++ = '&';
        const char *s = ent_iso_8859_1[c - 160];
        int len_2 = strlen(s);
        for (int n = 0; n < len_2; n++) {
          *q++ = *s++;
        }
        *q++ = ';';
        break;
      }

      bool should_skip =
        qsBitmask & static_cast<int64_t>(EntBitmask::ENT_BM_IGNORE);
      bool should_replace =
        qsBitmask & static_cast<int64_t>(EntBitmask::ENT_BM_SUBSTITUTE);

      if (!utf8 && should_skip) {
        *q++ = c;
        break;
      }

      auto avail = end - p;
      auto utf8_trail = [](unsigned char c) { return c >= 0x80 && c <= 0xbf; };
      auto utf8_lead = [](unsigned char c) {
        return c < 0x80 || (c >= 0xC2 && c <= 0xF4);
      };

      // This has to be a macro since it needs to be able to break away from
      // the for loop we're in.
      // ENT_IGNORE has higher precedence than ENT_SUBSTITUTE
      // \uFFFD is Unicode Replacement Character (U+FFFD)
      #define UTF8_ERROR_IF_LEN(cond, len) \
        if (cond) { \
          p += (len) - 1; \
          if (should_skip) { break; } \
          else if (should_replace) { strcpy(q, (const char *)u8"\uFFFD"); q += 3; break; } \
          else { goto exit_error; } \
        }

      #define UTF8_ERROR_IF(cond) UTF8_ERROR_IF_LEN(cond, 1)

      if (utf8) {
        if (c < 0xc2) {
          UTF8_ERROR_IF(true);
        } else if (c < 0xe0) {
          UTF8_ERROR_IF(avail < 2);
          UTF8_ERROR_IF_LEN(!utf8_trail(*(p + 1)), utf8_lead(*(p + 1)) ? 1 : 2);

          uint16_t tc = ((c & 0x1f) << 6) | (p[1] & 0x3f);
          UTF8_ERROR_IF_LEN(tc < 0x80, 2); // non-shortest form

          codeLength = 2;
          entity[0] = *p;
          entity[1] = *(p + 1);
          entity[2] = '\0';
        } else if (c < 0xf0) {
          if (avail < 3 || !utf8_trail(*(p + 1)) || !utf8_trail(*(p + 2))) {
            UTF8_ERROR_IF_LEN(avail < 2 || utf8_lead(*(p + 1)), 1);
            UTF8_ERROR_IF_LEN(avail < 3 || utf8_lead(*(p + 2)), 2);
            UTF8_ERROR_IF_LEN(true, 3);
          }

          uint32_t tc = ((c & 0x0f) << 12) |
                        ((*(p+1) & 0x3f) << 6) |
                        (*(p+2) & 0x3f);
          UTF8_ERROR_IF_LEN(tc < 0x800, 3); // non-shortest form
          UTF8_ERROR_IF_LEN(tc >= 0xd800 && tc <= 0xdfff, 3); // surrogate

          codeLength = 3;
          entity[0] = *p;
          entity[1] = *(p + 1);
          entity[2] = *(p + 2);
          entity[3] = '\0';
        } else if (c < 0xf5) {
          if (avail < 4 || !utf8_trail(*(p + 1)) || !utf8_trail(*(p + 2)) ||
              !utf8_trail(*(p + 3))) {
            UTF8_ERROR_IF_LEN(avail < 2 || utf8_lead(*(p + 1)), 1);
            UTF8_ERROR_IF_LEN(avail < 3 || utf8_lead(*(p + 2)), 2);
            UTF8_ERROR_IF_LEN(avail < 4 || utf8_lead(*(p + 3)), 3);
            UTF8_ERROR_IF_LEN(true, 4);
          }

          uint32_t tc = ((c & 0x07) << 18) |
                        ((*(p+1) & 0x3f) << 12) |
                        ((*(p+2) & 0x3f) << 6) |
                        (*(p+3) & 0x3f);

          // non-shortest form or outside range
          UTF8_ERROR_IF_LEN(tc < 0x10000 || tc > 0x10ffff, 4);

          codeLength = 4;
          entity[0] = *p;
          entity[1] = *(p + 1);
          entity[2] = *(p + 2);
          entity[3] = *(p + 3);
          entity[4] = '\0';
        } else {
          UTF8_ERROR_IF(true);
        }
      } else {
        codeLength = 1;
        entity[0] = *p;
        entity[1] = '\0';
      }

      if (htmlEnt) {
        html_get_entity_map();

        char buf[16] = {0};
        buf[0] = c;
        int len_2 = 1;

        if (encode_entity(buf, &len_2, const_cast<char*>(entity), utf8)) {
          *q++ = '&';
          const char *s = buf;
          for (int n = 0; n < len_2; n++) {
            *q++ = *s++;
          }
          *q++ = ';';
        } else {
          memcpy(q, p, codeLength);
          q += codeLength;
        }
      } else {
        memcpy(q, p, codeLength);
        q += codeLength;
      }
      p += codeLength - 1;

      break;
    }
    }

  }

  #undef UTF8_ERROR_IF
  #undef UTF8_ERROR_IF_LEN

  if (q - ret > INT_MAX) {
    goto exit_error;
  }
  *q = 0;
  len = q - ret;
  return ret;

exit_error:
  free(ret);
  return nullptr;
}

char *string_html_encode_extra(const char *input, int &len,
                               StringHtmlEncoding flags,
                               const AsciiMap *asciiMap) {
  assert(input);
  /**
   * Though seems to be wasting memory a lot, we have to realize most of the
   * time this function is called with small strings, or fragments of HTMLs.
   * Allocating/deallocating anything less than 1K is trivial these days, and
   * we want avoid string copying as much as possible. Of course, the return
   * char * is really sent back at large, occupying unnecessary space for
   * potentially longer time than we need, we have to realize the two closest
   * solutions are not that much better, either:
   *
   * 1. pre-calculate size by iterating through the string once: too time
   *    consuming;
   * 2. take a guess and double buffer size when over: still wasting, and
   *    it may not save that much.
   */
  char *ret = (char *)malloc(len * 8uL + 1);
  if (!ret) {
    return nullptr;
  }
  char *q = ret;
  const char *rep = (const char *)u8"\ufffd";
  int32_t srcPosBytes;
  for (srcPosBytes = 0; srcPosBytes < len; /* incremented in-loop */) {
    unsigned char c = input[srcPosBytes];
    if (c && c < 128) {
      srcPosBytes++; // Optimize US-ASCII case
      if ((asciiMap->map[c & 64 ? 1 : 0] >> (c & 63)) & 1) {
        switch (c) {
          case '"':
            *q++ = '&'; *q++ = 'q'; *q++ = 'u';
            *q++ = 'o'; *q++ = 't'; *q++ = ';';
            break;
          case '\'':
            *q++ = '&'; *q++ = '#'; *q++ = '0';
            *q++ = '3'; *q++ = '9'; *q++ = ';';
            break;
          case '<':
            *q++ = '&'; *q++ = 'l'; *q++ = 't'; *q++ = ';';
            break;
          case '>':
            *q++ = '&'; *q++ = 'g'; *q++ = 't'; *q++ = ';';
            break;
          case '&':
            *q++ = '&'; *q++ = 'a'; *q++ = 'm'; *q++ = 'p'; *q++ = ';';
            break;
          default:
            *q++ = '&'; *q++ = '#';
            *q++ = c >= 100 ? '1' : '0';
            *q++ = ((c / 10) % 10) + '0';
            *q++ = (c % 10) + '0';
            *q++ = ';';
            break;
        }
      } else {
        *q++ = c;
      }
    } else if (flags & STRING_HTML_ENCODE_UTF8) {
      UChar32 curCodePoint;
      U8_NEXT(input, srcPosBytes, len, curCodePoint);
      if ((flags & STRING_HTML_ENCODE_NBSP) && curCodePoint == 0xC2A0) {
        *q++ = '&'; *q++ = 'n'; *q++ = 'b'; *q++ = 's'; *q++ = 'p'; *q++ = ';';
      } else if (curCodePoint <= 0) {
        if (flags & STRING_HTML_ENCODE_UTF8IZE_REPLACE) {
          if (flags & STRING_HTML_ENCODE_HIGH) {
            *q++ = '&'; *q++ = '#'; *q++ = 'x';
            *q++ = 'f'; *q++ = 'f'; *q++ = 'f'; *q++ = 'd';
            *q++ = ';';
          } else {
            const char *r = rep;
            while (*r) *q++ = *r++;
          }
        }
      } else if (flags & STRING_HTML_ENCODE_HIGH) {
        q += sprintf(q, "&#x%x;", curCodePoint);
      } else {
        int32_t pos = 0;
        U8_APPEND_UNSAFE(q, pos, curCodePoint);
        q += pos;
      }
    } else {
      srcPosBytes++; // Optimize US-ASCII case
      if (c == 0xa0) {
        *q++ = '&'; *q++ = 'n'; *q++ = 'b'; *q++ = 's'; *q++ = 'p'; *q++ = ';';
      } else if (flags & STRING_HTML_ENCODE_HIGH) {
        *q++ = '&'; *q++ = '#';
        *q++ = c >= 200 ? '2' : '1';
        *q++ = ((c / 10) % 10) + '0';
        *q++ = (c % 10) + '0';
        *q++ = ';';
      } else {
        *q++ = c;
      }
    }
  }
  if (q - ret > INT_MAX) {
    free(ret);
    return nullptr;
  }
  *q = 0;
  len = q - ret;
  return ret;
}

char *string_html_decode(const char *input, int &len,
                         bool decode_double_quote, bool decode_single_quote,
                         const char *charset_hint, bool all,
                         bool xhp /* = false */) {
  assert(input);

  if (!EntityMapInited) {
    Lock lock(EntityMapMutex);
    if (!EntityMapInited) {
      init_entity_table();
      EntityMapInited = true;
    }
  }

  entity_charset charset = determine_charset(charset_hint);
  if (charset == cs_unknown) {
    return nullptr;
  }

  char *ret = (char *)malloc(len + 1);
  char *q = ret;
  for (const char *p = input; *p || UNLIKELY(p - input < len); p++) {
    char ch = *p;
    if (ch != '&') {
      *q++ = ch;
      continue;
    }
    p++;

    bool found = false;
    for (const char *t = p; *t; t++) {
      if (*t == ';') {
        int l = t - p;
        if (l > 0) {
          char sbuf[16] = {0};
          char *buf;
          if (l > 10) {
            buf = (char* )malloc(l + 1);
          } else {
            buf = sbuf;
          }
          memcpy(buf, p, l);
          buf[l] = '\0';
          if (decode_entity(buf, &l, decode_double_quote, decode_single_quote,
                            charset, all, xhp)) {
            memcpy(q, buf, l);
            found = true;
            p = t;
            q += l;
          }
          if (buf != sbuf) {
            free(buf);
          }
        }
        break;
      }
    }
    if (!found) {
      p--;
      *q++ = '&'; // not an entity
    }
  }
  *q = '\0';
  len = q - ret;
  return ret;
}

const html_entity_map* html_get_entity_map() {
  if (!EntityMapInited) {
    Lock lock(EntityMapMutex);
    if (!EntityMapInited) {
      init_entity_table();
      EntityMapInited = true;
    }
  }
  return entity_map;
}

///////////////////////////////////////////////////////////////////////////////
}
