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

#include "hphp/runtime/ext/std/ext_std_file.h"

#include "hphp/util/hphp-config.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-util.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/directory.h"
#include "hphp/runtime/base/file-stream-wrapper.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/http-stream-wrapper.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/pipe.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/temp-file.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/stat-cache.h"
#include "hphp/runtime/base/stream-wrapper-registry.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/ext/hash/ext_hash.h"
#if ENABLE_EXTENSION_POSIX
#include "hphp/runtime/ext/posix/ext_posix.h"
#endif
#include "hphp/runtime/ext/std/ext_std_options.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/static-content-cache.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/logger.h"
#include "hphp/util/rds-local.h"
#include "hphp/util/user-info.h"

#include <folly/Likely.h>
#include <folly/String.h>
#include <folly/portability/Dirent.h>
#include <folly/portability/SysFile.h>

#include <glob.h>
#include <utime.h>
#include <grp.h>
#include <pwd.h>
#include <fnmatch.h>

#include <boost/algorithm/string/predicate.hpp>

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

#define CHECK_HANDLE_BASE(handle, f, ret)               \
  auto f = dyn_cast_or_null<File>(handle);              \
  if (f == nullptr || f->isClosed()) {                  \
    raise_warning("Not a valid stream resource");       \
    return (ret);                                       \
  }                                                     \

#define CHECK_HANDLE(handle, f) \
  CHECK_HANDLE_BASE(handle, f, false)
#define CHECK_HANDLE_RET_NULL(handle, f) \
  CHECK_HANDLE_BASE(handle, f, uninit_variant)

#define CHECK_PATH_BASE(p, i, ret) \
  if (p.size() != strlen(p.data())) { \
    raise_warning( \
      "%s() expects parameter %d to be a valid path, string given", \
      __FUNCTION__ + 2, i \
    ); \
    return (ret); \
  }

#define CHECK_PATH(p, i) \
  CHECK_PATH_BASE(p, i, uninit_variant)
#define CHECK_PATH_FALSE(p, i) \
  CHECK_PATH_BASE(p, i, false)

#define CHECK_SYSTEM(exp)                                 \
  if ((exp) != 0) {                                       \
    raise_warning(                                        \
      "%s(): %s",                                         \
       __FUNCTION__ + 2,                                  \
       folly::errnoStr(errno).c_str()                     \
    );                                                    \
    return false;                                         \
  }                                                       \

#define CHECK_SYSTEM_SILENT(exp)                          \
  if ((exp) != 0) {                                       \
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,  \
                    folly::errnoStr(errno).c_str());      \
    return false;                                         \
  }                                                       \

#define CHECK_FS(dest, func, path)                        \
  std::error_code ec;                                     \
  dest = func(path.toCppString(), ec);                    \
  if (ec) {                                               \
    raise_warning(                                        \
      "%s(): %s",                                         \
      __FUNCTION__ + 2,                                   \
      ec.message().c_str()                                \
    );                                                    \
    return false;                                         \
  }                                                       \

#define CHECK_FS_SILENT(dest, func, path)                 \
  std::error_code ec;                                     \
  dest = func(path.toCppString(), ec);                    \
  if (ec) {                                               \
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,  \
      ec.message().c_str()                                \
    );                                                    \
    return false;                                         \
  }                                                       \

#define CHECK_FS_ASSIGN(func, path, arg)                  \
  std::error_code ec;                                     \
  func(path.toCppString(), arg, ec);                      \
  if (ec) {                                               \
    raise_warning(                                        \
      "%s(): %s",                                         \
      __FUNCTION__ + 2,                                   \
      ec.message().c_str()                                \
    );                                                    \
    return false;                                         \
  }                                                       \

// libxml/xpathInternals.h defines CHECK_ERROR,
// we need to undef it first
#ifdef CHECK_ERROR
#undef CHECK_ERROR
#endif
#define CHECK_ERROR(ret)                                 \
  check_error(__FUNCTION__, __LINE__, (ret))

namespace HPHP {

static const StaticString s_STREAM_URL_STAT_LINK("STREAM_URL_STAT_LINK");
static const StaticString s_STREAM_URL_STAT_QUIET("STREAM_URL_STAT_QUIET");

///////////////////////////////////////////////////////////////////////////////
// helpers

static bool check_error(const char *function, int line, bool ret) {
  if (!ret) {
    Logger::Verbose("%s/%d: %s", function, line,
                    folly::errnoStr(errno).c_str());
  }
  return ret;
}

static int accessSyscall(
    const OptString& uri_or_path,
    int mode,
    bool useFileCache = false) {
  Stream::Wrapper* w = Stream::getWrapperFromURI(uri_or_path);
  if (!w) return -1;

  OptString path(uri_or_path);
  if (UNLIKELY(StringUtil::IsFileUrl(uri_or_path.data()))) {
    path = StringUtil::DecodeFileUrl(uri_or_path);
    if (path.empty()) {
      return -1;
    }
  }
  if (useFileCache && dynamic_cast<FileStreamWrapper*>(w)) {
    return ::access(File::TranslatePathWithFileCache(path).data(), mode);
  }
  return w->access(path, mode);
}

static int statSyscall(
    const OptString& path,
    struct stat* buf,
    bool useFileCache = false) {
  bool isRelative = !FileUtil::isAbsolutePath(path.slice());
  int pathIndex = 0;
  Stream::Wrapper* w = Stream::getWrapperFromURI(path, &pathIndex);
  if (!w) return -1;
  bool isFileStream = dynamic_cast<FileStreamWrapper*>(w);
  auto canUseFileCache = useFileCache && isFileStream;
  if (isRelative && !pathIndex) {
    auto fullpath = g_context->getCwd() + OptString::FromChar('/') + path;
    if (!RID().hasSafeFileAccess() && !canUseFileCache) {
      if (strlen(fullpath.data()) != fullpath.size()) return ENOENT;
      if (!isFileStream && w->isNormalFileStream()) {
        return w->stat(fullpath.data(), buf);
      }
      return ::stat(fullpath.data(), buf);
    }
    std::string realpath = realpathLibc(fullpath.data());
    // realpath will return an empty string for nonexistent files
    if (realpath.empty()) {
      return ENOENT;
    }
    auto translatedPath = canUseFileCache ?
      File::TranslatePathWithFileCache(realpath) :
      File::TranslatePath(realpath);
    if (!isFileStream && w->isNormalFileStream()) {
      return w->stat(translatedPath.data(), buf);
    }
    return ::stat(translatedPath.data(), buf);
  }

  auto properPath = w->isNormalFileStream() ? path.substr(pathIndex) : path;
  if (canUseFileCache) {
    return ::stat(File::TranslatePathWithFileCache(properPath).data(), buf);
  }
  return w->stat(properPath, buf);
}

static int lstatSyscall(
    const OptString& path,
    struct stat* buf,
    bool useFileCache = false) {
  Stream::Wrapper* w = Stream::getWrapperFromURI(path);
  if (!w) return -1;
  if (useFileCache && dynamic_cast<FileStreamWrapper*>(w)) {
    return ::lstat(File::TranslatePathWithFileCache(path).data(), buf);
  }
  return w->lstat(path, buf);
}

const StaticString
  s_dev("dev"),
  s_ino("ino"),
  s_mode("mode"),
  s_nlink("nlink"),
  s_uid("uid"),
  s_gid("gid"),
  s_rdev("rdev"),
  s_size("size"),
  s_atime("atime"),
  s_mtime("mtime"),
  s_ctime("ctime"),
  s_blksize("blksize"),
  s_blocks("blocks");

Array stat_impl(struct stat *stat_sb) {
  DictInit ret(26);
  ret.append((int64_t)stat_sb->st_dev);
  ret.append((int64_t)stat_sb->st_ino);
  ret.append((int64_t)stat_sb->st_mode);
  ret.append((int64_t)stat_sb->st_nlink);
  ret.append((int64_t)stat_sb->st_uid);
  ret.append((int64_t)stat_sb->st_gid);
  ret.append((int64_t)stat_sb->st_rdev);
  ret.append((int64_t)stat_sb->st_size);
  ret.append((int64_t)stat_sb->st_atime);
  ret.append((int64_t)stat_sb->st_mtime);
  ret.append((int64_t)stat_sb->st_ctime);
  ret.append((int64_t)stat_sb->st_blksize);
  ret.append((int64_t)stat_sb->st_blocks);
  ret.set(s_dev,     (int64_t)stat_sb->st_dev);
  ret.set(s_ino,     (int64_t)stat_sb->st_ino);
  ret.set(s_mode,    (int64_t)stat_sb->st_mode);
  ret.set(s_nlink,   (int64_t)stat_sb->st_nlink);
  ret.set(s_uid,     (int64_t)stat_sb->st_uid);
  ret.set(s_gid,     (int64_t)stat_sb->st_gid);
  ret.set(s_rdev,    (int64_t)stat_sb->st_rdev);
  ret.set(s_size,    (int64_t)stat_sb->st_size);
  ret.set(s_atime,   (int64_t)stat_sb->st_atime);
  ret.set(s_mtime,   (int64_t)stat_sb->st_mtime);
  ret.set(s_ctime,   (int64_t)stat_sb->st_ctime);
  ret.set(s_blksize, (int64_t)stat_sb->st_blksize);
  ret.set(s_blocks,  (int64_t)stat_sb->st_blocks);
  return ret.toArray();
}

///////////////////////////////////////////////////////////////////////////////

Variant HHVM_FUNCTION(fopen,
                      const OptString& filename,
                      const OptString& mode,
                      bool use_include_path /* = false */,
                      const Variant& context /* = null */) {
  CHECK_PATH_FALSE(filename, 1);
  if (!context.isNull() && dyn_cast<StreamContext>(context) == nullptr) {
    raise_warning("$context must be a valid Stream Context or NULL");
    return false;
  }
  if (filename.empty()) {
    raise_warning("Filename cannot be empty");
    return false;
  }

  auto file = File::Open(filename, mode,
                         use_include_path ? File::USE_INCLUDE_PATH : 0,
                         cast_or_null<StreamContext>(context));
  if (!file) {
    return false;
  }
  return Variant(file);
}

Variant HHVM_FUNCTION(popen,
                      const OptString& command,
                      const OptString& mode) {
  if (!Cfg::Server::AllowExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Process execution is disabled by the Server.AllowExec configuration"
    );
  }
  if (!Cfg::Eval::AllowUngatedExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Ungated process execution is disabled by the "
      "Eval.AllowUngatedExec configuration"
    );
  }
  CHECK_PATH_FALSE(command, 1);
  auto file = req::make<Pipe>();
  bool ret = CHECK_ERROR(file->open(File::TranslateCommand(command), mode));
  if (!ret) {
    raise_warning("popen(%s,%s): Invalid argument",
                  command.data(), mode.data());
    return false;
  }
  return Variant(std::move(file));
}

bool HHVM_FUNCTION(fclose, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  return CHECK_ERROR(f->close());
}

Variant HHVM_FUNCTION(pclose,
                      const Variant& handle) {
  if (!Cfg::Server::AllowExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Process execution is disabled by the Server.AllowExec configuration"
    );
  }
  if (!Cfg::Eval::AllowUngatedExec) {
    SystemLib::throwRuntimeExceptionObject(
      "Ungated process execution is disabled by the "
      "Eval.AllowUngatedExec configuration"
    );
  }
  CHECK_HANDLE(handle.toResource(), f);
  int pclose_ret = 0;
  CHECK_ERROR(f->close(&pclose_ret));
  return pclose_ret;
}

Variant HHVM_FUNCTION(fseek,
                      const OptResource& handle,
                      int64_t offset,
                      int64_t whence /* = SEEK_SET */) {
  CHECK_HANDLE(handle, f);
  return CHECK_ERROR(f->seek(offset, whence)) ? 0 : -1;
}

bool HHVM_FUNCTION(rewind, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  return CHECK_ERROR(f->rewind());
}

Variant HHVM_FUNCTION(ftell, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  int64_t ret = f->tell();
  if (!CHECK_ERROR(ret != -1)) {
    return false;
  }
  return ret;
}

bool HHVM_FUNCTION(feof, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  return f->eof();
}

Variant HHVM_FUNCTION(fstat, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  struct stat sb;
  if (!CHECK_ERROR(f->stat(&sb)))
    return false;
  return stat_impl(&sb);
}

Variant HHVM_FUNCTION(fread,
                      const OptResource& handle,
                      int64_t length) {
  CHECK_HANDLE(handle, f);
  if (length < 1) {
    raise_warning(
      "fread(): Length parameter must be greater than 0"
    );
    return false;
  }
  return f->read(length);
}

Variant HHVM_FUNCTION(fgetc, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  int result = f->getc();
  if (result == EOF) {
    return false;
  }
  return OptString::FromChar(result);
}

Variant HHVM_FUNCTION(fgets,
                      const OptResource& handle,
                      int64_t length /* = 0 */) {
  if (length < 0) {
    raise_invalid_argument_warning("length (negative): %" PRId64, length);
    return false;
  }
  CHECK_HANDLE(handle, f);
  OptString line = f->readLine(length - 1);
  if (!line.isNull()) {
    return line;
  }
  return false;
}

Variant HHVM_FUNCTION(fgetss,
                      const OptResource& handle,
                      int64_t length /* = 0 */,
                      const OptString& allowable_tags /* = null_string */) {
  Variant ret = HHVM_FN(fgets)(handle, length);
  if (!same(ret, false)) {
    return StringUtil::StripHTMLTags(ret.toString(), allowable_tags);
  }
  return ret;
}

Variant HHVM_FUNCTION(fscanf,
                      const OptResource& handle,
                      const OptString& format) {
  CHECK_HANDLE(handle, f);
  OptString line = f->readLine();
  if (line.length() == 0) {
    return false;
  }
  return HHVM_FN(sscanf)(line, format);
}

Variant HHVM_FUNCTION(fpassthru, const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  return f->print();
}

Variant HHVM_FUNCTION(fwrite,
                      const OptResource& handle,
                      const OptString& data,
                      int64_t length /* = 0 */) {
  CHECK_HANDLE(handle, f);
  int64_t ret = f->write(data, length);
  if (ret < 0) {
    raise_notice("fwrite(): send of %ld bytes failed with errno=%d %s",
                 data.size(), errno, folly::errnoStr(errno).c_str());
    ret = 0;
  }
  return ret;
}

Variant HHVM_FUNCTION(fputs,
                      const OptResource& handle,
                      const OptString& data,
                      int64_t length /* = 0 */) {
  CHECK_HANDLE(handle, f);
  int64_t ret = f->write(data, length);
  if (ret < 0) ret = 0;
  return ret;
}

Variant HHVM_FUNCTION(fprintf,
                      const Variant& handle,
                      const OptString& format,
                      const Array& args /* = null_array */) {
  if (!handle.isResource()) {
    raise_param_type_warning("fprintf", 1, "resource", *handle.asTypedValue());
    return false;
  }
  const OptResource res = handle.toResource();
  CHECK_HANDLE(res, f);
  return f->printf(format, args);
}

Variant HHVM_FUNCTION(vfprintf,
                      const Variant& handle,
                      const Variant& format,
                      const Variant& args) {
  CHECK_HANDLE(handle.toResource(), f);
  return f->printf(format.toString(), args.toArray());
}

bool HHVM_FUNCTION(fflush,
                   const OptResource& handle) {
  CHECK_HANDLE(handle, f);
  return CHECK_ERROR(f->flush());
}

bool HHVM_FUNCTION(ftruncate,
                   const OptResource& handle,
                   int64_t size) {
  CHECK_HANDLE(handle, f);
  return CHECK_ERROR(f->truncate(size));
}

static int flock_values[] = { LOCK_SH, LOCK_EX, LOCK_UN };

bool HHVM_FUNCTION(flock,
                   const OptResource& handle,
                   int64_t operation,
                   bool& wouldblock) {
  CHECK_HANDLE(handle, f);
  int act;
  wouldblock = false;

  act = operation & 3;
  if (act < 1 || act > 3) {
    raise_invalid_argument_warning("operation: %ld", operation);
    return false;
  }
  act = flock_values[act - 1] | (operation & 4 ? LOCK_NB : 0);
  bool ret = f->lock(act, wouldblock);
  return ret;
}

// match the behavior of PHP5
#define FCSV_CHECK_ARG(NAME)                            \
  if (NAME.size() == 0) {                               \
    raise_invalid_argument_warning(#NAME ": %s", NAME.data());  \
    return false;                                       \
  } else if (NAME.size() > 1) {                         \
    raise_notice(#NAME " must be a single character");  \
  }                                                     \
  char NAME ## _char = NAME.charAt(0);                  \

Variant HHVM_FUNCTION(fputcsv,
                      const OptResource& handle,
                      const Array& fields,
                      const OptString& delimiter /* = "," */,
                      const OptString& enclosure /* = "\"" */,
                      const OptString& escape /* = "\\" */) {
  FCSV_CHECK_ARG(delimiter);
  FCSV_CHECK_ARG(enclosure);
  FCSV_CHECK_ARG(escape);

  CHECK_HANDLE_RET_NULL(handle, f);
  return f->writeCSV(fields, delimiter_char, enclosure_char, escape_char);
}

Variant HHVM_FUNCTION(fgetcsv,
                      const OptResource& handle,
                      int64_t length /* = 0 */,
                      const OptString& delimiter /* = "," */,
                      const OptString& enclosure /* = "\"" */,
                      const OptString& escape /* = "\\" */) {
  if (length < 0) {
    raise_invalid_argument_warning("Length parameter may not be negative");
    return false;
  }

  FCSV_CHECK_ARG(delimiter);
  FCSV_CHECK_ARG(enclosure);
  FCSV_CHECK_ARG(escape);

  CHECK_HANDLE_RET_NULL(handle, f);
  Array ret = f->readCSV(length, delimiter_char, enclosure_char, escape_char);
  if (!ret.isNull()) {
    return ret;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

Variant HHVM_FUNCTION(file_get_contents,
                      const OptString& filename,
                      bool use_include_path /* = false */,
                      const Variant& context /* = null */,
                      int64_t offset /* = -1 */,
                      int64_t maxlen /* = -1 */) {
  CHECK_PATH(filename, 1);
  Variant stream = HHVM_FN(fopen)(filename, "rb", use_include_path, context);
  if (same(stream, false)) return false;
  return HHVM_FN(stream_get_contents)(stream.toResource(), maxlen, offset);
}

Variant HHVM_FUNCTION(file_put_contents,
                      const OptString& filename,
                      const Variant& data,
                      int64_t flags /* = 0 */,
                      const Variant& context /* = null */) {
  CHECK_PATH(filename, 1);

  char mode[3] = "wb";
  if (flags & PHP_FILE_APPEND) {
    mode[0] = 'a';
  } else if (flags & LOCK_EX) {
    // Open in "create" mode (writing only, create if needed, no truncate)
    // so that the file is not modified before we attempt to aquire the
    // requested lock.
    mode[0] = 'c';
  }
  mode[2] = '\0';

  auto file = File::Open(
    filename,
    mode,
    flags,
    dyn_cast_or_null<StreamContext>(context)
  );

  if (!file) {
    return false;
  }

  if (flags & LOCK_EX) {
    // Check to make sure we are dealing with a regular file
    if (!isa<PlainFile>(file)) {
      raise_warning(
        "%s(): Exclusive locks may only be set for regular files",
        __FUNCTION__ + 2);
      return false;
    }

    if (!file->lock(LOCK_EX)) {
      return false;
    }
  }

  if (mode[0] == 'c') {
    file->truncate(0);
  }

  int numbytes = 0;

  switch (data.getType()) {
    case KindOfResource: {
      auto fsrc = dyn_cast_or_null<File>(data);
      if (!fsrc) {
        raise_warning("Not a valid stream resource");
        return false;
      }
      while (true) {
        char buffer[1024];
        int len = fsrc->readImpl(buffer, sizeof(buffer));
        if (len == 0) break;
        numbytes += len;
        int written = file->writeImpl(buffer, len);
        if (written != len) {
          numbytes = -1;
          break;
        }
      }
      break;
    }

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfClsMeth: {
      Array arr = data.toArray();
      for (ArrayIter iter(arr); iter; ++iter) {
        auto const value = iter.second().toString();
        if (!value.empty()) {
          numbytes += value.size();
          int written = file->writeImpl(value.data(), value.size());
          if (written != value.size()) {
            numbytes = -1;
            break;
          }
        }
      }
      break;
    }

    case KindOfObject:
      if (!data.getObjectData()->hasToString()) {
        raise_warning("Not a valid stream resource");
        return false;
      }
      [[fallthrough]];
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass: {
      OptString value = data.toString();
      if (!value.empty()) {
        numbytes += value.size();
        int written = file->writeImpl(value.data(), value.size());
        if (written != value.size()) {
          numbytes = -1;
        }
      }
      break;
    }
    case KindOfRFunc:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      raise_warning("Not a valid stream resource");
      return false;
  }

  // like fwrite(), fclose() can error when fflush()ing
  if (numbytes < 0 || !file->close()) {
    return false;
  }

  return numbytes;
}

Variant HHVM_FUNCTION(file,
                      const OptString& filename,
                      int64_t flags /* = 0 */,
                      const Variant& context /* = null */) {
  CHECK_PATH(filename, 1);
  Variant contents = HHVM_FN(file_get_contents)(filename,
                                         flags & PHP_FILE_USE_INCLUDE_PATH,
                                         context);
  if (same(contents, false)) {
    return false;
  }
  OptString content = contents.toString();
  if (content.empty()) {
    return empty_vec_array();
  }
  auto ret = Array::CreateVec();

  char eol_marker = '\n';
  bool include_new_line = !(flags & PHP_FILE_IGNORE_NEW_LINES);
  bool skip_blank_lines = flags & PHP_FILE_SKIP_EMPTY_LINES;
  const char *s = content.data();
  const char *e = s + content.size();
  const char *p = (const char *)memchr(s, '\n', content.size());
  if (!p) {
    p = e;
    goto parse_eol;
  }

  if (include_new_line) {
    do {
      p++;
    parse_eol:
      ret.append(OptString(s, p-s, CopyString));
      s = p;
    } while ((p = (const char *)memchr(p, eol_marker, (e-p))));
  } else {
    do {
      int windows_eol = 0;
      if (p != content.data() && eol_marker == '\n' && *(p - 1) == '\r') {
        windows_eol++;
      }

      if (skip_blank_lines && !(p-s-windows_eol)) {
        s = ++p;
        continue;
      }
      ret.append(OptString(s, p-s-windows_eol, CopyString));
      s = ++p;
    } while ((p = (const char *)memchr(p, eol_marker, (e-p))));
  }

  /* handle any left overs of files without new lines */
  if (s != e) {
    p = e;
    goto parse_eol;
  }
  return ret;
}

Variant HHVM_FUNCTION(readfile,
                      const OptString& filename,
                      bool use_include_path /* = false */,
                      const Variant& context /* = null */) {
  CHECK_PATH_FALSE(filename, 1);
  Variant f = HHVM_FN(fopen)(filename, "rb", use_include_path, context);
  if (same(f, false)) {
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,
                    folly::errnoStr(errno).c_str());
    return false;
  }
  Variant ret = HHVM_FN(fpassthru)(f.toResource());
  return ret;
}

namespace {

OptString resolve_parse_ini_filename(const OptString& filename) {
  // Try cleaning up the path. Use file_exists to avoid a warning
  // that get_contents generates
  OptString resolved = File::TranslatePath(filename);
  if (!resolved.empty() && HHVM_FN(file_exists)(resolved)) {
    return resolved;
  }

  if (FileUtil::isAbsolutePath(filename.data())) {
    return null_string;
  }

  // Still no go, and not an absolute path, try to resolve based
  // on containing file name.
  auto const cfd = OptString::attach(g_context->getContainingFileName());
  if (!cfd.empty()) {
    int npos = cfd.rfind('/');
    if (npos >= 0) {
      resolved = cfd.substr(0, npos + 1) + filename;
      if (!resolved.empty() && HHVM_FN(file_exists)(resolved)) {
        return resolved;
      }
    }
  }

  // Next, see if include path was set in the ini settings.
  auto const& includePaths = RID().getIncludePaths();

  for (auto const& path : includePaths) {
    resolved = path + FileUtil::getDirSeparator() + filename;
    if (HHVM_FN(file_exists)(resolved)) {
      return resolved;
    }
  }

  return null_string;
}
}

Variant HHVM_FUNCTION(parse_ini_file,
                      const OptString& filename,
                      bool process_sections /* = false */,
                      int64_t scanner_mode /* = k_INI_SCANNER_NORMAL */) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    raise_invalid_argument_warning("Filename cannot be empty!");
    return false;
  }

  // Block ability to load ini files via http (eg., remotely)
  int oFilename = 0;
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename, &oFilename);
  if (nullptr != dynamic_cast<HttpStreamWrapper*>(w)) {
    raise_warning("remote access to ini files is not allowed");
    return false;
  }

  // Extract the (local) filename
  OptString wrapperPrefix = filename.substr(0, oFilename);
  OptString path = resolve_parse_ini_filename(filename.substr(oFilename));

  if (path.empty()) {
    // At this point, we were unable to find the file.
    raise_warning("No such file or directory");
    return false;
  }

  Variant content = HHVM_FN(file_get_contents)(wrapperPrefix + path);
  if (same(content, false)) {
    // Don't generate a warning, as they have already been generated.
    return false;
  }

  return IniSetting::FromString(content.toString(), filename, process_sections,
                                scanner_mode);
}

Variant HHVM_FUNCTION(parse_ini_string,
                      const OptString& ini,
                      bool process_sections /* = false */,
                      int64_t scanner_mode /* = k_INI_SCANNER_NORMAL */) {
  return IniSetting::FromString(ini, "", process_sections, scanner_mode);
}

Variant HHVM_FUNCTION(md5_file,
                      const OptString& filename,
                      bool raw_output /* = false */) {
  CHECK_PATH(filename, 1);
  return HHVM_FN(hash_file)("md5", filename, raw_output);
}

Variant HHVM_FUNCTION(sha1_file,
                      const OptString& filename,
                      bool raw_output /* = false */) {
  CHECK_PATH(filename, 1);
  return HHVM_FN(hash_file)("sha1", filename, raw_output);
}

///////////////////////////////////////////////////////////////////////////////
// stats functions

Variant HHVM_FUNCTION(fileperms,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_mode;
}

Variant HHVM_FUNCTION(fileinode,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb));
  return (int64_t)sb.st_ino;
}

Variant HHVM_FUNCTION(filesize,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  if (filename.empty()) {
    return false;
  }
  if (StaticContentCache::TheFileCache) {
    auto sizeRes = StaticContentCache::TheFileCache->fileSize(filename.data());
    if (sizeRes) return *sizeRes;
  }
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_size;
}

Variant HHVM_FUNCTION(fileowner,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_uid;
}

Variant HHVM_FUNCTION(filegroup,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_gid;
}

Variant HHVM_FUNCTION(fileatime,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_atime;
}

Variant HHVM_FUNCTION(filemtime,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_mtime;
}

Variant HHVM_FUNCTION(filectime,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb, true));
  return (int64_t)sb.st_ctime;
}

Variant HHVM_FUNCTION(filetype,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(lstatSyscall(filename, &sb));

  switch (sb.st_mode & S_IFMT) {
  case S_IFLNK:  return "link";
  case S_IFIFO:  return "fifo";
  case S_IFCHR:  return "char";
  case S_IFDIR:  return "dir";
  case S_IFBLK:  return "block";
  case S_IFREG:  return "file";
  case S_IFSOCK: return "socket";
  }
  return "unknown";
}

Variant HHVM_FUNCTION(linkinfo,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  struct stat sb;
  CHECK_SYSTEM(statSyscall(filename, &sb));
  return (int64_t)sb.st_dev;
}

bool HHVM_FUNCTION(is_writable,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    return false;
  }
  CHECK_SYSTEM_SILENT(accessSyscall(filename, W_OK));
  return true;
  /*
  int mask = S_IWOTH;
  if (sb.st_uid == getuid()) {
    mask = S_IWUSR;
  } else if (sb.st_gid == getgid()) {
    mask = S_IWGRP;
  } else {
    int groups = getgroups(0, NULL);
    if (groups > 0) {
      gid_t *gids = (gid_t *)malloc(groups * sizeof(gid_t));
      int n = getgroups(groups, gids);
      for (int i = 0; i < n; i++) {
        if (sb.st_gid == gids[i]) {
          mask = S_IWGRP;
          break;
        }
      }
      free(gids);
    }
  }
  return sb.st_mode & mask;
  */
}

bool HHVM_FUNCTION(is_writeable,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  return HHVM_FN(is_writable)(filename);
}

bool HHVM_FUNCTION(is_readable,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    return false;
  }
  CHECK_SYSTEM_SILENT(accessSyscall(filename, R_OK, true));
  return true;
  /*
  int mask = S_IROTH;
  if (sb.st_uid == getuid()) {
    mask = S_IRUSR;
  } else if (sb.st_gid == getgid()) {
    mask = S_IRGRP;
  } else {
    int groups = getgroups(0, NULL);
    if (groups > 0) {
      gid_t *gids = (gid_t *)malloc(groups * sizeof(gid_t));
      int n = getgroups(groups, gids);
      for (int i = 0; i < n; i++) {
        if (sb.st_gid == gids[i]) {
          mask = S_IRGRP;
          break;
        }
      }
      free(gids);
    }
  }
  return sb.st_mode & mask;
  */
}

bool HHVM_FUNCTION(is_executable,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    return false;
  }
  CHECK_SYSTEM_SILENT(accessSyscall(filename, X_OK));
  return true;
  /*
  int mask = S_IXOTH;
  if (sb.st_uid == getuid()) {
    mask = S_IXUSR;
  } else if (sb.st_gid == getgid()) {
    mask = S_IXGRP;
  } else {
    int groups = getgroups(0, NULL);
    if (groups > 0) {
      gid_t *gids = (gid_t *)malloc(groups * sizeof(gid_t));
      int n = getgroups(groups, gids);
      for (int i = 0; i < n; i++) {
        if (sb.st_gid == gids[i]) {
          mask = S_IXGRP;
          break;
        }
      }
      free(gids);
    }
  }
  return (sb.st_mode & mask) && (sb.st_mode & S_IFMT) != S_IFDIR;
  */
}

static VirtualFileSystem::FileType lookupVirtualFile(const OptString& filename) {
  if (filename.empty() || !StaticContentCache::TheFileCache) {
    return VirtualFileSystem::FileType::NOT_FOUND;
  }

  OptString cwd;
  std::string root;
  bool isRelative = !FileUtil::isAbsolutePath(filename.slice());
  if (isRelative) {
    cwd = g_context->getCwd();
    root = Cfg::Server::SourceRoot;
    if (cwd.empty() || FileUtil::isDirSeparator(cwd[cwd.size() - 1])) {
      root.pop_back();
    }
  }

  if (!isRelative || !root.compare(cwd.data())) {
    if (StaticContentCache::TheFileCache->exists(filename.toCppString())) {
      if (StaticContentCache::TheFileCache->dirExists(filename.toCppString())) {
        return VirtualFileSystem::FileType::DIRECTORY;
      }
      return VirtualFileSystem::FileType::REGULAR_FILE;
    }
  }

  return VirtualFileSystem::FileType::NOT_FOUND;
}

bool HHVM_FUNCTION(is_file,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    return false;
  }
  auto vtype = lookupVirtualFile(filename);
  if (vtype != VirtualFileSystem::FileType::NOT_FOUND) {
    return vtype == VirtualFileSystem::FileType::REGULAR_FILE;
  }

  struct stat sb;
  CHECK_SYSTEM_SILENT(statSyscall(filename, &sb, true));
  return (sb.st_mode & S_IFMT) == S_IFREG;
}

bool HHVM_FUNCTION(is_dir,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  if (filename.empty()) {
    return false;
  }
  auto vtype = lookupVirtualFile(filename);
  if (vtype != VirtualFileSystem::FileType::NOT_FOUND) {
    return vtype == VirtualFileSystem::FileType::DIRECTORY;
  }

  struct stat sb;
  CHECK_SYSTEM_SILENT(statSyscall(filename, &sb, false));
  return (sb.st_mode & S_IFMT) == S_IFDIR;
}

bool HHVM_FUNCTION(is_link,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  struct stat sb;
  CHECK_SYSTEM_SILENT(lstatSyscall(filename, &sb));
  return (sb.st_mode & S_IFMT) == S_IFLNK;
}

bool HHVM_FUNCTION(is_uploaded_file,
                   const OptString& filename) {
  Transport *transport = g_context->getTransport();
  if (transport) {
    return transport->isUploadedFile(filename);
  }
  return false;
}

bool HHVM_FUNCTION(file_exists,
                   const OptString& filename) {
  CHECK_PATH_FALSE(filename, 1);
  auto vtype = lookupVirtualFile(filename);
  if (vtype != VirtualFileSystem::FileType::NOT_FOUND) {
    return true;
  }

  if (filename.empty() ||
      (accessSyscall(filename, F_OK, true)) < 0) {
    return false;
  }
  return true;
}

Variant HHVM_FUNCTION(stat,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  if (filename.empty()) {
    return false;
  }

  struct stat sb;
  if (statSyscall(filename, &sb, true) != 0) {
    raise_warning(
      "stat(): stat failed for %s",
       filename.c_str()
    );
    return false;
  }
  return stat_impl(&sb);
}

Variant HHVM_FUNCTION(lstat,
                      const OptString& filename) {
  CHECK_PATH(filename, 1);
  if (filename.empty()) {
    return false;
  }

  struct stat sb;
  CHECK_SYSTEM(lstatSyscall(filename, &sb, true));
  return stat_impl(&sb);
}

void HHVM_FUNCTION(clearstatcache, bool /*clear_realpath_cache*/ /* = false */,
                   const Variant& /*filename*/ /* = uninit_variant */) {
  // we are not having a cache for file stats, so do nothing here
}

Variant HHVM_FUNCTION(readlink,
                      const OptString& path) {
  CHECK_PATH(path, 1);

  char buff[PATH_MAX];
  int ret = readlink(File::TranslatePath(path).data(), buff, PATH_MAX-1);
  if (ret < 0) {
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,
                    folly::errnoStr(errno).c_str());
    raise_warning("readlink(): No such file or directory %s",path.c_str());
    return false;
  }
  buff[ret] = '\0';
  return OptString(buff, ret, CopyString);
}

Variant HHVM_FUNCTION(realpath,
                      const OptString& path) {
  CHECK_PATH(path, 1);

  OptString translated;
  if (path.empty()) {
    translated = File::TranslatePath(g_context->getCwd());
  } else {
    translated = File::TranslatePath(path);
  }

  if (translated.empty()) {
    return false;
  }
  if (StaticContentCache::TheFileCache &&
      StaticContentCache::TheFileCache->exists(translated.data())) {
    return translated;
  }
  // Zend doesn't support streams in realpath
  Stream::Wrapper* w = Stream::getWrapperFromURI(path);
  if (!w || !w->isNormalFileStream()) {
    return false;
  }
  if (!dynamic_cast<FileStreamWrapper*>(w)) {
    auto str = w->realpath(translated);
    if (str.isNull()) return false;
    return str;
  }
  char resolved_path[PATH_MAX];
  if (!realpath(translated.c_str(), resolved_path)) {
    return false;
  }
  return OptString(resolved_path, CopyString);
}

#define PHP_PATHINFO_DIRNAME    1
#define PHP_PATHINFO_BASENAME   2
#define PHP_PATHINFO_EXTENSION  4
#define PHP_PATHINFO_FILENAME   8

const StaticString
  s_dirname("dirname"),
  s_basename("basename"),
  s_extension("extension"),
  s_filename("filename");

Variant HHVM_FUNCTION(pathinfo,
                      const OptString& path,
                      int64_t opt /* = 15 */) {
  DictInit ret{4};

  if (opt == 0) {
    return empty_string_variant();
  }

  if ((opt & PHP_PATHINFO_DIRNAME) == PHP_PATHINFO_DIRNAME) {
    OptString dirname = HHVM_FN(dirname)(path);
    if (opt == PHP_PATHINFO_DIRNAME) {
      return dirname;
    }
    if (!dirname.equal(staticEmptyString())) {
      ret.set(s_dirname, dirname);
    }
  }

  OptString basename = HHVM_FN(basename)(path);
  if ((opt & PHP_PATHINFO_BASENAME) == PHP_PATHINFO_BASENAME) {
    if (opt == PHP_PATHINFO_BASENAME) {
      return basename;
    }
    ret.set(s_basename, basename);
  }

  if ((opt & PHP_PATHINFO_EXTENSION) == PHP_PATHINFO_EXTENSION) {
    int pos = basename.rfind('.');
    OptString extension(String::Empty());
    if (pos >= 0) {
      extension = basename.substr(pos + 1);
      ret.set(s_extension, extension);
    }
    if (opt == PHP_PATHINFO_EXTENSION) {
      return extension;
    }
  }

  if ((opt & PHP_PATHINFO_FILENAME) == PHP_PATHINFO_FILENAME) {
    int pos = basename.rfind('.');
    OptString filename(String::Empty());
    if (pos >= 0) {
      filename = basename.substr(0, pos);
    } else {
      filename = basename;
    }
    if (opt == PHP_PATHINFO_FILENAME) {
      return filename;
    }
    ret.set(s_filename, filename);
  }

  return ret.toVariant();
}

Variant HHVM_FUNCTION(disk_free_space,
                      const OptString& directory) {
  CHECK_PATH(directory, 1);
  fs::space_info sb;
  OptString translated = File::TranslatePath(directory);
  CHECK_FS(sb, fs::space, translated);
  return (double)sb.free;
}

Variant HHVM_FUNCTION(diskfreespace,
                      const OptString& directory) {
  CHECK_PATH(directory, 1);
  return HHVM_FN(disk_free_space)(directory);
}

Variant HHVM_FUNCTION(disk_total_space,
                      const OptString& directory) {
  CHECK_PATH(directory, 1);
  fs::space_info sb;
  OptString translated = File::TranslatePath(directory);
  CHECK_FS(sb, fs::space, translated);
  return (double)sb.capacity;
}

///////////////////////////////////////////////////////////////////////////////
// system wrappers

bool HHVM_FUNCTION(chmod,
                   const OptString& filename,
                   int64_t mode) {
  CHECK_PATH_FALSE(filename, 1);
  OptString translated = File::TranslatePath(filename);

  // If filename points to a user file, invoke ExtendedWrapper::chmod(..)
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename);
  auto usw = dynamic_cast<Stream::ExtendedWrapper*>(w);
  if (usw != nullptr) {
    return usw->chmod(filename, mode);
  }

  CHECK_SYSTEM(chmod(translated.c_str(), mode));
  return true;
}

static bool do_chown(const OptString& filename,
                     const Variant& user,
                     bool islChown,
                     const char* funcName) {
  // If filename points to a user file, invoke ExtendedWrapper::chown(..)
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename);
  auto usw = dynamic_cast<Stream::ExtendedWrapper*>(w);
  if (usw != nullptr) {
    if (user.isInteger()) {
      return usw->chown(filename, user.toInt64());
    } else if (user.isString()) {
      return usw->chown(filename, user.toString());
    }
    raise_warning("%s(): parameter 2 should be string or integer, %s given",
      funcName, getDataTypeString(user.getType()).c_str());
    return false;
  }

  int uid;
  if (user.isString()) {
    OptString suser = user.toString();
    auto buf = PasswdBuffer{};
    struct passwd *pw = nullptr;
    int err;
    // getpwnam_r returns the error number directly; grow the buffer on ERANGE.
    while ((err = getpwnam_r(suser.data(), &buf.ent, buf.data.get(),
                             buf.size, &pw)) == ERANGE) {
      buf.resize();
    }
    // A non-zero return or a null result both mean the user could not be
    // resolved; match PHP semantics by emitting a warning in either case.
    if (err != 0 || !pw) {
      Logger::Verbose("%s/%d: Unable to find uid for %s",
        __FUNCTION__, __LINE__, suser.data());
      raise_warning("%s(): Unable to find uid for %s", funcName,
        user.toString().c_str());
      return false;
    }
    uid = pw->pw_uid;
  } else {
    uid = (int)user.toInt64();
  }

  if (islChown) {
    CHECK_SYSTEM(lchown(File::TranslatePath(filename).data(), uid, (gid_t)-1));
  } else {
    CHECK_SYSTEM(chown(File::TranslatePath(filename).data(), uid, (gid_t)-1));
  }
  return true;
}

bool HHVM_FUNCTION(chown,
                   const OptString& filename,
                   const Variant& user) {
  CHECK_PATH_FALSE(filename, 1);
  return do_chown(filename, user, false, "chown");
}

bool HHVM_FUNCTION(lchown,
                   const OptString& filename,
                   const Variant& user) {
  CHECK_PATH_FALSE(filename, 1);
  return do_chown(filename, user, true, "lchown");
}

static bool do_chgrp(const OptString& filename,
                     const Variant& group,
                     bool islChgrp,
                     const char* funcName) {
  // If filename points to a user file, invoke ExtendedWrapper::chgrp(..)
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename);
  auto usw = dynamic_cast<Stream::ExtendedWrapper*>(w);
  if (usw != nullptr) {
    if (group.isInteger()) {
      return usw->chgrp(filename, group.toInt64());
    } else if (group.isString()) {
      return usw->chgrp(filename, group.toString());
    }
    raise_warning("%s(): parameter 2 should be string or integer, %s given",
      funcName, getDataTypeString(group.getType()).c_str());
    return false;
  }

  int gid;
  if (group.isString()) {
    OptString sgroup = group.toString();
    auto buf = GroupBuffer{};
    struct group *gr = nullptr;
    int err;
    // getgrnam_r returns the error number directly (it does not set errno);
    // grow the buffer on ERANGE.
    while ((err = getgrnam_r(sgroup.data(), &buf.ent, buf.data.get(),
                             buf.size, &gr)) == ERANGE) {
      buf.resize();
    }
    // A non-zero return or a null result both mean the group could not be
    // resolved; match PHP semantics by emitting a warning in either case.
    if (err != 0 || !gr) {
      Logger::Verbose("%s/%d: Unable to find gid for %s",
        __FUNCTION__, __LINE__, sgroup.data());
      raise_warning("%s(): Unable to find gid for %s", funcName,
        group.toString().c_str());
      return false;
    }
    gid = gr->gr_gid;
  } else {
    gid = (int)group.toInt64();
  }

  if (islChgrp) {
    CHECK_SYSTEM(lchown(File::TranslatePath(filename).data(), (uid_t)-1, gid));
  } else {
    CHECK_SYSTEM(chown(File::TranslatePath(filename).data(), (uid_t)-1, gid));
  }
  return true;
}

bool HHVM_FUNCTION(chgrp,
                   const OptString& filename,
                   const Variant& group) {
  CHECK_PATH_FALSE(filename, 1);
  return do_chgrp(filename, group, false, "chgrp");
}

bool HHVM_FUNCTION(lchgrp,
                   const OptString& filename,
                   const Variant& group) {
  CHECK_PATH_FALSE(filename, 1);
  return do_chgrp(filename, group, true, "lchgrp");
}

bool HHVM_FUNCTION(touch,
                   const OptString& filename,
                   int64_t mtime /* = 0 */,
                   int64_t atime /* = 0 */) {
  CHECK_PATH_FALSE(filename, 1);

  if (filename.empty()) {
    return false;
  }

  // If filename points to a user file, invoke ExtendedWrapper::touch(..)
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename);
  auto usw = dynamic_cast<Stream::ExtendedWrapper*>(w);
  if (usw != nullptr) {
    return usw->touch(filename, mtime, atime);
  }

  OptString translated = File::TranslatePath(filename);

  /* create the file if it doesn't exist already */
  if (accessSyscall(translated, F_OK)) {
    FILE *f = fopen(translated.data(), "w");
    if (!f) {
      raise_warning(
        "touch(): Unable to create file %s because %s",
        translated.data(), folly::errnoStr(errno).c_str()
      );
      return false;
    }
    fclose(f);
  }

  if (mtime == 0 && atime == 0) {
    // It is important to pass nullptr so that the OS sets mtime and atime
    // to the current time with maximum precision (more precise then seconds)
    CHECK_SYSTEM(utime(translated.data(), nullptr));
  } else {
    struct utimbuf newtime;
    newtime.actime = atime ? atime : mtime;
    newtime.modtime = mtime;
    CHECK_SYSTEM(utime(translated.data(), &newtime));
  }

  return true;
}

bool HHVM_FUNCTION(copy,
                   const OptString& source,
                   const OptString& dest,
                   const Variant& context /* = null */) {
  CHECK_PATH_FALSE(source, 1);
  CHECK_PATH_FALSE(dest, 2);
  if (!context.isNull() || !File::IsPlainFilePath(source) ||
      !File::IsPlainFilePath(dest) || is_cli_server_mode()) {
    Variant sfile = HHVM_FN(fopen)(source, "r", false, context);
    if (same(sfile, false)) {
      return false;
    }
    Variant dfile = HHVM_FN(fopen)(dest, "w", false, context);
    if (same(dfile, false)) {
      return false;
    }

    if (!HHVM_FN(stream_copy_to_stream)(sfile.toResource(),
                                        dfile.toResource()).toBoolean() &&
        HHVM_FN(filesize)(source).toBoolean()) {
      return false;
    }

    return HHVM_FN(fclose)(dfile.toResource());
  } else {
    int ret =
      Cfg::Server::UseDirectCopy ?
      FileUtil::directCopy(File::TranslatePath(source).data(),
          File::TranslatePath(dest).data())
      :
      FileUtil::copy(File::TranslatePath(source).data(),
          File::TranslatePath(dest).data());
    return (ret == 0);
  }
}

bool HHVM_FUNCTION(rename, const OptString& oldname, const OptString& newname,
                   const Variant& /*context*/ /* = null */) {
  CHECK_PATH_FALSE(oldname, 1);
  CHECK_PATH_FALSE(newname, 2);
  Stream::Wrapper* w = Stream::getWrapperFromURI(oldname);
  if (!w) return false;
  if (w != Stream::getWrapperFromURI(newname)) {
    raise_warning("Can't rename a file on different streams");
    return false;
  }
  CHECK_SYSTEM(w->rename(oldname, newname));
  return true;
}

int64_t HHVM_FUNCTION(umask,
                      const Variant& mask /* = uninit_variant */) {
  int oldumask = umask(077);
  if (mask.isNull()) {
    umask(oldumask);
  } else {
    umask((int)mask.toInt64());
  }
  return oldumask;
}

bool HHVM_FUNCTION(unlink, const OptString& filename,
                   const Variant& /*context*/ /* = null */) {
  CHECK_PATH_FALSE(filename, 1);
  Stream::Wrapper* w = Stream::getWrapperFromURI(filename);
  if (!w) return false;
  CHECK_SYSTEM_SILENT(w->unlink(filename));
  return true;
}

bool HHVM_FUNCTION(move_uploaded_file,
                   const OptString& filename,
                   const OptString& destination) {
  Transport *transport = g_context->getTransport();
  if (!transport || !transport->isUploadedFile(filename)) {
    return false;
  }

  CHECK_PATH_FALSE(destination, 2);

  if (HHVM_FN(rename)(filename, destination, uninit_null())) {
    return true;
  }

  // If rename didn't work, fall back to copy followed by unlink
  if (!HHVM_FN(copy)(filename, destination, uninit_null())) {
    return false;
  }
  HHVM_FN(unlink)(filename, uninit_null());

  return true;
}

bool HHVM_FUNCTION(link,
                   const OptString& target,
                   const OptString& link) {
  CHECK_PATH_FALSE(target, 1);
  CHECK_PATH_FALSE(link, 2);
  CHECK_FS_ASSIGN(fs::create_hard_link, File::TranslatePath(target),
                  File::TranslatePath(link).toCppString());
  return true;
}

bool HHVM_FUNCTION(symlink,
                   const OptString& target,
                   const OptString& link) {
  CHECK_PATH_FALSE(target, 1);
  CHECK_PATH_FALSE(link, 2);
  if (HHVM_FN(is_dir)(target)) {
    CHECK_FS_ASSIGN(fs::create_directory_symlink,
                    File::TranslatePathKeepRelative(target),
                       File::TranslatePath(link).toCppString());
  } else {
    CHECK_FS_ASSIGN(fs::create_symlink,
                    File::TranslatePathKeepRelative(target),
                       File::TranslatePath(link).toCppString());
  }
  return true;
}

OptString HHVM_FUNCTION(basename,
                        const OptString& path,
                        const OptString& suffix /* = null_string */) {
  int state = 0;
  const char *c = path.data();
  const char *comp, *cend;
  comp = cend = c;
  for (int cnt = path.size(); cnt > 0; --cnt, ++c) {
    if (FileUtil::isDirSeparator(*c)) {
      if (state == 1) {
        state = 0;
        cend = c;
      }
    } else if (state == 0) {
      comp = c;
      state = 1;
    }
  }

  if (state == 1) {
    cend = c;
  }
  int sufflen = suffix.size();
  if (!suffix.empty() && sufflen < (int)(cend - comp) &&
      memcmp(cend - sufflen, suffix.data(), sufflen) == 0) {
    cend -= sufflen;
  }
  return OptString(comp, cend - comp, CopyString);
}

bool HHVM_FUNCTION(fnmatch,
                   const OptString& pattern,
                   const OptString& filename, int64_t flags /* = 0 */) {
  CHECK_PATH_FALSE(pattern, 1);
  CHECK_PATH_FALSE(filename, 2);
  if (filename.size() >= PATH_MAX) {
    raise_warning(
      "Filename exceeds the maximum allowed length of %d characters",
      PATH_MAX);
    return false;
  }
  if (pattern.size() >= PATH_MAX) {
    raise_warning("Path exceeds the maximum allowed length of %d characters",
                  PATH_MAX);
    return false;
  }

  return fnmatch(pattern.data(), filename.data(), flags) == 0;
}

Variant HHVM_FUNCTION(glob,
                      const OptString& pattern,
                      int64_t flags /* = 0 */) {
  CHECK_PATH(pattern, 1);
  glob_t globbuf;
  int cwd_skip = 0;
  memset(&globbuf, 0, sizeof(glob_t));
  globbuf.gl_offs = 0;
  OptString work_pattern;

  if (pattern.size() >= PATH_MAX) {
    raise_warning("Pattern exceeds the maximum allowed length of %d characters",
                  PATH_MAX);
    return false;
  }

  if (pattern.charAt(0) == '/') {
    work_pattern = pattern;
  } else {
    OptString cwd = g_context->getCwd();
    if (!cwd.empty() && cwd[cwd.length() - 1] == '/') {
      work_pattern = cwd + pattern;
      cwd_skip = cwd.length();
    } else {
      work_pattern = cwd + "/" + pattern;
      cwd_skip = cwd.length() + 1;
    }
  }
  int nret = glob(work_pattern.data(),
                  flags & PHP_GLOB_FLAGMASK,
                  nullptr,
                  &globbuf);
  if (nret == GLOB_NOMATCH) {
    globfree(&globbuf);
    return empty_vec_array();
  }

  if (!globbuf.gl_pathc || !globbuf.gl_pathv) {
    if (RID().hasSafeFileAccess()) {
      if (!HHVM_FN(is_dir)(work_pattern)) {
        globfree(&globbuf);
        return false;
      }
    }
    globfree(&globbuf);
    return empty_vec_array();
  }

  if (nret) {
    globfree(&globbuf);
    return false;
  }

  auto ret = Array::CreateVec();
  bool basedir_limit = false;
  for (int n = 0; n < (int)globbuf.gl_pathc; n++) {
    OptString translated = File::TranslatePath(globbuf.gl_pathv[n]);
    if (translated.empty()) {
      basedir_limit = true;
      continue;
    }
    /* we need to do this every time since GLOB_ONLYDIR does not guarantee that
     * all directories will be filtered. GNU libc documentation states the
     * following:
     * If the information about the type of the file is easily available
     * non-directories will be rejected but no extra work will be done to
     * determine the information for each file. I.e., the caller must still be
     * able to filter directories out.
     */
    if ((flags & GLOB_ONLYDIR) && !HHVM_FN(is_dir)(globbuf.gl_pathv[n])) {
      continue;
    }
    ret.append(OptString(globbuf.gl_pathv[n] + cwd_skip, CopyString));
  }

  globfree(&globbuf);

  if (basedir_limit && ret.empty()) {
    return false;
  }
  // php's glob always produces an array, but Variant::Variant(CArrRef)
  // will produce KindOfNull if given a req::ptr wrapped around null.
  if (ret.isNull()) {
    return empty_vec_array();
  }
  return ret;
}

Variant HHVM_FUNCTION(tempnam,
                      const OptString& dir,
                      const OptString& prefix) {
  CHECK_PATH(dir, 1);
  CHECK_PATH(prefix, 2);
  OptString tmpdir = dir, trailing_slash = "/";
  if (tmpdir.empty() || !HHVM_FN(is_dir)(tmpdir) ||
      !HHVM_FN(is_writable)(tmpdir)) {
    tmpdir = HHVM_FN(sys_get_temp_dir)();
  }
  tmpdir = File::TranslatePath(tmpdir);
  OptString pbase = HHVM_FN(basename)(prefix);
  if (pbase.size() > 64) pbase = pbase.substr(0, 63);
  if ((tmpdir.length() > 0) && (tmpdir[tmpdir.length() - 1] == '/')) {
    trailing_slash = "";
  }
  OptString templ = tmpdir + trailing_slash + pbase + "XXXXXX";
  auto buf = templ.get()->mutableData();
  if (UNLIKELY(is_cli_server_mode())) {
    if (!cli_mkstemp(buf)) return false;
  } else {
    int fd = mkstemp(buf);
    if (fd < 0) {
      Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,
                      folly::errnoStr(errno).c_str());
      return false;
    }

    close(fd);
  }
  return templ.setSize(strlen(buf));
}

Variant HHVM_FUNCTION(tmpfile) {
  auto file = req::make<TempFile>(true);
  if (!file->valid()) {
    return false;
  }
  return Variant(file);
}

///////////////////////////////////////////////////////////////////////////////
// directory functions

bool HHVM_FUNCTION(mkdir, const OptString& pathname, int64_t mode /* = 0777 */,
                   bool recursive /* = false */,
                   const Variant& /*context*/ /* = null */) {
  CHECK_PATH_FALSE(pathname, 1);
  Stream::Wrapper* w = Stream::getWrapperFromURI(pathname);
  if (!w) return false;
  int options = recursive ? k_STREAM_MKDIR_RECURSIVE : 0;
  CHECK_SYSTEM_SILENT(w->mkdir(pathname, mode, options));
  return true;
}

bool HHVM_FUNCTION(rmdir, const OptString& dirname,
                   const Variant& /*context*/ /* = null */) {
  CHECK_PATH_FALSE(dirname, 1);
  Stream::Wrapper* w = Stream::getWrapperFromURI(dirname);
  if (!w) return false;
  int options = 0;
  CHECK_SYSTEM_SILENT(w->rmdir(dirname, options));
  return true;
}

OptString HHVM_FUNCTION(dirname,
                        const OptString& path) {
  return FileUtil::dirname(path);
}

Variant HHVM_FUNCTION(getcwd) {
  return g_context->getCwd();
}

bool HHVM_FUNCTION(chdir,
                   const OptString& directory) {
  CHECK_PATH_FALSE(directory, 1);
  if (!HHVM_FN(is_dir)(directory)) {
    raise_warning("chdir(): No such file or directory (errno 2)");
    return false;
  }

#if ENABLE_EXTENSION_POSIX
  if (!HHVM_FN(posix_access)(directory, X_OK)) {
    raise_warning("chdir(): Permission denied (errno 13)");
    return false;
  }
#endif

  g_context->setCwd(HHVM_FN(realpath)(directory).toString());
  return true;
}

bool HHVM_FUNCTION(chroot,
                   const OptString& directory) {
  CHECK_PATH_FALSE(directory, 1);
  CHECK_SYSTEM(chroot(File::TranslatePath(directory).data()));
  CHECK_SYSTEM(chdir("/"));
  return true;
}

///////////////////////////////////////////////////////////////////////////////

/**
 * A stack maintains the states of nested structures.
 */

struct DirectoryData final : RequestEventHandler {
  void requestInit() override {
    assertx(!defaultDirectory);
  }
  void requestShutdown() override {
    defaultDirectory = nullptr;
  }
  req::ptr<Directory> defaultDirectory;
};

namespace {

IMPLEMENT_STATIC_REQUEST_LOCAL(DirectoryData, s_directory_data);

const StaticString
  s_handle("handle"),
  s_path("path");

req::ptr<Directory> get_dir(const OptResource& dir_handle) {
  if (dir_handle.isNull()) {
    auto defaultDir = s_directory_data->defaultDirectory;
    if (!defaultDir) {
      raise_warning("no Directory resource supplied");
      return nullptr;
    }
    return defaultDir;
  }

  auto d = dyn_cast_or_null<Directory>(dir_handle);
  if (!d) {
    raise_warning("Not a valid directory resource");
    return nullptr;
  }
  return d;
}

bool StringDescending(const OptString& s1, const OptString& s2) {
  return s1.more(s2);
}

bool StringAscending(const OptString& s1, const OptString& s2) {
  return s1.less(s2);
}

}

Variant HHVM_FUNCTION(opendir, const OptString& path,
                      const Variant& /*context*/ /* = null */) {
  CHECK_PATH(path, 1);
  Stream::Wrapper* w = Stream::getWrapperFromURI(path);
  if (!w) return false;
  auto p = w->opendir(path);
  if (!p) {
    return false;
  }
  s_directory_data->defaultDirectory = p;
  return Variant(p);
}

Variant HHVM_FUNCTION(dir,
                      const OptString& directory) {
  CHECK_PATH(directory, 1);
  Variant dir = HHVM_FN(opendir)(directory, uninit_null());
  if (same(dir, false)) {
    return false;
  }
  auto d = SystemLib::AllocDirectoryObject();
  // public properties
  d->setProp(nullctx, s_path.get(), directory.asTypedValue());
  d->setProp(nullctx, s_handle.get(), *dir.asTypedValue());
  return d;
}

Variant HHVM_FUNCTION(readdir,
                      const Variant& dir_handle /* = null */) {
  const OptResource& res_dir_handle = dir_handle.isNull()
                                 ? null_resource
                                 : dir_handle.toResource();
  auto dir = get_dir(res_dir_handle);
  if (!dir) {
    return false;
  }
  return dir->read();
}

void HHVM_FUNCTION(rewinddir,
                   const Variant& dir_handle /* = null */) {
  const OptResource& res_dir_handle = dir_handle.isNull()
                                 ? null_resource
                                 : dir_handle.toResource();
  auto dir = get_dir(res_dir_handle);
  if (!dir) {
    return;
  }
  dir->rewind();
}

Variant
HHVM_FUNCTION(scandir, const OptString& directory, bool descending /* = false */,
              const Variant& /*context*/ /* = null */) {
  if (directory.empty()) {
    raise_warning("scandir(): Directory name cannot be empty");
    return false;
  }

  CHECK_PATH(directory, 1);
  Stream::Wrapper* w = Stream::getWrapperFromURI(directory);
  if (!w) return false;
  auto dir = w->opendir(directory);
  if (!dir) {
    return false;
  }

  std::vector<OptString> names;
  while (true) {
    auto name = dir->read();
    if (same(name, false)) {
      break;
    }
    names.push_back(name.toString());
  }
  dir->close();

  sort(
    names.begin(),
    names.end(),
    descending ? StringDescending : StringAscending
  );

  VecInit ret{names.size()};
  for (auto& name : names) {
    ret.append(name);
  }
  return ret.toVariant();
}

void HHVM_FUNCTION(closedir,
                   const Variant& dir_handle /* = null */) {
  const OptResource& res_dir_handle = dir_handle.isNull()
                                 ? null_resource
                                 : dir_handle.toResource();
  auto d = get_dir(res_dir_handle);
  if (!d) {
    return;
  }
  if (s_directory_data->defaultDirectory == d) {
    s_directory_data->defaultDirectory = nullptr;
  }
  d->close();
}

namespace {
template<typename T>
Variant try_stdio(T f) {
  return is_any_cli_mode() ? f().asCResRef() : OptResource();
}

Variant HHVM_FUNCTION(try_stdin) {
  return try_stdio([] { return BuiltinFiles::getSTDIN(); });
}

Variant HHVM_FUNCTION(try_stdout) {
  return try_stdio([] { return BuiltinFiles::getSTDOUT(); });
}

Variant HHVM_FUNCTION(try_stderr) {
  return try_stdio([] { return BuiltinFiles::getSTDERR(); });
}
}

///////////////////////////////////////////////////////////////////////////////

void StandardExtension::registerNativeFile() {
  HHVM_RC_STR(DIRECTORY_SEPARATOR, s_DIRECTORY_SEPARATOR);
  HHVM_RC_STR(PATH_SEPARATOR, s_PATH_SEPARATOR);

  HHVM_RC_INT(FILE_USE_INCLUDE_PATH, PHP_FILE_USE_INCLUDE_PATH);
  HHVM_RC_INT(FILE_IGNORE_NEW_LINES, PHP_FILE_IGNORE_NEW_LINES);
  HHVM_RC_INT(FILE_SKIP_EMPTY_LINES, PHP_FILE_SKIP_EMPTY_LINES);
  HHVM_RC_INT(FILE_APPEND, PHP_FILE_APPEND);
  HHVM_RC_INT(FILE_NO_DEFAULT_CONTEXT, PHP_FILE_NO_DEFAULT_CONTEXT);
  HHVM_RC_INT_SAME(FNM_NOESCAPE);
  HHVM_RC_INT_SAME(FNM_CASEFOLD);
  HHVM_RC_INT_SAME(FNM_PERIOD);
  HHVM_RC_INT_SAME(FNM_PATHNAME);
  HHVM_RC_INT(GLOB_AVAILABLE_FLAGS, PHP_GLOB_FLAGS);
  HHVM_RC_INT_SAME(GLOB_BRACE);
  HHVM_RC_INT_SAME(GLOB_ERR);
  HHVM_RC_INT_SAME(GLOB_MARK);
  HHVM_RC_INT_SAME(GLOB_NOCHECK);
  HHVM_RC_INT_SAME(GLOB_NOESCAPE);
  HHVM_RC_INT_SAME(GLOB_NOSORT);
  HHVM_RC_INT_SAME(GLOB_ONLYDIR);
  HHVM_RC_INT(LOCK_SH, k_LOCK_SH);
  HHVM_RC_INT(LOCK_EX, k_LOCK_EX);
  HHVM_RC_INT(LOCK_UN, k_LOCK_UN);
  HHVM_RC_INT(LOCK_NB, k_LOCK_NB);

  HHVM_RC_INT(SCANDIR_SORT_ASCENDING, k_SCANDIR_SORT_ASCENDING);
  HHVM_RC_INT(SCANDIR_SORT_DESCENDING, k_SCANDIR_SORT_DESCENDING);
  HHVM_RC_INT(SCANDIR_SORT_NONE, k_SCANDIR_SORT_NONE);
  HHVM_RC_INT_SAME(SEEK_SET);
  HHVM_RC_INT_SAME(SEEK_CUR);
  HHVM_RC_INT_SAME(SEEK_END);

  HHVM_FALIAS(HH\\try_stdin, try_stdin);
  HHVM_FALIAS(HH\\try_stdout, try_stdout);
  HHVM_FALIAS(HH\\try_stderr, try_stderr);

  HHVM_RC_INT(INI_SCANNER_NORMAL, k_INI_SCANNER_NORMAL);
  HHVM_RC_INT(INI_SCANNER_RAW,    k_INI_SCANNER_RAW);

  HHVM_RC_INT(PATHINFO_BASENAME,  PHP_PATHINFO_BASENAME);
  HHVM_RC_INT(PATHINFO_DIRNAME,   PHP_PATHINFO_DIRNAME);
  HHVM_RC_INT(PATHINFO_EXTENSION, PHP_PATHINFO_EXTENSION);
  HHVM_RC_INT(PATHINFO_FILENAME,  PHP_PATHINFO_FILENAME);

  HHVM_FE(fopen);
  HHVM_FE(popen);
  HHVM_FE(fclose);
  HHVM_FE(pclose);
  HHVM_FE(fseek);
  HHVM_FE(rewind);
  HHVM_FE(ftell);
  HHVM_FE(feof);
  HHVM_FE(fstat);
  HHVM_FE(fread);
  HHVM_FE(fgetc);
  HHVM_FE(fgets);
  HHVM_FE(fgetss);
  HHVM_FE(fscanf);
  HHVM_FE(fpassthru);
  HHVM_FE(fwrite);
  HHVM_FE(fputs);
  HHVM_FE(fprintf);
  HHVM_FE(vfprintf);
  HHVM_FE(fflush);
  HHVM_FE(ftruncate);
  HHVM_FE(flock);
  HHVM_FE(fputcsv);
  HHVM_FE(fgetcsv);
  HHVM_FE(file_get_contents);
  HHVM_FE(file_put_contents);
  HHVM_FE(file);
  HHVM_FE(readfile);
  HHVM_FE(move_uploaded_file);
  HHVM_FE(parse_ini_file);
  HHVM_FE(parse_ini_string);
  HHVM_FE(md5_file);
  HHVM_FE(sha1_file);
  HHVM_FE(chmod);
  HHVM_FE(chown);
  HHVM_FE(lchown);
  HHVM_FE(chgrp);
  HHVM_FE(lchgrp);
  HHVM_FE(touch);
  HHVM_FE(copy);
  HHVM_FE(rename);
  HHVM_FE(umask);
  HHVM_FE(unlink);
  HHVM_FE(link);
  HHVM_FE(symlink);
  HHVM_FE(basename);
  HHVM_FE(fnmatch);
  HHVM_FE(glob);
  HHVM_FE(tempnam);
  HHVM_FE(tmpfile);
  HHVM_FE(fileperms);
  HHVM_FE(fileinode);
  HHVM_FE(filesize);
  HHVM_FE(fileowner);
  HHVM_FE(filegroup);
  HHVM_FE(fileatime);
  HHVM_FE(filemtime);
  HHVM_FE(filectime);
  HHVM_FE(filetype);
  HHVM_FE(linkinfo);
  HHVM_FE(is_writable);
  HHVM_FE(is_writeable);
  HHVM_FE(is_readable);
  HHVM_FE(is_executable);
  HHVM_FE(is_file);
  HHVM_FE(is_dir);
  HHVM_FE(is_link);
  HHVM_FE(is_uploaded_file);
  HHVM_FE(file_exists);
  HHVM_FE(stat);
  HHVM_FE(lstat);
  HHVM_FE(clearstatcache);
  HHVM_FE(readlink);
  HHVM_FE(realpath);
  HHVM_FE(pathinfo);
  HHVM_FE(disk_free_space);
  HHVM_FE(diskfreespace);
  HHVM_FE(disk_total_space);
  HHVM_FE(mkdir);
  HHVM_FE(rmdir);
  HHVM_FE(dirname);
  HHVM_FE(getcwd);
  HHVM_FE(chdir);
  HHVM_FE(chroot);
  HHVM_FE(dir);
  HHVM_FE(opendir);
  HHVM_FE(readdir);
  HHVM_FE(rewinddir);
  HHVM_FE(scandir);
  HHVM_FE(closedir);
}

///////////////////////////////////////////////////////////////////////////////
}
