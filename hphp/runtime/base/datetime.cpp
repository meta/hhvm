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

#include "hphp/runtime/base/datetime.h"
#include "hphp/runtime/base/dateinterval.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/array-init.h"

#include <fmt/format.h>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////
// statics

RDS_LOCAL(DateTime::LastErrors, DateTime::s_lastErrors);

const char *DateTime::DateFormatRFC822     = "D, d M y H:i:s O";
const char *DateTime::DateFormatRFC850     = "l, d-M-y H:i:s T";
const char *DateTime::DateFormatRFC1036    = "D, d M y H:i:s O";
const char *DateTime::DateFormatRFC1123    = "D, d M Y H:i:s O";
const char *DateTime::DateFormatRFC2822    = "D, d M Y H:i:s O";
const char *DateTime::DateFormatRFC3339    = "Y-m-d\\TH:i:sP";
const char *DateTime::DateFormatISO8601    = "Y-m-d\\TH:i:sO";
const char *DateTime::DateFormatCookie     = "D, d-M-Y H:i:s T";
const char *DateTime::DateFormatHttpHeader = "D, d M Y H:i:s T";

const char *DateTime::MonthNames[] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

const char *DateTime::ShortMonthNames[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

const char *DateTime::WeekdayNames[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

const char *DateTime::ShortWeekdayNames[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

const char *DateTime::GetWeekdayName(int y, int m, int d) {
  int day_of_week = timelib_day_of_week(y, m, d);
  if (day_of_week < 0) {
    return "Unknown";
  }
  return WeekdayNames[day_of_week];
}

const char *DateTime::GetShortWeekdayName(int y, int m, int d) {
  int day_of_week = timelib_day_of_week(y, m, d);
  if (day_of_week < 0) {
    return "Unknown";
  }
  return ShortWeekdayNames[day_of_week];
}

const char *DateTime::OrdinalSuffix(int number) {
  if (number >= 10 && number <= 19) {
    return "th";
  }
  switch (number % 10) {
  case 1: return "st";
  case 2: return "nd";
  case 3: return "rd";
  }
  return "th";
}

bool DateTime::IsLeap(int year) {
  // Aka "timelib_is_leap(year)" (from "timelib_private.h").
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

int DateTime::DaysInMonth(int y, int m) {
  return timelib_days_in_month(y, m);
}

bool DateTime::IsValid(int y, int m, int d) {
  return y >= 1 && y <= 32767 && m >= 1 && m <= 12 && d >= 1 &&
    d <= timelib_days_in_month(y, m);
}

req::ptr<DateTime> DateTime::Current(bool utc /* = false */) {
  return req::make<DateTime>(time(0), utc);
}

const StaticString
  s_year("year"),
  s_month("month"),
  s_day("day"),
  s_hour("hour"),
  s_minute("minute"),
  s_second("second"),
  s_zone("zone"),
  s_zone_type("zone_type"),
  s_fraction("fraction"),
  s_warning_count("warning_count"),
  s_warnings("warnings"),
  s_error_count("error_count"),
  s_errors("errors"),
  s_is_localtime("is_localtime"),
  s_is_dst("is_dst"),
  s_tz_abbr("tz_abbr"),
  s_tz_id("tz_id"),
  s_weekday("weekday"),
  s_relative("relative"),
  s_tm_sec("tm_sec"),
  s_tm_min("tm_min"),
  s_tm_hour("tm_hour"),
  s_tm_mday("tm_mday"),
  s_tm_mon("tm_mon"),
  s_tm_year("tm_year"),
  s_tm_wday("tm_wday"),
  s_tm_yday("tm_yday"),
  s_tm_isdst("tm_isdst"),
  s_unparsed("unparsed"),
  s_seconds("seconds"),
  s_minutes("minutes"),
  s_hours("hours"),
  s_mday("mday"),
  s_wday("wday"),
  s_mon("mon"),
  s_yday("yday");

#define PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(name, elem) \
  if ((int)parsed_time->elem == -99999) {                \
    ret.set(name, false);                               \
  } else {                                               \
    ret.set(name, (int)parsed_time->elem);              \
  }

Array DateTime::Parse(const OptString& datetime) {

  timelib_error_container* error;
  timelib_time* parsed_time =
    timelib_strtotime((char *)datetime.data(), datetime.size(), &error,
                      TimeZone::GetDatabase(), TimeZone::GetTimeZoneInfoRaw);
  return DateTime::ParseTime(parsed_time, error);
}

Array DateTime::Parse(const OptString& format, const OptString& date) {
  timelib_error_container* error;
  timelib_time* parsed_time =
    timelib_parse_from_format((char *)format.data(), (char *)date.data(),
                              date.size(), &error, TimeZone::GetDatabase(),
                              TimeZone::GetTimeZoneInfoRaw);
  return DateTime::ParseTime(parsed_time, error);
}

Array DateTime::ParseAsStrptime(const OptString& format, const OptString& date) {
  struct tm parsed_time;
  memset(&parsed_time, 0, sizeof(parsed_time));
  char* unparsed_part = strptime(date.data(), format.data(), &parsed_time);
  if (unparsed_part == nullptr) {
    return Array();
  }

  return make_dict_array(
    s_tm_sec,  parsed_time.tm_sec,
    s_tm_min,  parsed_time.tm_min,
    s_tm_hour, parsed_time.tm_hour,
    s_tm_mday, parsed_time.tm_mday,
    s_tm_mon,  parsed_time.tm_mon,
    s_tm_year, parsed_time.tm_year,
    s_tm_wday, parsed_time.tm_wday,
    s_tm_yday, parsed_time.tm_yday,
    s_unparsed, OptString(unparsed_part, CopyString)
  );
}

  Array DateTime::ParseTime(timelib_time* parsed_time,
                          timelib_error_container* error) {
  auto ret = Array::CreateDict();
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_year,      y);
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_month,     m);
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_day,       d);
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_hour,      h);
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_minute,    i);
  PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_second,    s);

#if TIMELIB_VERSION >= TIMELIB_MODERN
  if (parsed_time->us == TIMELIB_UNSET) {
    ret.set(s_fraction, false);
  } else {
    ret.set(s_fraction, (double) parsed_time->us / 1000000.0);
  }
#else
  if (parsed_time->f == -99999) {
    ret.set(s_fraction, false);
  } else {
    ret.set(s_fraction, parsed_time->f);
  }
#endif

  setLastErrors(error);
  {
    Array warnings = DateTime::getLastWarnings();
    ret.set(s_warning_count, warnings.size());
    ret.set(s_warnings, warnings);
  }
  {
    Array errors = DateTime::getLastErrors();
    ret.set(s_error_count, errors.size());
    ret.set(s_errors, errors);
  }

  ret.set(s_is_localtime, (bool)parsed_time->is_localtime);
  if (parsed_time->is_localtime) {
    PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_zone_type, zone_type);
    switch (parsed_time->zone_type) {
    case TIMELIB_ZONETYPE_OFFSET:
      PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_zone, z);
      ret.set(s_is_dst, (bool)parsed_time->dst);
      break;
    case TIMELIB_ZONETYPE_ID:
      if (parsed_time->tz_abbr) {
        ret.set(s_tz_abbr, OptString(parsed_time->tz_abbr, CopyString));
      }
      if (parsed_time->tz_info) {
        ret.set(s_tz_id, OptString(parsed_time->tz_info->name, CopyString));
      }
      break;
    case TIMELIB_ZONETYPE_ABBR:
      PHP_DATE_PARSE_DATE_SET_TIME_ELEMENT(s_zone, z);
      ret.set(s_is_dst, (bool)parsed_time->dst);
      ret.set(s_tz_abbr, OptString(parsed_time->tz_abbr, CopyString));
      break;
    }
  }

  if (parsed_time->have_relative) {
    auto element = make_dict_array(
      s_year,   parsed_time->relative.y,
      s_month,  parsed_time->relative.m,
      s_day,    parsed_time->relative.d,
      s_hour,   parsed_time->relative.h,
      s_minute, parsed_time->relative.i,
      s_second, parsed_time->relative.s
    );
    if (
#if defined(TIMELIB_VERSION)
        parsed_time->relative.have_weekday_relative
#else
        parsed_time->have_weekday_relative
#endif
    ) {
      element.set(s_weekday, parsed_time->relative.weekday);
    }
    ret.set(s_relative, element);
  }

  timelib_time_dtor(parsed_time);
  return ret;
}

///////////////////////////////////////////////////////////////////////////////
// constructors

DateTime::DateTime() : m_timestamp(-1), m_timestampSet(false) {
  m_time = TimePtr(timelib_time_ctor(), time_deleter());
  setTimezone(TimeZone::Current());
}

DateTime::DateTime(int64_t timestamp, bool utc /* = false */) {
  fromTimeStamp(timestamp, utc);
}

DateTime::DateTime(int64_t timestamp, req::ptr<TimeZone> tz) : m_tz(tz) {
  fromTimeStamp(timestamp);
}

DateTime::DateTime(const DateTime& dt) :
  m_tz(dt.m_tz),
  m_timestamp(dt.m_timestamp),
  m_timestampSet(dt.m_timestampSet) {

  auto t = timelib_time_clone(dt.m_time.get());
  m_time = TimePtr(t, time_deleter());
}

void DateTime::fromTimeStamp(int64_t timestamp, bool utc /* = false */) {
  m_timestamp = timestamp;
  m_timestampSet = true;

  timelib_time *t = timelib_time_ctor();
  if (utc) {
    t->zone_type = TIMELIB_ZONETYPE_OFFSET;
    t->z = 0;
    timelib_unixtime2gmt(t, (timelib_sll)m_timestamp);
  } else {
    if (!m_tz.get()) {
      m_tz = TimeZone::Current();
    }

    if (!m_tz->isValid()) {
      raise_error("No tz info found for timezone check for tzdata package.");
    }

    // We could get a correct result for this time stamp by just using
    // ZONETYPE_OFFSET and m_tz->offset(timestamp); the problem is that
    // potentially breaks if we do another timezone-sensitive operation
    // on the result.
    t->zone_type = m_tz->type();
    switch (t->zone_type) {
      case TIMELIB_ZONETYPE_OFFSET:
        t->z = m_tz->offset(timestamp);
        break;
      case TIMELIB_ZONETYPE_ABBR:
        t->z = m_tz->offset(timestamp);
        t->dst = m_tz->dst(timestamp);
        t->tz_abbr = strdup(m_tz->abbr().data());
        break;
      case TIMELIB_ZONETYPE_ID:
        t->tz_info = m_tz->getTZInfo();
        break;
    }
    timelib_unixtime2local(t, (timelib_sll)m_timestamp);
  }
  m_time = TimePtr(t, time_deleter());
}

void DateTime::sweep() {
  m_time.reset();
}

///////////////////////////////////////////////////////////////////////////////
// informational

int DateTime::beat() const {
  int retval = ((((long)m_time->sse)-(((long)m_time->sse) -
                                       ((((long)m_time->sse) % 86400) +
                                        3600))) * 10);
  while (retval < 0) {
    retval += 864000;
  }
  retval = (retval / 864) % 1000;
  return retval;
}

int DateTime::dow() const {
  return timelib_day_of_week(year(), month(), day());
}

int DateTime::doy() const {
  return timelib_day_of_year(year(), month(), day());
}

int DateTime::isoWeek() const {
  timelib_sll iw, iy;
  timelib_isoweek_from_date(year(), month(), day(), &iw, &iy);
  return iw;
}

int DateTime::isoYear() const {
  timelib_sll iw, iy;
  timelib_isoweek_from_date(year(), month(), day(), &iw, &iy);
  return iy;
}

int DateTime::isoDow() const {
  return timelib_iso_day_of_week(year(), month(), day());
}

int DateTime::offset() const {
  if (local()) {
    switch (m_time->zone_type) {
    case TIMELIB_ZONETYPE_ABBR:
    case TIMELIB_ZONETYPE_OFFSET:

#if TIMELIB_VERSION >= TIMELIB_MODERN
      return m_time->z + m_time->dst * 3600;
#else
      return (m_time->z - (m_time->dst * 60)) * -60;
#endif

    default:
      {
        bool error;
        timelib_time_offset *offset =
          timelib_get_time_zone_info(toTimeStamp(error), m_tz->getTZInfo());
        int ret = offset->offset;
        timelib_time_offset_dtor(offset);
        return ret;
      }
    }
  }
  return 0;
}

const char *DateTime::weekdayName() const {
  return GetWeekdayName(year(), month(), day());
}

const char *DateTime::shortWeekdayName() const {
  return GetShortWeekdayName(year(), month(), day());
}

const char *DateTime::monthName() const {
  return MonthNames[month() - 1];
}

const char *DateTime::shortMonthName() const {
  return ShortMonthNames[month() - 1];
}

///////////////////////////////////////////////////////////////////////////////
// modifications

void DateTime::update() {
  if (utc()) {
    timelib_update_ts(m_time.get(), nullptr);
  } else {
    timelib_update_ts(m_time.get(), m_tz->getTZInfo());
  }
  m_timestamp = 0;
  m_timestampSet = false;
}

void DateTime::set(int hou, int min, int sec, int mon, int day, int yea) {
  /* Fill in the new data */
  if (yea != INT_MAX) {
    if (yea < 70) {
      yea += 2000;
    } else if (yea >= 70 && yea <= 100) {
      yea += 1900;
    }
    m_time->y = yea;
  }
  if (day != INT_MAX) m_time->d = day;
  if (mon != INT_MAX) m_time->m = mon;
  if (sec != INT_MAX) m_time->s = sec;
  if (min != INT_MAX) m_time->i = min;
  if (hou != INT_MAX) m_time->h = hou;
  update();
}

void DateTime::setDate(int y, int m, int d) {
  m_time->y = y;
  m_time->m = m;
  m_time->d = d;
  update();
}

void DateTime::setISODate(int y, int w, int d /* = 1 */) {
  m_time->y = y;
  m_time->m = 1;
  m_time->d = 1;
  m_time->relative.d = timelib_daynr_from_weeknr(y, w, d);
  m_time->have_relative = 1;
  update();
}

void DateTime::setTime(int hour, int minute, int second) {
  m_time->h = hour;
  m_time->i = minute;
  m_time->s = second;
  update();
#if TIMELIB_VERSION >= TIMELIB_MODERN
  timelib_update_from_sse(m_time.get());
#endif
}

void DateTime::setTimezone(req::ptr<TimeZone> timezone) {
  if (!timezone) {
    return;
  }

  m_tz = timezone->cloneTimeZone();

  if (!m_tz.get()) {
    return;
  }

  assertx(m_tz->isValid());

  switch (m_tz->type()) {
    case TIMELIB_ZONETYPE_ID:
      timelib_set_timezone(m_time.get(), m_tz->getTZInfo());
      break;
    case TIMELIB_ZONETYPE_OFFSET:
      timelib_set_timezone_from_offset(m_time.get(), m_tz->offset(0));
      break;
    case TIMELIB_ZONETYPE_ABBR: {
      timelib_abbr_info abbr;
      abbr.utc_offset = m_tz->offset(0);
      abbr.abbr = strdup(m_tz->abbr().data());
      abbr.dst = m_tz->dst(0);
      timelib_set_timezone_from_abbr(m_time.get(), abbr);
      break;
    }
  }
  timelib_unixtime2local(m_time.get(), m_time->sse);
}

bool DateTime::modify(const OptString& diff) {
  timelib_error_container* error = nullptr;
  timelib_time *tmp_time = timelib_strtotime((char*)diff.data(), diff.size(),
                                             &error, TimeZone::GetDatabase(),
                                             TimeZone::GetTimeZoneInfoRaw);
  SCOPE_EXIT {
    timelib_time_dtor(tmp_time);
    if (error) timelib_error_container_dtor(error);
  };

  if (error && error->error_count > 0) {
    raise_warning("DateTime::modify(): Failed to parse time string (%s)"
                  " at position %d (%c): %s",
      diff.c_str(), error->error_messages[0].position,
      error->error_messages[0].character, error->error_messages[0].message
    );
    return false;
  }
  internalModify(tmp_time);
  return true;
}

void DateTime::internalModify(timelib_time *t) {
  // TIMELIB_UNSET (and other constants) defined in timelib.h
  // (see hhvm-third-party)
  if (t->y != TIMELIB_UNSET) {
    m_time->y = t->y;
  }
  if (t->m != TIMELIB_UNSET) {
    m_time->m = t->m;
  }
  if (t->d != TIMELIB_UNSET) {
    m_time->d = t->d;
  }
  if (t->h != TIMELIB_UNSET) {
    m_time->h = t->h;
    m_time->i = 0;
    m_time->s = 0;
    if (t->i != TIMELIB_UNSET) {
      m_time->i = t->i;
      if (t->s != TIMELIB_UNSET) {
        m_time->s = t->s;
      }
    }
  }
#if TIMELIB_VERSION >= TIMELIB_MODERN
  if (t->us != TIMELIB_UNSET) {
    m_time->us = t->us;
  }
#endif
  internalModifyRelative(&(t->relative), t->have_relative, 1);
}

void DateTime::internalModifyRelative(timelib_rel_time *rel,
                              bool have_relative, int8_t bias) {
  m_time->relative.y = rel->y * bias;
  m_time->relative.m = rel->m * bias;
  m_time->relative.d = rel->d * bias;
  m_time->relative.h = rel->h * bias;
  m_time->relative.i = rel->i * bias;
  m_time->relative.s = rel->s * bias;
#if TIMELIB_VERSION >= TIMELIB_MODERN
  m_time->relative.us = rel->us * bias;
#endif
  m_time->relative.weekday = rel->weekday;
  m_time->have_relative = have_relative;
  m_time->relative.special = rel->special;
  m_time->relative.have_special_relative = rel->have_special_relative;
  m_time->relative.have_weekday_relative = rel->have_weekday_relative;
  m_time->relative.weekday_behavior = rel->weekday_behavior;
  m_time->relative.first_last_day_of = rel->first_last_day_of;
  m_time->sse_uptodate = 0;
  update();
  timelib_update_from_sse(m_time.get());
}

void DateTime::add(const req::ptr<DateInterval>& interval) {
  timelib_rel_time *rel = interval->get();
  internalModifyRelative(rel, true, rel->invert ? -1 :  1);
}

void DateTime::sub(const req::ptr<DateInterval>& interval) {
  timelib_rel_time *rel = interval->get();
#if TIMELIB_VERSION >= TIMELIB_MODERN
  timelib_time* new_time;
  new_time = timelib_sub(m_time.get(), rel);
  m_timestamp = 0;
  m_timestampSet = false;
  m_time = TimePtr(new_time, time_deleter());
#else
  internalModifyRelative(rel, true, rel->invert ?  1 : -1);
#endif
}

///////////////////////////////////////////////////////////////////////////////
// conversions

void DateTime::toTm(struct tm &ta) const {
  // TODO: Fixme under MSVC!
  ta.tm_sec  = second();
  ta.tm_min  = minute();
  ta.tm_hour = hour();
  ta.tm_mday = day();
  ta.tm_mon  = month() - 1;
  ta.tm_year = year() - 1900;
  ta.tm_wday = dow();
  ta.tm_yday = doy();
  if (utc()) {
    ta.tm_isdst = 0;
    ta.tm_gmtoff = 0;
    ta.tm_zone = "GMT";
  } else {
    timelib_time_offset *offset =
      timelib_get_time_zone_info(m_time->sse, m_time->tz_info);
    ta.tm_isdst = offset->is_dst;
    ta.tm_gmtoff = offset->offset;
    ta.tm_zone = offset->abbr;
    timelib_time_offset_dtor(offset);
  }
}

int64_t DateTime::toTimeStamp(bool &err) const {
  err = false;
  if (!m_timestampSet) {
    int error;
    m_timestamp = timelib_date_to_int(m_time.get(), &error);
    if (error) {
      err = true;
    } else {
      m_timestampSet = true;
    }
  }
  return m_timestamp;
}

int64_t DateTime::toInteger(char format) const {
  bool error;
  switch (format) {
  case 'd':
  case 'j': return day();
  case 'w': return dow();
  case 'z': return doy();
  case 'W': return isoWeek();
  case 'm':
  case 'n': return month();
  case 't': return DaysInMonth(year(), month());
  case 'L': return DateTime::IsLeap(year());
  case 'y': return (year() % 100);
  case 'Y': return year();
  case 'B': return beat();
  case 'g':
  case 'h': return hour12();
  case 'H':
  case 'G': return hour();
  case 'i': return minute();
  case 's': return second();
  case 'I': return (!utc() && m_tz->dst(toTimeStamp(error))) ? 1 : 0;
  case 'Z': return utc() ? 0 : m_tz->offset(toTimeStamp(error));
  case 'U': return toTimeStamp(error);
  }
  raise_invalid_argument_warning("unknown format char: %d", (int)format);
  return -1;
}

OptString DateTime::toString(const OptString& format, bool stdc /* = false */) const {
  if (format.empty()) return OptString();
  return stdc ? stdcFormat(format) : rfcFormat(format);
}

OptString DateTime::toString(DateFormat format) const {
  switch (format) {
  case DateFormat::RFC822:     return rfcFormat(DateFormatRFC822);
  case DateFormat::RFC850:     return rfcFormat(DateFormatRFC850);
  case DateFormat::RFC1036:    return rfcFormat(DateFormatRFC1036);
  case DateFormat::RFC1123:    return rfcFormat(DateFormatRFC1123);
  case DateFormat::RFC2822:    return rfcFormat(DateFormatRFC2822);
  case DateFormat::RFC3339:    return rfcFormat(DateFormatRFC3339);
  case DateFormat::ISO8601:    return rfcFormat(DateFormatISO8601);
  case DateFormat::Cookie:     return rfcFormat(DateFormatCookie);
  case DateFormat::HttpHeader: return rfcFormat(DateFormatHttpHeader);
  default:
    assertx(false);
  }
  raise_invalid_argument_warning("format: %d", static_cast<int>(format));
  return OptString();
}

OptString DateTime::rfcFormat(const OptString& format) const {
  StringBuffer s;
  bool rfc_colon = false;
  bool error;
  for (int i = 0; i < format.size(); i++) {
    switch (format.charAt(i)) {
    case 'd': s.printf("%02d", day()); break;
    case 'D': s.append(shortWeekdayName()); break;
    case 'j': s.append(day()); break;
    case 'l': s.append(weekdayName()); break;
    case 'S': s.append(OrdinalSuffix(day())); break;
    case 'w': s.append(dow()); break;
    case 'N': s.append(isoDow()); break;
    case 'z': s.append(doy()); break;
    case 'W': s.printf("%02d", isoWeek()); break;
    case 'o': s.append(isoYear()); break;
    case 'F': s.append(monthName()); break;
    case 'm': s.printf("%02d", month()); break;
    case 'M': s.append(shortMonthName()); break;
    case 'n': s.append(month()); break;
    case 't': s.append(DaysInMonth(year(), month())); break;
    case 'L': s.append(IsLeap(year())); break;
    case 'y': s.printf("%02d", year() % 100); break;
    case 'Y': s.printf("%s%04d", year() < 0 ? "-" : "", abs(year()));
      break;
    case 'a': s.append(hour() >= 12 ? "pm" : "am"); break;
    case 'A': s.append(hour() >= 12 ? "PM" : "AM"); break;
    case 'B': s.printf("%03d", beat()); break;
    case 'g': s.append((hour() % 12) ? (int)hour() % 12 : 12); break;
    case 'G': s.append(hour()); break;
    case 'h': s.printf("%02d", (hour() % 12) ? (int)hour() % 12 : 12); break;
    case 'H': s.printf("%02d", (int)hour()); break;
    case 'i': s.printf("%02d", (int)minute()); break;
    case 's': s.printf("%02d", (int)second()); break;
#if TIMELIB_VERSION >= TIMELIB_MODERN
    case 'u': s.printf("%06d", (int)m_time->us); break;
    case 'v': s.printf("%03d", (int)(m_time->us / 1000)); break;
#else
    case 'u': s.printf("%06d", (int)floor(fraction() * 1000000)); break;
    case 'v': s.printf("%03d", (int)floor(fraction() * 1000)); break;
#endif
    case 'I': s.append(!utc() && m_tz->dst(toTimeStamp(error)) ? 1 : 0);
      break;
    case 'P': rfc_colon = true;
      [[fallthrough]];
    case 'O':
      if (utc()) {
        s.printf("+00%s00", rfc_colon ? ":" : "");
      } else {
        int offset = this->offset();
        s.printf("%c%02d%s%02d",
                 (offset < 0 ? '-' : '+'), abs(offset / 3600),
                 rfc_colon ? ":" : "", abs((offset % 3600) / 60));
      }
      break;
    case 'T':
      if (utc()) {
        s.append("GMT");
      } else {
        if (m_time->zone_type != TIMELIB_ZONETYPE_OFFSET) {
          s.append(m_time->tz_abbr);
        } else {
#if TIMELIB_VERSION >= TIMELIB_MODERN
          auto offset = m_time->z;
#else
          auto offset = m_time->z * -60;
#endif

          char abbr[9] = {0};
          snprintf(abbr, 9, "GMT%c%02d%02d",
            ((offset < 0) ? '-' : '+'),
            abs(offset / 3600) % 100, // % 100 to convince compiler we have 2 digits
            abs((offset % 3600) / 60));
          s.append(abbr);
        }
      }
      break;
    case 'e':
      if (utc()) {
        s.append("UTC");
      } else {
        if (m_time->zone_type != TIMELIB_ZONETYPE_OFFSET) {
          s.append(m_tz->name());
        } else {
#if TIMELIB_VERSION >= TIMELIB_MODERN
          auto offset = m_time->z;
#else
          auto offset = m_time->z * -60;
#endif
          char abbr[7] = {0};
          snprintf(abbr, 7, "%c%02d:%02d",
            ((offset < 0) ? '-' : '+'),
            abs(offset / 3600) % 100, // % 100 to convince compiler we have 2 digits
            abs((offset % 3600) / 60));
          s.append(abbr);
        }
      }
      break;
    case 'Z': s.append(utc() ? 0 : this->offset()); break;
    case 'c':
      if (utc()) {
        s.printf("%04d-%02d-%02dT%02d:%02d:%02d+00:00",
                 year(), month(), day(), hour(), minute(), second());
      } else {
        int offset = this->offset();
        s.printf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                 year(), month(), day(), hour(), minute(), second(),
                 (offset < 0 ? '-' : '+'),
                 abs(offset / 3600), abs((offset % 3600) / 60));
      }
      break;
    case 'r':
      if (utc()) {
        s.printf("%3s, %02d %3s %04d %02d:%02d:%02d +0000",
                 shortWeekdayName(), day(), shortMonthName(), year(),
                 hour(), minute(), second());
      } else {
        int offset = this->offset();
        s.printf("%3s, %02d %3s %04d %02d:%02d:%02d %c%02d%02d",
                 shortWeekdayName(), day(), shortMonthName(), year(),
                 hour(), minute(), second(),
                 (offset < 0 ? '-' : '+'),
                 abs(offset / 3600), abs((offset % 3600) / 60));
      }
      break;
    case 'U': s.printf("%" PRId64, toTimeStamp(error)); break;
    case '\\':
      if (i < format.size()) i++;
      [[fallthrough]];
    default:
      s.append(format[i]);
      break;
    }
  }
  return s.detach();
}

OptString DateTime::stdcFormat(const OptString& format) const {
  // TODO: Fixme under MSVC!
  struct tm ta;
  timelib_time_offset *offset = nullptr;
  ta.tm_sec  = second();
  ta.tm_min  = minute();
  ta.tm_hour = hour();
  ta.tm_mday = day();
  ta.tm_mon  = month() - 1;
  ta.tm_year = year() - 1900;
  ta.tm_wday = dow();
  ta.tm_yday = doy();
  if (utc()) {
    ta.tm_isdst = 0;
    ta.tm_gmtoff = 0;
    ta.tm_zone = "GMT";
  } else {
    offset = timelib_get_time_zone_info(m_time->sse, m_time->tz_info);
    ta.tm_isdst = offset->is_dst;
    ta.tm_gmtoff = offset->offset;
    ta.tm_zone = offset->abbr;
  }

  if ((ta.tm_sec < 0 || ta.tm_sec > 60) ||
      (ta.tm_min < 0 || ta.tm_min > 59) ||
      (ta.tm_hour < 0 || ta.tm_hour > 23) ||
      (ta.tm_mday < 1 || ta.tm_mday > 31) ||
      (ta.tm_mon < 0 || ta.tm_mon > 11) ||
      (ta.tm_wday < 0 || ta.tm_wday > 6) ||
      (ta.tm_yday < 0 || ta.tm_yday > 365)) {
    raise_invalid_argument_warning("argument: invalid time");
    return OptString();
  }

  int max_reallocs = 5;
  size_t buf_len = 256, real_len;
  char *buf = (char *)malloc(buf_len);
  while ((real_len = strftime(buf, buf_len, format.data(), &ta)) == buf_len ||
         real_len == 0) {
    buf_len *= 2;
    free(buf);
    buf = (char *)malloc(buf_len);
    if (!--max_reallocs) {
      break;
    }
  }
  if (!utc()) {
    timelib_time_offset_dtor(offset);
  }
  if (real_len && real_len != buf_len) {
    return OptString(buf, real_len, AttachString);
  }
  free(buf);
  raise_invalid_argument_warning("format: (over internal buffer)");
  return OptString();
}

Array DateTime::toArray(ArrayFormat format) const {
  bool error;
  switch (format) {
  case ArrayFormat::TimeMap:
    return make_dict_array(
      s_seconds, second(),
      s_minutes, minute(),
      s_hours,   hour(),
      s_mday,    day(),
      s_wday,    dow(),
      s_mon,     month(),
      s_year,    year(),
      s_yday,    doy(),
      s_weekday, weekdayName(),
      s_month,   monthName(),
      0,         toTimeStamp(error)
    );
  case ArrayFormat::TmMap:
    {
      struct tm tm;
      toTm(tm);
      return make_dict_array(
        s_tm_sec,   tm.tm_sec,
        s_tm_min,   tm.tm_min,
        s_tm_hour,  tm.tm_hour,
        s_tm_mday,  tm.tm_mday,
        s_tm_mon,   tm.tm_mon,
        s_tm_year,  tm.tm_year,
        s_tm_wday,  tm.tm_wday,
        s_tm_yday,  tm.tm_yday,
        s_tm_isdst, tm.tm_isdst
      );
    }
  case ArrayFormat::TmVector:
    {
      struct tm tm;
      toTm(tm);
      return make_vec_array(
        tm.tm_sec,
        tm.tm_min,
        tm.tm_hour,
        tm.tm_mday,
        tm.tm_mon,
        tm.tm_year,
        tm.tm_wday,
        tm.tm_yday,
        tm.tm_isdst
      );
    }
  }
  return empty_vec_array();
}

bool DateTime::fromString(const OptString& input, req::ptr<TimeZone> tz,
                          const char* format /*=NUL*/,
                          bool throw_on_error /*= true*/) {
  timelib_error_container *error;
  timelib_time *t;
  if (format) {
    t = timelib_parse_from_format((char*)format, (char*)input.data(),
                                  input.size(), &error, TimeZone::GetDatabase(),
                                  TimeZone::GetTimeZoneInfoRaw);
  } else {
    t = timelib_strtotime((char*)input.data(), input.size(),
                                 &error, TimeZone::GetDatabase(),
                                 TimeZone::GetTimeZoneInfoRaw);
  }
  int error1 = error->error_count;
  setLastErrors(error);
  if (error1) {
    timelib_time_dtor(t);
    if (!throw_on_error) {
      return false;
    }
    auto msg = fmt::format(
      "DateTime::__construct(): Failed to parse time string "
      "({}) at position {} ({}): {}",
      input,
      error->error_messages[0].position,
      error->error_messages[0].character,
      error->error_messages[0].message
    );
    SystemLib::throwExceptionObject(msg);
  }

  if (m_timestamp == -1) {
    fromTimeStamp(0);
  }
  if (tz.get() && tz->isValid() && (input.size() <= 0 || input[0] != '@')) {
    setTimezone(tz);
  } else {
    setTimezone(TimeZone::Current());
  }

  int options = TIMELIB_NO_CLONE;
#if TIMELIB_VERSION >= TIMELIB_MODERN
	if (format != nullptr) {
    	options |= TIMELIB_OVERRIDE_TIME;
  }
#endif
  timelib_fill_holes(t, m_time.get(), options);
  timelib_update_ts(t, m_tz->getTZInfo());
  timelib_update_from_sse(t);
  int error2;
  m_timestamp = timelib_date_to_int(t, &error2);
  if (error1 || error2) {
    // Don't free t->tz_info, it belongs to GetTimeZoneInfo
    timelib_time_dtor(t);
    return false;
  }

  m_time = TimePtr(t, time_deleter());
  if (t->tz_info != m_tz->getTZInfo()) {
    m_tz = req::make<TimeZone>(t->tz_info);
  }
  return true;
}

req::ptr<DateTime> DateTime::cloneDateTime() const {
  return req::make<DateTime>(*this);
}

///////////////////////////////////////////////////////////////////////////////
// comparison

req::ptr<DateInterval>
DateTime::diff(req::ptr<DateTime> datetime2, bool absolute) {
  timelib_rel_time *rel = timelib_diff(m_time.get(), datetime2.get()->m_time.get());
  if (absolute) {
    rel->invert = 0;
  }
  return req::make<DateInterval>(rel);
}

int DateTime::compare(req::ptr<DateTime> datetime2) {
  return timelib_time_compare(m_time.get(), datetime2.get()->m_time.get());
}

///////////////////////////////////////////////////////////////////////////////
// sun

const StaticString
  s_sunrise("sunrise"),
  s_sunset("sunset"),
  s_transit("transit"),
  s_civil_twilight_begin("civil_twilight_begin"),
  s_civil_twilight_end("civil_twilight_end"),
  s_nautical_twilight_begin("nautical_twilight_begin"),
  s_nautical_twilight_end("nautical_twilight_end"),
  s_astronomical_twilight_begin("astronomical_twilight_begin"),
  s_astronomical_twilight_end("astronomical_twilight_end");

Array DateTime::getSunInfo(double latitude, double longitude) const {
  Array ret = Array::CreateDict();
  timelib_sll sunrise, sunset, transit;
  double ddummy;

  /* Get sun up/down and transit */
  int rs = timelib_astro_rise_set_altitude(m_time.get(), longitude, latitude,
                                           -35.0/60, 1, &ddummy, &ddummy,
                                           &sunrise, &sunset, &transit);
  switch (rs) {
  case -1: /* always below */
    ret.set(s_sunrise, false);
    ret.set(s_sunset, false);
    break;
  case 1: /* always above */
    ret.set(s_sunrise, true);
    ret.set(s_sunset, true);
    break;
  default:
    ret.set(s_sunrise, sunrise);
    ret.set(s_sunset, sunset);
  }
  ret.set(s_transit, transit);

  /* Get civil twilight */
  rs = timelib_astro_rise_set_altitude(m_time.get(), longitude, latitude,
                                       -6.0, 0,
                                       &ddummy, &ddummy, &sunrise, &sunset,
                                       &transit);
  switch (rs) {
  case -1: /* always below */
    ret.set(s_civil_twilight_begin, false);
    ret.set(s_civil_twilight_end, false);
    break;
  case 1: /* always above */
    ret.set(s_civil_twilight_begin, true);
    ret.set(s_civil_twilight_end, true);
    break;
  default:
    ret.set(s_civil_twilight_begin, sunrise);
    ret.set(s_civil_twilight_end, sunset);
  }

  /* Get nautical twilight */
  rs = timelib_astro_rise_set_altitude(m_time.get(), longitude, latitude,
                                       -12.0, 0,
                                       &ddummy, &ddummy, &sunrise, &sunset,
                                       &transit);
  switch (rs) {
  case -1: /* always below */
    ret.set(s_nautical_twilight_begin, false);
    ret.set(s_nautical_twilight_end, false);
    break;
  case 1: /* always above */
    ret.set(s_nautical_twilight_begin, true);
    ret.set(s_nautical_twilight_end, true);
    break;
  default:
    ret.set(s_nautical_twilight_begin, sunrise);
    ret.set(s_nautical_twilight_end, sunset);
  }

  /* Get astronomical twilight */
  rs = timelib_astro_rise_set_altitude(m_time.get(), longitude, latitude,
                                       -18.0, 0,
                                       &ddummy, &ddummy, &sunrise, &sunset,
                                       &transit);
  switch (rs) {
  case -1: /* always below */
    ret.set(s_astronomical_twilight_begin, false);
    ret.set(s_astronomical_twilight_end, false);
    break;
  case 1: /* always above */
    ret.set(s_astronomical_twilight_begin, true);
    ret.set(s_astronomical_twilight_end, true);
    break;
  default:
    ret.set(s_astronomical_twilight_begin, sunrise);
    ret.set(s_astronomical_twilight_end, sunset);
  }
  return ret;
}

Variant DateTime::getSunInfo(SunInfoFormat retformat,
                             double latitude, double longitude,
                             double zenith, double utc_offset,
                             bool calc_sunset) const {
  if (retformat != SunInfoFormat::ReturnTimeStamp &&
      retformat != SunInfoFormat::ReturnString &&
      retformat != SunInfoFormat::ReturnDouble) {
    raise_warning("Wrong return format given, pick one of "
                    "SUNFUNCS_RET_TIMESTAMP, SUNFUNCS_RET_STRING or "
                    "SUNFUNCS_RET_DOUBLE");
    return false;
  }
  double altitude = 90 - zenith;
  double h_rise, h_set;
  timelib_sll sunrise, sunset, transit;
  int rs = timelib_astro_rise_set_altitude(m_time.get(), longitude, latitude,
                                           altitude, 1,
                                           &h_rise, &h_set, &sunrise, &sunset,
                                           &transit);
  if (rs != 0) {
    return false;
  }

  if (retformat == SunInfoFormat::ReturnTimeStamp) {
    return calc_sunset ? sunset : sunrise;
  }

  double N = (calc_sunset ? h_set : h_rise) + utc_offset;
  if (N > 24 || N < 0) {
    N -= floor(N / 24) * 24;
  }

  if (retformat == SunInfoFormat::ReturnString) {
    char retstr[6];
    snprintf(retstr, sizeof(retstr),
             "%02d:%02d", (int) N, (int) (60 * (N - (int) N)));
    return OptString(retstr, CopyString);
  }

  assertx(retformat == SunInfoFormat::ReturnDouble);
  return N;
}

///////////////////////////////////////////////////////////////////////////////
}
