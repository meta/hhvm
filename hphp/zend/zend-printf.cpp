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

#include "hphp/zend/zend-printf.h"

#include "hphp/util/exception.h"
#include "hphp/zend/zend-string.h"
#include "hphp/zend/zend-strtod.h"

#include <cmath>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace HPHP {

/* These definitions are copied from the Zend formatted output conversion
   files so that we only need to make minimal changes to the Zend formatted
   output conversion functions that are incorporated here.
 */
#define NDIG 320

enum length_modifier_e {
  LM_STD = 0,
  LM_INTMAX_T,
  LM_PTRDIFF_T,
  LM_LONG_LONG,
  LM_SIZE_T,
  LM_LONG,
  LM_LONG_DOUBLE
};

enum boolean_e {
  NO = 0, YES = 1
};

#define NUM(c) (c - '0')

#define STR_TO_DEC(str, num) do {  \
  num = NUM(*str++);               \
  while (isdigit((int)*str)) {     \
    num *= 10;                     \
    num += NUM(*str++);            \
    if (num >= INT_MAX / 10) {     \
      while (isdigit((int)*str++)) \
        continue;                  \
      break;                       \
    }                              \
  }                                \
} while (0)

/*
 * This macro does zero padding so that the precision
 * requirement is satisfied. The padding is done by
 * adding '0's to the left of the string that is going
 * to be printed.
 */
#define FIX_PRECISION(adjust, precision, s, s_len) do { \
  if (adjust)                                           \
    while (s_len < precision) {                         \
      *--s = '0';                                       \
      s_len++;                                          \
    }                                                   \
} while (0)

using wide_int = int64_t;
using u_wide_int = uint64_t;

#define FALSE           0
#define TRUE            1
#define NUL             '\0'
#define INT_NULL        ((int *)0)

static const char* s_null = "(null)";
#define S_NULL_LEN      6

#define FLOAT_DIGITS    6
#define EXPONENT_LENGTH 10

#define HAVE_LOCALE_H 1

#ifdef HAVE_LOCALE_H
} // namespace HPHP

#include <locale.h>

namespace HPHP {
#define LCONV_DECIMAL_POINT (*lconv->decimal_point)
#else
#define LCONV_DECIMAL_POINT '.'
#endif

///////////////////////////////////////////////////////////////////////////////
/*
 * Copyright (c) 2002, 2006 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

static char * __cvt(double value, int ndigit, int *decpt, int *sign,
                    int fmode, int pad) {
  char *s = nullptr;
  char *p, *rve, c;
  size_t siz;

  if (ndigit < 0) {
    siz = -ndigit + 1;
  } else {
    siz = ndigit + 1;
  }

  /* __dtoa() doesn't allocate space for 0 so we do it by hand */
  if (value == 0.0) {
    *decpt = 1 - fmode; /* 1 for 'e', 0 for 'f' */
    *sign = 0;
    if ((rve = s = (char *)malloc(ndigit?siz:2)) == nullptr) {
      return(nullptr);
    }
    *rve++ = '0';
    *rve = '\0';
    if (!ndigit) {
      return(s);
    }
  } else {
    p = zend_dtoa(value, fmode + 2, ndigit, decpt, sign, &rve);
    if (*decpt == 9999) {
      /* Infinity or Nan, convert to inf or nan like printf */
      *decpt = 0;
      c = *p;
      zend_freedtoa(p);
      return strdup(c == 'I' ? "INF" : "NAN");
    }
    /* Make a local copy and adjust rve to be in terms of s */
    if (pad && fmode) {
      siz += *decpt;
    }
    if ((s = (char *)malloc(siz+1)) == nullptr) {
      zend_freedtoa(p);
      return(nullptr);
    }
    (void)string_copy(s, p, siz);
    rve = s + (rve - p);
    zend_freedtoa(p);
  }

  /* Add trailing zeros */
  if (pad) {
    siz -= rve - s;
    while (--siz) {
      *rve++ = '0';
    }
    *rve = '\0';
  }

  return(s);
}

static inline char *php_ecvt(double value, int ndigit, int *decpt, int *sign) {
  return(__cvt(value, ndigit, decpt, sign, 0, 1));
}

static inline char *php_fcvt(double value, int ndigit, int *decpt, int *sign) {
  return(__cvt(value, ndigit, decpt, sign, 1, 1));
}

char *php_gcvt(double value, int ndigit, char dec_point,
                      char exponent, char *buf) {
  char *digits, *dst, *src;
  int i, decpt, sign;

  digits = zend_dtoa(value, 2, ndigit, &decpt, &sign, nullptr);
  if (decpt == 9999) {
    /*
     * Infinity or NaN, convert to inf or nan with sign.
     * We assume the buffer is at least ndigit long.
     */
    snprintf(buf, ndigit + 1, "%s%s", (sign && *digits == 'I') ? "-" : "",
             *digits == 'I' ? "INF" : "NAN");
    zend_freedtoa(digits);
    return (buf);
  }

  dst = buf;
  if (sign) {
    *dst++ = '-';
  }

  if ((decpt >= 0 && decpt > ndigit) || decpt < -3) { /* use E-style */
    /* exponential format (e.g. 1.2345e+13) */
    if (--decpt < 0) {
      sign = 1;
      decpt = -decpt;
    } else {
      sign = 0;
    }
    src = digits;
    *dst++ = *src++;
    *dst++ = dec_point;
    if (*src == '\0') {
      *dst++ = '0';
    } else {
      do {
        *dst++ = *src++;
      } while (*src != '\0');
    }
    *dst++ = exponent;
    if (sign) {
      *dst++ = '-';
    } else {
      *dst++ = '+';
    }
    if (decpt < 10) {
      *dst++ = '0' + decpt;
      *dst = '\0';
    } else {
      /* XXX - optimize */
      for (sign = decpt, i = 0; (sign /= 10) != 0; i++)
        continue;
      dst[i + 1] = '\0';
      while (decpt != 0) {
        dst[i--] = '0' + decpt % 10;
        decpt /= 10;
      }
    }
  } else if (decpt < 0) {
    /* standard format 0. */
    *dst++ = '0';   /* zero before decimal point */
    *dst++ = dec_point;
    do {
      *dst++ = '0';
    } while (++decpt < 0);
    src = digits;
    while (*src != '\0') {
      *dst++ = *src++;
    }
    *dst = '\0';
  } else {
    /* standard format */
    for (i = 0, src = digits; i < decpt; i++) {
      if (*src != '\0') {
        *dst++ = *src++;
      } else {
        *dst++ = '0';
      }
    }
    if (*src != '\0') {
      if (src == digits) {
        *dst++ = '0';   /* zero before decimal point */
      }
      *dst++ = dec_point;
      for (i = decpt; digits[i] != '\0'; i++) {
        *dst++ = digits[i];
      }
    }
    *dst = '\0';
  }
  zend_freedtoa(digits);
  return (buf);
}

///////////////////////////////////////////////////////////////////////////////
// Apache license

/* ====================================================================
 * Copyright (c) 1995-1998 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission.
 *
 * 5. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 * This code is based on, and used with the permission of, the
 * SIO stdio-replacement strx_* functions by Panos Tsirigotis
 * <panos@alumni.cs.colorado.edu> for xinetd.
 */

/*
 * Convert num to a base X number where X is a power of 2. nbits determines X.
 * For example, if nbits is 3, we do base 8 conversion
 * Return value:
 *      a pointer to a string containing the number
 *
 * The caller provides a buffer for the string: that is the buf_end argument
 * which is a pointer to the END of the buffer + 1 (i.e. if the buffer
 * is declared as buf[ 100 ], buf_end should be &buf[ 100 ])
 */
char * ap_php_conv_p2(uint64_t num, int nbits, char format, char *buf_end,
                      int *len)
{
  int mask = (1 << nbits) - 1;
  char *p = buf_end;
  static char low_digits[] = "0123456789abcdef";
  static char upper_digits[] = "0123456789ABCDEF";
  char *digits = (format == 'X') ? upper_digits : low_digits;

  do {
    *--p = digits[num & mask];
    num >>= nbits;
  }
  while (num);

  *len = buf_end - p;
  return (p);
}

/*
 * Convert num to its decimal format.
 * Return value:
 *   - a pointer to a string containing the number (no sign)
 *   - len contains the length of the string
 *   - is_negative is set to TRUE or FALSE depending on the sign
 *     of the number (always set to FALSE if is_unsigned is TRUE)
 *
 * The caller provides a buffer for the string: that is the buf_end argument
 * which is a pointer to the END of the buffer + 1 (i.e. if the buffer
 * is declared as buf[ 100 ], buf_end should be &buf[ 100 ])
 */
char * ap_php_conv_10(int64_t num, bool is_unsigned, int *is_negative,
                      char *buf_end, int *len) {
  char *p = buf_end;
  uint64_t magnitude;

  if (is_unsigned) {
    magnitude = (uint64_t) num;
    *is_negative = 0;
  } else {
    *is_negative = (num < 0);

    /*
     * On a 2's complement machine, negating the most negative integer
     * results in a number that cannot be represented as a signed integer.
     * Here is what we do to obtain the number's magnitude:
     *      a. add 1 to the number
     *      b. negate it (becomes positive)
     *      c. convert it to unsigned
     *      d. add 1
     */
    if (*is_negative) {
      int64_t t = num + 1;
      magnitude = ((uint64_t) - t) + 1;
    } else {
      magnitude = (uint64_t) num;
    }
  }

  /*
   * We use a do-while loop so that we write at least 1 digit
   */
  do {
    uint64_t new_magnitude = magnitude / 10;

    *--p = (char)(magnitude - new_magnitude * 10 + '0');
    magnitude = new_magnitude;
  }
  while (magnitude);

  *len = buf_end - p;
  return (p);
}

///////////////////////////////////////////////////////////////////////////////
/*
 * Convert a floating point number to a string formats 'f', 'e' or 'E'.
 * The result is placed in buf, and len denotes the length of the string
 * The sign is returned in the is_negative argument (and is not placed
 * in buf).
 */
char * php_conv_fp(char format, double num, bool add_dp, int precision,
                   char dec_point, int *is_negative, char *buf, int *len) {
  char *s = buf;
  char *p, *p_orig;
  int decimal_point;

  if (precision >= NDIG - 1) {
    precision = NDIG - 2;
  }

  if (format == 'F') {
    p_orig = p = php_fcvt(num, precision, &decimal_point, is_negative);
  } else { // either e or E format
    p_orig = p = php_ecvt(num, precision + 1, &decimal_point, is_negative);
  }

  // Check for Infinity and NaN
  if (isalpha((int)*p)) {
    *len = strlen(p);
    memcpy(buf, p, *len + 1);
    *is_negative = 0;
    free(p_orig);
    return (buf);
  }
  if (format == 'F') {
    if (decimal_point <= 0) {
      if (num != 0 || precision > 0) {
        *s++ = '0';
        if (precision > 0) {
          *s++ = dec_point;
          while (decimal_point++ < 0) {
            *s++ = '0';
          }
        } else if (add_dp) {
          *s++ = dec_point;
        }
      }
    } else {
      int addz = decimal_point >= NDIG ? decimal_point - NDIG + 1 : 0;
      decimal_point -= addz;
      while (decimal_point-- > 0) {
        *s++ = *p++;
      }
      while (addz-- > 0) {
        *s++ = '0';
      }
      if (precision > 0 || add_dp) {
        *s++ = dec_point;
      }
    }
  } else {
    *s++ = *p++;
    if (precision > 0 || add_dp) {
      *s++ = '.';
    }
  }

  // copy the rest of p, the NUL is NOT copied
  while (*p) {
    *s++ = *p++;
  }

  if (format != 'F') {
    char temp[EXPONENT_LENGTH]; // for exponent conversion
    int t_len;
    int exponent_is_negative;

    *s++ = format; // either e or E
    decimal_point--;
    if (decimal_point != 0) {
      p = ap_php_conv_10((int64_t) decimal_point, false,
                         &exponent_is_negative, &temp[EXPONENT_LENGTH],
                         &t_len);
      *s++ = exponent_is_negative ? '-' : '+';

      // Make sure the exponent has at least 2 digits
      while (t_len--) {
        *s++ = *p++;
      }
    } else {
      *s++ = '+';
      *s++ = '0';
    }
  }
  *len = s - buf;
  free(p_orig);
  return (buf);
}

///////////////////////////////////////////////////////////////////////////////

inline static void appendchar(char **buffer, int *pos, int *size, char add) {
  if ((*pos + 1) >= *size) {
    *size <<= 1;
    *buffer = (char*)realloc(*buffer, *size);
  }
  (*buffer)[(*pos)++] = add;
}

inline static void appendsimplestring(char **buffer, int *pos, int *size,
                                      const char *add, int len) {
  int req_size = *pos + len;

  if (req_size > *size) {
    while (req_size > *size) {
      *size <<= 1;
    }
    *buffer = (char *)realloc(*buffer, *size);
  }
  memcpy(&(*buffer)[*pos], add, len);
  *pos += len;
}

/*
 * Do format conversion placing the output in buffer
 */
static int xbuf_format_converter(char **outbuf, const char *fmt, va_list ap)
{
  char *s = nullptr;
  char *q;
  int s_len;

  int min_width = 0;
  int precision = 0;
  enum {
    LEFT, RIGHT
  } adjust;
  char pad_char;
  char prefix_char;

  double fp_num;
  wide_int i_num = (wide_int) 0;
  u_wide_int ui_num;

  char num_buf[NUM_BUF_SIZE];
  char char_buf[2];      /* for printing %% and %<unknown> */

#ifdef HAVE_LOCALE_H
  struct lconv *lconv = nullptr;
#endif

  /*
   * Flag variables
   */
  length_modifier_e modifier;
  boolean_e alternate_form;
  boolean_e print_sign;
  boolean_e print_blank;
  boolean_e adjust_precision;
  boolean_e adjust_width;
  int is_negative;

  int size = 240;
  char *result = (char *)malloc(size);
  int outpos = 0;

  while (*fmt) {
    if (*fmt != '%') {
      appendchar(&result, &outpos, &size, *fmt);
    } else {
      /*
       * Default variable settings
       */
      adjust = RIGHT;
      alternate_form = print_sign = print_blank = NO;
      pad_char = ' ';
      prefix_char = NUL;

      fmt++;

      /*
       * Try to avoid checking for flags, width or precision
       */
      if (isascii((int)*fmt) && !islower((int)*fmt)) {
        /*
         * Recognize flags: -, #, BLANK, +
         */
        for (;; fmt++) {
          if (*fmt == '-')
            adjust = LEFT;
          else if (*fmt == '+')
            print_sign = YES;
          else if (*fmt == '#')
            alternate_form = YES;
          else if (*fmt == ' ')
            print_blank = YES;
          else if (*fmt == '0')
            pad_char = '0';
          else
            break;
        }

        /*
         * Check if a width was specified
         */
        if (isdigit((int)*fmt)) {
          STR_TO_DEC(fmt, min_width);
          adjust_width = YES;
        } else if (*fmt == '*') {
          min_width = va_arg(ap, int);
          fmt++;
          adjust_width = YES;
          if (min_width < 0) {
            adjust = LEFT;
            min_width = -min_width;
          }
        } else
          adjust_width = NO;

        /*
         * Check if a precision was specified
         *
         * XXX: an unreasonable amount of precision may be specified
         * resulting in overflow of num_buf. Currently we
         * ignore this possibility.
         */
        if (*fmt == '.') {
          adjust_precision = YES;
          fmt++;
          if (isdigit((int)*fmt)) {
            STR_TO_DEC(fmt, precision);
          } else if (*fmt == '*') {
            precision = va_arg(ap, int);
            fmt++;
            if (precision < 0)
              precision = 0;
          } else
            precision = 0;
        } else
          adjust_precision = NO;
      } else
        adjust_precision = adjust_width = NO;

      /*
       * Modifier check
       */
      switch (*fmt) {
        case 'L':
          fmt++;
          modifier = LM_LONG_DOUBLE;
          break;
        case 'I':
          fmt++;
#if SIZEOF_LONG_LONG
          if (*fmt == '6' && *(fmt+1) == '4') {
            fmt += 2;
            modifier = LM_LONG_LONG;
          } else
#endif
            if (*fmt == '3' && *(fmt+1) == '2') {
              fmt += 2;
              modifier = LM_LONG;
            } else {
#ifdef _WIN64
              modifier = LM_LONG_LONG;
#else
              modifier = LM_LONG;
#endif
            }
          break;
        case 'l':
          fmt++;
#if SIZEOF_LONG_LONG
          if (*fmt == 'l') {
            fmt++;
            modifier = LM_LONG_LONG;
          } else
#endif
            modifier = LM_LONG;
          break;
        case 'z':
          fmt++;
          modifier = LM_SIZE_T;
          break;
        case 'j':
          fmt++;
#if SIZEOF_INTMAX_T
          modifier = LM_INTMAX_T;
#else
          modifier = LM_SIZE_T;
#endif
          break;
        case 't':
          fmt++;
#if SIZEOF_PTRDIFF_T
          modifier = LM_PTRDIFF_T;
#else
          modifier = LM_SIZE_T;
#endif
          break;
        case 'h':
          fmt++;
          if (*fmt == 'h') {
            fmt++;
          }
          [[fallthrough]]; // these are promoted to int, so no break
        default:
          modifier = LM_STD;
          break;
      }

      /*
       * Argument extraction and printing.
       * First we determine the argument type.
       * Then, we convert the argument to a string.
       * On exit from the switch, s points to the string that
       * must be printed, s_len has the length of the string
       * The precision requirements, if any, are reflected in s_len.
       *
       * NOTE: pad_char may be set to '0' because of the 0 flag.
       *   It is reset to ' ' by non-numeric formats
       */
      switch (*fmt) {
        case 'u':
          switch(modifier) {
            default:
              i_num = (wide_int) va_arg(ap, unsigned int);
              break;
            case LM_LONG_DOUBLE:
              goto fmt_error;
            case LM_LONG:
              i_num = (wide_int) va_arg(ap, unsigned long int);
              break;
            case LM_SIZE_T:
              i_num = (wide_int) va_arg(ap, size_t);
              break;
#if SIZEOF_LONG_LONG
            case LM_LONG_LONG:
              i_num = (wide_int) va_arg(ap, u_wide_int);
              break;
#endif
#if SIZEOF_INTMAX_T
            case LM_INTMAX_T:
              i_num = (wide_int) va_arg(ap, uintmax_t);
              break;
#endif
#if SIZEOF_PTRDIFF_T
            case LM_PTRDIFF_T:
              i_num = (wide_int) va_arg(ap, ptrdiff_t);
              break;
#endif
          }
          /*
           * The rest also applies to other integer formats, so fall
           * into that case.
           */
          [[fallthrough]];
        case 'd':
        case 'i':
          /*
           * Get the arg if we haven't already.
           */
          if ((*fmt) != 'u') {
            switch(modifier) {
              default:
                i_num = (wide_int) va_arg(ap, int);
                break;
              case LM_LONG_DOUBLE:
                goto fmt_error;
              case LM_LONG:
                i_num = (wide_int) va_arg(ap, long int);
                break;
              case LM_SIZE_T:
#if SIZEOF_SSIZE_T
                i_num = (wide_int) va_arg(ap, ssize_t);
#else
                i_num = (wide_int) va_arg(ap, size_t);
#endif
                break;
#if SIZEOF_LONG_LONG
              case LM_LONG_LONG:
                i_num = (wide_int) va_arg(ap, wide_int);
                break;
#endif
#if SIZEOF_INTMAX_T
              case LM_INTMAX_T:
                i_num = (wide_int) va_arg(ap, intmax_t);
                break;
#endif
#if SIZEOF_PTRDIFF_T
              case LM_PTRDIFF_T:
                i_num = (wide_int) va_arg(ap, ptrdiff_t);
                break;
#endif
            }
          }
          s = ap_php_conv_10(i_num, (*fmt) == 'u', &is_negative,
                &num_buf[NUM_BUF_SIZE], &s_len);
          FIX_PRECISION(adjust_precision, precision, s, s_len);

          if (*fmt != 'u') {
            if (is_negative)
              prefix_char = '-';
            else if (print_sign)
              prefix_char = '+';
            else if (print_blank)
              prefix_char = ' ';
          }
          break;


        case 'o':
          switch(modifier) {
            default:
              ui_num = (u_wide_int) va_arg(ap, unsigned int);
              break;
            case LM_LONG_DOUBLE:
              goto fmt_error;
            case LM_LONG:
              ui_num = (u_wide_int) va_arg(ap, unsigned long int);
              break;
            case LM_SIZE_T:
              ui_num = (u_wide_int) va_arg(ap, size_t);
              break;
#if SIZEOF_LONG_LONG
            case LM_LONG_LONG:
              ui_num = (u_wide_int) va_arg(ap, u_wide_int);
              break;
#endif
#if SIZEOF_INTMAX_T
            case LM_INTMAX_T:
              ui_num = (u_wide_int) va_arg(ap, uintmax_t);
              break;
#endif
#if SIZEOF_PTRDIFF_T
            case LM_PTRDIFF_T:
              ui_num = (u_wide_int) va_arg(ap, ptrdiff_t);
              break;
#endif
          }
          s = ap_php_conv_p2(ui_num, 3, *fmt,
                &num_buf[NUM_BUF_SIZE], &s_len);
          FIX_PRECISION(adjust_precision, precision, s, s_len);
          if (alternate_form && *s != '0') {
            *--s = '0';
            s_len++;
          }
          break;


        case 'x':
        case 'X':
          switch(modifier) {
            default:
              ui_num = (u_wide_int) va_arg(ap, unsigned int);
              break;
            case LM_LONG_DOUBLE:
              goto fmt_error;
            case LM_LONG:
              ui_num = (u_wide_int) va_arg(ap, unsigned long int);
              break;
            case LM_SIZE_T:
              ui_num = (u_wide_int) va_arg(ap, size_t);
              break;
#if SIZEOF_LONG_LONG
            case LM_LONG_LONG:
              ui_num = (u_wide_int) va_arg(ap, u_wide_int);
              break;
#endif
#if SIZEOF_INTMAX_T
            case LM_INTMAX_T:
              ui_num = (u_wide_int) va_arg(ap, uintmax_t);
              break;
#endif
#if SIZEOF_PTRDIFF_T
            case LM_PTRDIFF_T:
              ui_num = (u_wide_int) va_arg(ap, ptrdiff_t);
              break;
#endif
          }
          s = ap_php_conv_p2(ui_num, 4, *fmt,
                &num_buf[NUM_BUF_SIZE], &s_len);
          FIX_PRECISION(adjust_precision, precision, s, s_len);
          if (alternate_form && i_num != 0) {
            *--s = *fmt;  /* 'x' or 'X' */
            *--s = '0';
            s_len += 2;
          }
          break;


        case 's':
        case 'v':
          s = va_arg(ap, char *);
          if (s != nullptr) {
            // %.*s must bound the read to `precision`; strlen would over-read a
            // non-NUL-terminated arg.
            s_len = adjust_precision ? strnlen(s, (size_t)precision) : strlen(s);
          } else {
            s = const_cast<char*>(s_null);
            s_len = S_NULL_LEN;
          }
          pad_char = ' ';
          break;


        case 'f':
        case 'F':
        case 'e':
        case 'E':
          switch(modifier) {
            case LM_LONG_DOUBLE:
              fp_num = (double) va_arg(ap, long double);
              break;
            case LM_STD:
              fp_num = va_arg(ap, double);
              break;
            default:
              goto fmt_error;
          }

          if (std::isnan(fp_num)) {
            s = const_cast<char*>("nan");
            s_len = 3;
          } else if (std::isinf(fp_num)) {
            s = const_cast<char*>("inf");
            s_len = 3;
          } else {
#ifdef HAVE_LOCALE_H
            if (!lconv) {
              lconv = localeconv();
            }
#endif
            s = php_conv_fp((*fmt == 'f')?'F':*fmt, fp_num, alternate_form,
             (adjust_precision == NO) ? FLOAT_DIGITS : precision,
             (*fmt == 'f')?LCONV_DECIMAL_POINT:'.',
                  &is_negative, &num_buf[1], &s_len);
            if (is_negative)
              prefix_char = '-';
            else if (print_sign)
              prefix_char = '+';
            else if (print_blank)
              prefix_char = ' ';
          }
          break;


        case 'g':
        case 'k':
        case 'G':
        case 'H': {
          switch(modifier) {
            case LM_LONG_DOUBLE:
              fp_num = (double) va_arg(ap, long double);
              break;
            case LM_STD:
              fp_num = va_arg(ap, double);
              break;
            default:
              goto fmt_error;
          }

          if (std::isnan(fp_num)) {
            s = const_cast<char*>("NAN");
            s_len = 3;
            break;
          } else if (std::isinf(fp_num)) {
            if (fp_num > 0) {
              s = const_cast<char*>("INF");
              s_len = 3;
            } else {
              s = const_cast<char*>("-INF");
              s_len = 4;
            }
            break;
          }

          if (adjust_precision == NO)
            precision = FLOAT_DIGITS;
          else if (precision == 0)
            precision = 1;
          /*
          * * We use &num_buf[ 1 ], so that we have room for the sign
          */
          char expon = '.';
          if (*fmt != 'H' && *fmt != 'k') {
#ifdef HAVE_LOCALE_H
            if (!lconv) lconv = localeconv();
#endif
            expon = LCONV_DECIMAL_POINT;
          }
          s = php_gcvt(fp_num, precision,
                      expon,
                      (*fmt == 'G' || *fmt == 'H')?'E':'e', &num_buf[1]);
          if (*s == '-')
            prefix_char = *s++;
          else if (print_sign)
            prefix_char = '+';
          else if (print_blank)
            prefix_char = ' ';

          s_len = strlen(s);

          if (alternate_form && (q = strchr(s, '.')) == nullptr)
            s[s_len++] = '.';
          break;
        }

        case 'c':
          char_buf[0] = (char) (va_arg(ap, int));
          s = &char_buf[0];
          s_len = 1;
          pad_char = ' ';
          break;


        case '%':
          char_buf[0] = '%';
          s = &char_buf[0];
          s_len = 1;
          pad_char = ' ';
          break;


        case 'n':
          *(va_arg(ap, int *)) = outpos;
          goto skip_output;

          /*
           * Always extract the argument as a "char *" pointer. We
           * should be using "void *" but there are still machines
           * that don't understand it.
           * If the pointer size is equal to the size of an unsigned
           * integer we convert the pointer to a hex number, otherwise
           * we print "%p" to indicate that we don't handle "%p".
           */
        case 'p':
          if (sizeof(char *) <= sizeof(u_wide_int)) {
            ui_num = (u_wide_int)((size_t) va_arg(ap, char *));
            s = ap_php_conv_p2(ui_num, 4, 'x',
                &num_buf[NUM_BUF_SIZE], &s_len);
            if (ui_num != 0) {
              *--s = 'x';
              *--s = '0';
              s_len += 2;
            }
          } else {
            s = const_cast<char*>("%p");
            s_len = 2;
          }
          pad_char = ' ';
          break;


        case NUL:
          /*
           * The last character of the format string was %.
           * We ignore it.
           */
          continue;


fmt_error:
        throw Exception("Illegal length modifier specified '%c'", *fmt);

          /*
           * The default case is for unrecognized %'s.
           * We print %<char> to help the user identify what
           * option is not understood.
           * This is also useful in case the user wants to pass
           * the output of format_converter to another function
           * that understands some other %<char> (like syslog).
           * Note that we can't point s inside fmt because the
           * unknown <char> could be preceded by width etc.
           */
        default:
          char_buf[0] = '%';
          char_buf[1] = *fmt;
          s = char_buf;
          s_len = 2;
          pad_char = ' ';
          break;
      }

      if (prefix_char != NUL) {
        *--s = prefix_char;
        s_len++;
      }
      if (adjust_width && adjust == RIGHT && min_width > s_len) {
        if (pad_char == '0' && prefix_char != NUL) {
          appendchar(&result, &outpos, &size, *s);
          s++;
          s_len--;
          min_width--;
        }
        for (int i = 0; i < min_width - s_len; i++) {
          appendchar(&result, &outpos, &size, pad_char);
        }
      }
      /*
       * Print the (for now) non-null terminated string s.
       */
      appendsimplestring(&result, &outpos, &size, s, s_len);

      if (adjust_width && adjust == LEFT && min_width > s_len) {
        for (int i = 0; i < min_width - s_len; i++) {
          appendchar(&result, &outpos, &size, pad_char);
        }
      }
    }
skip_output:
    fmt++;
  }
  /*
   * Add the terminating null here since it wasn't added incrementally above
   * once the whole string has been composed.
   */
  appendchar(&result, &outpos, &size, NUL);
  *outbuf = result;
  return outpos - 1;
}

/*
 * This is the general purpose conversion function.
 */
int vspprintf(char** pbuf, size_t /*max_len*/, const char* format, ...) {
  int len;
  va_list ap;
  va_start(ap, format);
  len = xbuf_format_converter(pbuf, format, ap);
  va_end(ap);
  return len;
}

/*
 * Same as vspprintf but taking an va_list
 */
int vspprintf_ap(char** pbuf, size_t /*max_len*/, const char* format,
                 va_list ap) {
  int len;
  len = xbuf_format_converter(pbuf, format, ap);
  return len;
}

int spprintf(char **pbuf, size_t max_len, const char *format, ...)
{
  int cc;
  va_list ap;

  va_start(ap, format);
  cc = vspprintf(pbuf, max_len, format, ap);
  va_end(ap);
  return (cc);
}

///////////////////////////////////////////////////////////////////////////////
}
