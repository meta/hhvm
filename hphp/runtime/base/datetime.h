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

#include <memory>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/request-event-handler.h"
#include "hphp/runtime/base/req-ptr.h"
#include "hphp/runtime/base/timezone.h"
#include "hphp/runtime/base/dateinterval.h"
#include "hphp/util/rds-local.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/**
 * Encapsulating all date/time manipulations, conversions, input and output
 * into this one single class.
 */
struct DateTime final : SweepableResourceData {
  DECLARE_RESOURCE_ALLOCATION(DateTime)

  /**
   * Different RFC/ISO date/time formats for toString(DateFormat).
   */
  enum class DateFormat {
    InvalidFormat,

    /**
     * RFC822, Section 5.1: http://www.ietf.org/rfc/rfc822.txt
     *  date-time   =  [ day "," ] date time  ; dd mm yy hh:mm:ss zzz
     *  day         =  "Mon" / "Tue" / "Wed" / "Thu" / "Fri" / "Sat" / "Sun"
     *  date        =  1*2DIGIT month 2DIGIT  ; day month year e.g. 20 Jun 82
     *  month       =  "Jan" / "Feb" / "Mar" / "Apr" / "May" / "Jun" /
     *                 "Jul" / "Aug" / "Sep" / "Oct" / "Nov" / "Dec"
     *  time        =  hour zone              ; ANSI and Military
     *  hour        =  2DIGIT ":" 2DIGIT [":" 2DIGIT] ; 00:00:00 - 23:59:59
     *  zone        =  "UT" / "GMT" / "EST" / "EDT" / "CST" / "CDT" / "MST" /
     *                 "MDT" / "PST" / "PDT" / 1ALPHA / (("+" / "-") 4DIGIT)
     */
    RFC822,

    /**
     * RFC850, Section 2.1.4: http://www.ietf.org/rfc/rfc850.txt
     *  Format must be acceptable both to the ARPANET and to the getdate
     *  routine. One format that is acceptable to both is Weekday,
     *  DD-Mon-YY HH:MM:SS TIMEZONE
     *  TIMEZONE can be any timezone name (3 or more letters)
     */
    RFC850,

    /**
     * RFC1036, Section 2.1.2: http://www.ietf.org/rfc/rfc1036.txt
     *  Its format must be acceptable both in RFC-822 and to the getdate(3)
     *  Wdy, DD Mon YY HH:MM:SS TIMEZONE
     *  There is no hope of having a complete list of timezones.  Universal
     *  Time (GMT), the North American timezones (PST, PDT, MST, MDT, CST,
     *  CDT, EST, EDT) and the +/-hhmm offset specified in RFC-822 should be
     *  supported.
     */
    RFC1036,

    /**
     * RFC1123, Section 5.2.14: http://www.ietf.org/rfc/rfc1123.txt
     *  RFC-822 Date and Time Specification: RFC-822 Section 5
     *  The syntax for the date is hereby changed to:
     *    date = 1*2DIGIT month 2*4DIGIT
     */
    RFC1123,

    /**
     * RFC2822, Section 3.3: http://www.ietf.org/rfc/rfc2822.txt
     *  FWS             =       ([*WSP CRLF] 1*WSP) /   ; Folding white space
     *  CFWS            =       *([FWS] comment) (([FWS] comment) / FWS)
     *
     *  date-time       =       [ day-of-week "," ] date FWS time [CFWS]
     *  day-of-week     =       ([FWS] day-name)
     *  day-name        =       "Mon"/"Tue"/"Wed"/"Thu"/"Fri"/"Sat"/"Sun"
     *  date            =       day month year
     *  year            =       4*DIGIT
     *  month           =       (FWS month-name FWS)
     *  month-name      =       "Jan" / "Feb" / "Mar" / "Apr" / "May" / "Jun" /
     *                          "Jul" / "Aug" / "Sep" / "Oct" / "Nov" / "Dec"
     *  day             =       ([FWS] 1*2DIGIT)
     *  time            =       time-of-day FWS zone
     *  time-of-day     =       hour ":" minute [ ":" second ]
     *  hour            =       2DIGIT
     *  minute          =       2DIGIT
     *  second          =       2DIGIT
     *  zone            =       (( "+" / "-" ) 4DIGIT)
     */
    RFC2822,

    /**
     * RFC3339, Section 5.6: http://www.ietf.org/rfc/rfc3339.txt
     *  date-fullyear   = 4DIGIT
     *  date-month      = 2DIGIT  ; 01-12
     *  date-mday       = 2DIGIT  ; 01-28, 01-29, 01-30, 01-31 based on
     *                            ; month/year
     *
     *  time-hour       = 2DIGIT  ; 00-23
     *  time-minute     = 2DIGIT  ; 00-59
     *  time-second     = 2DIGIT  ; 00-58, 00-59, 00-60 based on
     *                            ; leap second rules
     *
     *  time-secfrac    = "." 1*DIGIT
     *  time-numoffset  = ("+" / "-") time-hour ":" time-minute
     *  time-offset     = "Z" / time-numoffset
     *
     *  partial-time    = time-hour ":" time-minute ":" time-second
     *                    [time-secfrac]
     *  full-date       = date-fullyear "-" date-month "-" date-mday
     *  full-time       = partial-time time-offset
     *
     *  date-time       = full-date "T" full-time
     */
    RFC3339,

    ISO8601,

    /**
     * Preliminary specification:
     *   http://wp.netscape.com/newsref/std/cookie_spec.html
     *   "This is based on RFC 822, RFC 850,  RFC 1036, and  RFC 1123,
     *   with the variations that the only legal time zone is GMT
     *   and the separators between the elements of the date must be dashes."
     */
    Cookie,

    /**
     * Similar to Cookie without dashes.
     */
    HttpHeader,

    /**
     * RFC4287, Section 3.3: http://www.ietf.org/rfc/rfc4287.txt
     *   A Date construct is an element whose content MUST conform to the
     *   "date-time" production in [RFC3339].  In addition, an uppercase "T"
     *   character MUST be used to separate date and time, and an uppercase
     *   "Z" character MUST be present in the absence of a numeric time zone
     *   offset.
     */
    Atom = RFC3339,

    /**
     * RSS 2.0 Specification: http://blogs.law.harvard.edu/tech/rss
     *   "All date-times in RSS conform to the Date and Time Specification of
     *    RFC 822, with the exception that the year may be expressed with two
     *    characters or four characters (four preferred)"
     */
    RSS = RFC1123,

    W3C = RFC3339,
  };

  /**
   * Different array formats for toArray().
   */
  enum class ArrayFormat {
    TimeMap,
    TmMap,
    TmVector
  };

  /**
   * Different formats for getSunInfo().
   */
  enum class SunInfoFormat {
    ReturnTimeStamp,
    ReturnString,
    ReturnDouble
  };

public:
  static const char *DateFormatRFC822;
  static const char *DateFormatRFC850;
  static const char *DateFormatRFC1036;
  static const char *DateFormatRFC1123;
  static const char *DateFormatRFC2822;
  static const char *DateFormatRFC3339;
  static const char *DateFormatISO8601;
  static const char *DateFormatCookie;
  static const char *DateFormatHttpHeader;

  static const char *MonthNames[];
  static const char *ShortMonthNames[];
  static const char *WeekdayNames[];
  static const char *ShortWeekdayNames[];

  static const char *GetWeekdayName(int y, int m, int d);
  static const char *GetShortWeekdayName(int y, int m, int d);
  static const char *OrdinalSuffix(int number);

  static bool IsLeap(int year);
  static int DaysInMonth(int y, int m);
  static bool IsValid(int y, int m, int d);

  /**
   * What time is it?
   */
  static req::ptr<DateTime> Current(bool utc = false);

  /**
   * Returns are really in special PHP formats, and please read datetime.cpp
   * for details.
   */
  static Array Parse(const OptString& datetime);
  static Array Parse(const OptString& format, const OptString& date);
  static Array ParseAsStrptime(const OptString& format, const OptString& date);

public:
  // constructor
  DateTime();
  DateTime(const DateTime&);
  explicit DateTime(int64_t timestamp, bool utc = false); // from a timestamp
  explicit DateTime(int64_t timestamp, req::ptr<TimeZone> tz);

  CLASSNAME_IS("DateTime")
  // overriding ResourceData
  const OptString& o_getClassNameHook() const override { return classnameof(); }

  // informational
  bool local() const { return m_time->is_localtime;}
  bool utc() const   { return !m_time->is_localtime;}
  int year() const   { return m_time->y;}
  int month() const  { return m_time->m;}
  int day() const    { return m_time->d;}
  int hour() const   { return m_time->h;}
  int hour12() const { return (m_time->h % 12) ? (int) m_time->h % 12 : 12;}
  int minute() const { return m_time->i;}
  int second() const { return m_time->s;}
  double fraction() const {
#if TIMELIB_VERSION >= TIMELIB_MODERN
      return m_time->us / 1000000.0;
#else
      return m_time->f;
#endif
  }
  int zoneType() const { return m_time->zone_type;}
  int beat() const;    // Swatch Beat a.k.a. Internet Time
  int dow() const;     // day of week
  int doy() const;     // day of year
  int isoWeek() const;
  int isoYear() const;
  int isoDow() const;
  int offset() const;  // timezone offset from UTC
  req::ptr<TimeZone> timezone() const { return m_tz->cloneTimeZone();}

  const char *weekdayName() const;
  const char *shortWeekdayName() const;
  const char *monthName() const;
  const char *shortMonthName() const;

  // modifications
  void set(int hour, int minute, int second, int month, int day, int year);
  void setDate(int year, int month, int day);
  void setISODate(int year, int week, int day = 1);
  void setTime(int hour, int minute, int second = 0);
  void setTimezone(req::ptr<TimeZone> tz);
  bool modify(const OptString& diff); // PHP's date_modify() function, muy powerful
  void add(const req::ptr<DateInterval>& interval);
  void sub(const req::ptr<DateInterval>& interval);

  // conversions
  void toTm(struct tm &ta) const;
  int64_t toTimeStamp(bool &err) const;
  int64_t toInteger(char format) const;
  OptString toString(const OptString& format, bool stdc = false) const;
  OptString toString(DateFormat format) const;
  Array toArray(ArrayFormat format) const;
  void fromTimeStamp(int64_t timestamp, bool utc = false);
  bool fromString(const OptString& input, req::ptr<TimeZone> tz,
                  const char* format=nullptr, bool throw_on_error = true);

  // comparison
  req::ptr<DateInterval> diff(req::ptr<DateTime> datetime2,
                              bool absolute = false);
  int compare(req::ptr<DateTime> datetime2);

  // cloning
  req::ptr<DateTime> cloneDateTime() const;

  // sun info
  Array getSunInfo(double latitude, double longitude) const;
  Variant getSunInfo(SunInfoFormat retformat,
                     double latitude, double longitude,
                     double zenith, double utc_offset, bool calc_sunset) const;

  // Error access
private:
  void internalModify(timelib_time *t);
  void internalModifyRelative(timelib_rel_time *rel, bool have_relative,
                              int8_t bias);
  struct LastErrors final : RequestEventHandler {
    void requestInit() override {
      m_errors = nullptr;
    }
    void requestShutdown() override {
      if (m_errors) {
        timelib_error_container_dtor(m_errors);
      }
    }
    void set(timelib_error_container *ec) {
      requestShutdown();
      m_errors = ec;
    }
    Array getLastWarnings() const {
      if (!m_errors) return Array::CreateDict();
      DictInit ret(m_errors->warning_count);
      for(int i = 0; i < m_errors->warning_count; i++) {
        timelib_error_message *em = m_errors->warning_messages + i;
        ret.set(em->position, OptString(em->message, CopyString));
      }
      return ret.toArray();
    }
    Array getLastErrors() const {
      if (!m_errors) return Array::CreateDict();
      DictInit ret(m_errors->error_count);
      for(int i = 0; i < m_errors->error_count; i++) {
        timelib_error_message *em = m_errors->error_messages + i;
        ret.set(em->position, OptString(em->message, CopyString));
      }
      return ret.toArray();
    }

  private:
    timelib_error_container *m_errors;
  };
  static RDS_LOCAL(LastErrors, s_lastErrors);

public:
  static void setLastErrors(timelib_error_container *ec)
    { s_lastErrors.get()->set(ec); }
  static Array getLastWarnings()
    { return s_lastErrors.get()->getLastWarnings(); }
  static Array getLastErrors()
    { return s_lastErrors.get()->getLastErrors(); }

private:
  struct time_deleter {
    void operator()(timelib_time *t) {
      timelib_time_dtor(t);
    }
  };
  using TimePtr = std::shared_ptr<timelib_time>;

  TimePtr m_time;
  req::ptr<TimeZone> m_tz;
  mutable int64_t m_timestamp;
  mutable bool m_timestampSet;

  // helpers
  static Array ParseTime(timelib_time* time,
                        timelib_error_container* error);

  void update();
  OptString rfcFormat(const OptString& format) const;
  OptString stdcFormat(const OptString& format) const;
};

///////////////////////////////////////////////////////////////////////////////
}
