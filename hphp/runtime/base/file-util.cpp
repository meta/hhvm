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
#include "hphp/runtime/base/file-util.h"

#include <algorithm>
#include <vector>
#include <filesystem>

#include <folly/String.h>
#include <folly/portability/Dirent.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Libgen.h>
#include <folly/portability/SysStat.h>

#include "hphp/runtime/base/file-util-defs.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/util/logger.h"
#include "hphp/util/compatibility.h"
#include "hphp/util/process.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

using std::string;

bool FileUtil::mkdir(const std::string &path, int mode /* = 0777 */) {
  if (path.empty()) {
    return false;
  }
  size_t pos = path.rfind('/');
  if (pos != string::npos) {
    // quick test whole path exists
    if (access(path.substr(0, pos).c_str(), F_OK) >= 0) {
      return false;
    }
    for (pos = path.find('/'); pos != string::npos;
         pos = path.find('/', pos + 1)) {
      string subpath = path.substr(0, pos);
      if (subpath.empty()) continue;
      if (access(subpath.c_str(), F_OK) < 0 &&
          ::mkdir(subpath.c_str(), mode) < 0) {
        Logger::Error("unable to mkdir %s", subpath.c_str());
        return false;
      }
    }
  }
  return true;
}

static bool same(const char *file1, const char *file2) {
  FILE *f1 = fopen(file1, "r");
  if (f1 == nullptr) {
    Logger::Error("unable to read %s", file1);
    return false;
  }
  SCOPE_EXIT { fclose(f1); };
  FILE *f2 = fopen(file2, "r");
  if (f2 == nullptr) {
    Logger::Error("unable to read %s", file2);
    return false;
  }
  SCOPE_EXIT { fclose(f2); };

  char buf1[8192];
  char buf2[sizeof(buf1)];
  int n1;
  while ((n1 = fread(buf1, 1, sizeof(buf1), f1))) {
    int toread = n1;
    int pos = 0;
    while (toread) {
      int n2 = fread(buf2 + pos, 1, toread, f2);
      if (n2 <= 0) {
        return false;
      }
      toread -= n2;
      pos += n2;
    }
    if (memcmp(buf1, buf2, n1) != 0) {
      return false;
    }
  }
  if (fread(buf2, 1, 1, f2) == 0) {
    return true;
  }
  return false;
}

void FileUtil::syncdir(const std::string &dest_, const std::string &src_,
                       bool keepSrc /* = false */) {
  if (src_.empty() || dest_.empty()) return;

  string src = src_;
  if (src[src.size() - 1] != '/') src += '/';
  string dest = dest_;
  if (dest[dest.size() - 1] != '/') dest += '/';

  DIR *ddest = opendir(dest.c_str());
  if (ddest == nullptr) {
    Logger::Error("syncdir: unable to open dest %s", dest.c_str());
    return;
  }
  SCOPE_EXIT { closedir(ddest); };

  DIR *dsrc = opendir(src.c_str());
  if (dsrc == nullptr) {
    Logger::Error("syncdir: unable to open src %s", src.c_str());
    return;
  }
  SCOPE_EXIT { closedir(dsrc); };

  dirent *e;

  std::set<string> todelete;
  while ((e = readdir(ddest))) {
    if (strcmp(e->d_name, ".") == 0 ||
        strcmp(e->d_name, "..") == 0) {
      continue;
    }
    string fsrc = src + e->d_name;
    string fdest = dest + e->d_name;

    // delete files/directories that are only in dest
    if (access(fsrc.c_str(), F_OK) < 0) {
      size_t pos = fdest.rfind('.');
      if (pos != string::npos) {
        string ext = fdest.substr(pos + 1);
        // do not delete intermediate files
        if (ext == "o" || ext == "d") {
          continue;
        }
      }
      todelete.insert(fdest);
      continue;
    }

    // delete mismatched types so to copy over new ones
    struct stat sb1, sb2;
    if (stat(fsrc.c_str(), &sb1) != 0 || stat(fdest.c_str(), &sb2) != 0) {
      // Either path is gone; leave the dest entry in place.
      continue;
    }
    if ((sb1.st_mode & S_IFMT) != (sb2.st_mode & S_IFMT)) {
      todelete.insert(fdest.c_str());
      continue;
    }

    // updates
    if ((sb1.st_mode & S_IFMT) == S_IFDIR) {
      syncdir(fdest, fsrc);
    } else if (sb1.st_size != sb2.st_size ||
               !same(fsrc.c_str(), fdest.c_str())) {
      todelete.insert(fdest);
    }
  }

  // delete the ones to delete
  if (!todelete.empty()) {
    for (std::set<string>::const_iterator iter = todelete.begin();
         iter != todelete.end(); ++iter) {
      Logger::Info("sync: deleting %s", iter->c_str());
      std::filesystem::remove_all(*iter);
    }
  }

  // insert new ones
  while ((e = readdir(dsrc))) {
    string fdest = dest + e->d_name;
    if (access(fdest.c_str(), F_OK) < 0) {
      Logger::Info("sync: updating %s", fdest.c_str());
      if (keepSrc) {
        ssystem((string("cp -R ") + src + e->d_name + " " + dest).c_str());
      } else {
        rename((src + e->d_name).c_str(), (dest + e->d_name).c_str());
      }
    }
  }
}

int FileUtil::copy(const char *srcfile, const char *dstfile) {
  int srcFd = open(srcfile, O_RDONLY);
  if (srcFd == -1) return -1;
  SCOPE_EXIT { close(srcFd); };

  struct stat st;
  int ret = fstat(srcFd, &st);
  if (ret == -1) {
    Logger::Error("fstat failed: %s", folly::errnoStr(errno).c_str());
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    Logger::Error("copy failed: the first argument must be a regular file");
    return -1;
  }

  int dstFd = open(dstfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (dstFd == -1) return -1;
  SCOPE_EXIT { close(dstFd); };

  while (1) {
    char buf[8192];
    ssize_t rbytes = read(srcFd, buf, sizeof(buf));
    ssize_t wbytes;
    if (rbytes == 0) break;
    if (rbytes == -1) {
      Logger::Error("read failed: %s", folly::errnoStr(errno).c_str());
      return -1;
    } else if ((wbytes = write(dstFd, buf, rbytes)) != rbytes) {
      Logger::Error("write failed: %zd, %s", wbytes,
                    folly::errnoStr(errno).c_str());
      return -1;
    }
  }
  return 0;
}

static int force_sync(int fd) {
  return fdatasync(fd);
}

int FileUtil::directCopy(const char *srcfile, const char *dstfile) {
  int srcFd = open(srcfile, O_RDONLY);
  if (srcFd == -1) return -1;
  SCOPE_EXIT { close(srcFd); };

  struct stat st;
  int ret = fstat(srcFd, &st);
  if (ret == -1) {
    Logger::Error("fstat failed: %s", folly::errnoStr(errno).c_str());
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    Logger::Error("copy failed: the first argument must be a regular file");
    return -1;
  }

  int dstFd = open(dstfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (dstFd == -1) return -1;
  SCOPE_EXIT { close(dstFd); };

  while (1) {
    char buf[1 << 20];
    bool err = false;
    ssize_t rbytes = read(srcFd, buf, sizeof(buf));
    ssize_t wbytes;
    if (rbytes == 0) break;

    if (rbytes == -1) {
      err = true;
      Logger::Error("read failed: %s", folly::errnoStr(errno).c_str());
    } else if (force_sync(srcFd) == -1) {
      err = true;
      Logger::Error("read sync failed: %s",
                    folly::errnoStr(errno).c_str());
    } else if (fadvise_dontneed(srcFd, 0) == -1) {
      err = true;
      Logger::Error("read cache drop failed: %s",
                    folly::errnoStr(errno).c_str());
    } else if ((wbytes = write(dstFd, buf, rbytes)) != rbytes) {
      err = true;
      Logger::Error("write failed: %zd, %s", wbytes,
                    folly::errnoStr(errno).c_str());
    } else if (force_sync(dstFd) == -1) {
      err = true;
      Logger::Error("write sync failed: %s",
                    folly::errnoStr(errno).c_str());
    } else if (fadvise_dontneed(dstFd, 0) == -1) {
      err = true;
      Logger::Error("write cache drop failed: %s",
                    folly::errnoStr(errno).c_str());
    }
    if (err) {
      return -1;
    }
  }
  return 0;
}

int FileUtil::rename(const char *oldname, const char *newname) {
  int ret = ::rename(oldname, newname);
  if (ret == 0) return 0;
  if (errno != EXDEV) return -1;

  ret = copy(oldname, newname);
  if (ret != 0) {
    return -1;
  }
  unlink(oldname);
  return 0;
}

int FileUtil::directRename(const char *oldname, const char *newname) {
  int ret = ::rename(oldname, newname);
  if (ret == 0) return 0;
  if (errno != EXDEV) return -1;

  ret = directCopy(oldname, newname);
  if (ret != 0) {
    return -1;
  }
  unlink(oldname);
  return 0;
}

int FileUtil::ssystem(const char* command) {
  int ret = system(command);
  if (ret == -1) {
    Logger::Error("system(\"%s\"): %s", command,
                  folly::errnoStr(errno).c_str());
  } else if (ret != 0) {
    Logger::Error("command failed: \"%s\"", command);
  }
  return ret;
}

namespace {

size_t dirname_impl(char *path, int len) {
  if (len == 0) {
    /* Illegal use of this function */
    return 0;
  }

  /* Strip trailing slashes */
  char *end = path + len - 1;
  while (end >= path && FileUtil::isDirSeparator(*end)) {
    end--;
  }
  if (end < path) {
    /* The path only contained slashes */
    path[0] = FileUtil::getDirSeparator();
    path[1] = '\0';
    return 1;
  }

  /* Strip filename */
  while (end >= path && !FileUtil::isDirSeparator(*end)) {
    end--;
  }
  if (end < path) {
    /* No slash found, therefore return '.' */
    path[0] = '.';
    path[1] = '\0';
    return 1;
  }

  /* Strip slashes which came before the file name */
  while (end >= path && FileUtil::isDirSeparator(*end)) {
    end--;
  }
  if (end < path) {
    path[0] = FileUtil::getDirSeparator();
    path[1] = '\0';
    return 1;
  }
  *(end+1) = '\0';

  return end + 1 - path;
}

}

OptString FileUtil::dirname(const OptString& path) {
  OptString str{path, CopyString};
  auto new_length = dirname_impl(str.mutableData(), str.length());
  str.setSize(new_length);
  return str;
}

OptString FileUtil::relativePath(const std::string& fromDir,
                                 const OptString& toFile) {

  size_t maxlen = (fromDir.size() + toFile.size()) * 3;

  // Sanity checks
  if (!isAbsolutePath(fromDir) || !isAbsolutePath(toFile.slice()) ||
      !isDirSeparator(fromDir[fromDir.size() - 1])) {
    return String::Empty();
  }

  // Maybe we're lucky and this is an easy case
  int from_len = fromDir.size();
  if (strncmp(toFile.c_str(), fromDir.c_str(), from_len) == 0) {
    return toFile.substr(from_len);
  }

  OptString ret(maxlen, ReserveString);
  char* path = ret.mutableData();

  const char* from_dir = fromDir.c_str();
  const char* to_file = toFile.c_str();

  const char* from_start = from_dir + 1;
  const char* to_start = to_file + 1;

  while (true) {
    int seg_len = 0;
    char cur = from_start[0];
    while (cur && !isDirSeparator(cur)) {
      ++seg_len;
      cur = from_start[seg_len];
    }

    if (memcmp(from_start, to_start, seg_len + 1)) {
      break;
    }
    from_start += seg_len + 1;
    to_start += seg_len + 1;
  }

  // Now to build the path
  char cur = *from_start;
  char* path_end = path;
  while (cur) {
    if (isDirSeparator(cur)) {
      strcpy(path_end, "..");
      path_end[2] = getDirSeparator();
      maxlen -= 3;
      path_end += 3;
    }
    ++from_start;
    cur = *from_start;
  }

  if (!isDirSeparator(from_start[-1])) {
    strcpy(path_end, "..");
    path_end[2] = getDirSeparator();
    maxlen -= 3;
    path_end += 3;
  }

  // Ensure the result is null-terminated after the strcpy
  assertx(to_start - to_file <= toFile.size());
  assertx(path_end - path + strlen(to_start) <= ret.capacity());

  strcpy(path_end, to_start);
  return ret.setSize(strlen(path));
}

OptString FileUtil::canonicalize(const OptString &path) {
  return canonicalize(path.data(), path.size());
}

OptString FileUtil::canonicalize(const std::string &path) {
  return canonicalize(path.c_str(), path.size());
}

/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

OptString FileUtil::canonicalize(const char *addpath, size_t addlen,
                                 bool collapse_slashes /* = true */) {
  assertx(strlen(addpath) <= addlen);
  // 4 for slashes at start, after root, and at end, plus trailing
  // null
  size_t maxlen = addlen + 4;
  size_t pathlen = 0; // is the length of the result path
  size_t seglen;  // is the end of the current segment
  auto pathend = addpath + addlen;

  /* Treat null as an empty path.
   */
  if (!addpath)
    addpath = "";

  OptString ret(maxlen-1, ReserveString);
  char *path = ret.mutableData();

  if (addpath[0] == '/' && collapse_slashes) {
    /* Ignore the given root path, strip off leading
     * '/'s to a single leading '/' from the addpath,
     * and leave addpath at the first non-'/' character.
     */
    while (addpath[0] == '/')
      ++addpath;
    path[0] = '/';
    pathlen = 1;
  }

  while (*addpath) {
    /* Parse each segment, find the closing '/'
     */
    const char *next = addpath;
    while (*next && !isDirSeparator(*next)) {
      ++next;
    }
    seglen = next - addpath;

    if (seglen == 0) {
      /* / */
      if (!collapse_slashes) {
        path[pathlen++] = getDirSeparator();
      }
    } else if (seglen == 1 && addpath[0] == '.'
               && (pathlen > 0 || (*next && *(next+1)))) {
      /* ./ (safe to drop iff there is something before or after it) */
    } else if (seglen == 2 && addpath[0] == '.' && addpath[1] == '.') {
      /* backpath (../) */
      if (pathlen == 1 && isDirSeparator(path[0])) {
      } else if (pathlen == 0
                 || (pathlen == 3
                     && !memcmp(path + pathlen - 3, "..", 2)
                     && isDirSeparator(path[pathlen - 1]))
                 || (pathlen  > 3
                     && isDirSeparator(path[pathlen - 4])
                     && !memcmp(path + pathlen - 3, "..", 2)
                     && isDirSeparator(path[pathlen - 1]))) {
        /* Append another backpath, including
         * trailing slash if present.
         */
        memcpy(path + pathlen, "..", 2);
        if (*next) {
          path[pathlen + 2] = getDirSeparator();
        }
        pathlen += *next ? 3 : 2;
      } else {
        /* otherwise crop the prior segment */
        do {
          --pathlen;
        } while (pathlen && !isDirSeparator(path[pathlen - 1]));
      }
    } else {
      /* An actual segment, append it to the destination path */
      if (*next) {
        seglen++;
      }
      memcpy(path + pathlen, addpath, seglen);
      pathlen += seglen;
    }

    /* Skip over trailing slash to the next segment */
    if (*next) {
      ++next;
    }

    addpath = next;
  }

  // If there are null bytes in the path, treat it as the empty string
  if (addpath != pathend) pathlen = 0;

  ret.setSize(pathlen);
  return ret;
}

std::string FileUtil::expandUser(const std::string& path,
                                 const std::string& sysUser) {
  if (path.front() != '~') {
    return path;
  }

  auto pos = std::min(path.find('/'), path.size());
  auto user = (pos > 1) ? path.substr(1, pos - 1) : sysUser;

  auto defaultUser = user.empty() || user == Process::GetCurrentUser();
  auto home = defaultUser ? Process::GetHomeDirectory() : "/home/" + user + "/";

  if (pos + 1 < path.size()) {
    return home + path.substr(pos + 1);
  } else {
    return home;
  }
}

std::string FileUtil::normalizeDir(const std::string &dirname) {
  /*
   * normalizeDir may be called before very early one, such as
   * in Runtime option parsing, when MemoryManager may not have been
   * initialized
   */
  tl_heap.getCheck();
  string ret = FileUtil::canonicalize(dirname).toCppString();
  if (!ret.empty() && !isDirSeparator(ret[ret.length() - 1])) {
    ret += getDirSeparator();
  }
  return ret;
}

void FileUtil::find(std::vector<std::string> &out,
                    const std::string &root, const std::string& path, bool php,
                    bool failHard,
                    const hphp_fast_string_set *excludeDirs /* = NULL */,
                    const hphp_fast_string_set *excludeFiles /* = NULL */) {

  find(root, path, php, failHard,
       [&] (const std::string& rpath, bool isDir, size_t) {
         if (isDir) {
           return !excludeDirs || !excludeDirs->count(rpath);
         }
         if (!excludeFiles || !excludeFiles->count(rpath)) {
           out.push_back(root + rpath);
         }
         return false;
       });
}

bool FileUtil::isValidPath(const OptString& path) {
  return path.size() == strlen(path.data());
}

bool FileUtil::checkPathAndWarn(const OptString& path,
                                const char* func_name,
                                int param_pos) {
  if (!FileUtil::isValidPath(path)) {
    raise_warning(
      "%s() expects parameter %d to be a valid path, string given",
      func_name,
      param_pos
    );
    return false;
  }
  return true;
}

void FileUtil::checkPathAndError(const OptString& path,
                                 const char* func_name,
                                 int param_pos) {
  if (!FileUtil::isValidPath(path)) {
    raise_error(
      "%s() expects parameter %d to be a valid path, string given",
      func_name,
      param_pos
    );
  }
}

bool FileUtil::isSystemName(folly::StringPiece path) {
  static const char prefix[] = "/:";
  return !strncmp(path.begin(), prefix, sizeof prefix - 1);
}

///////////////////////////////////////////////////////////////////////////////
}
