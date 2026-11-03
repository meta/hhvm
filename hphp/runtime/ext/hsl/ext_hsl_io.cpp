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

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/server/cli-server.h"

namespace HPHP {
namespace {
  Array HHVM_FUNCTION(HH_io_pipe) {
    int fds[2];
    if (::pipe(fds) != 0) {
      raise_error("Failed to pipe(2)");
    }
    return make_vec_array(
      req::make<PlainFile>(fds[0]),
      req::make<PlainFile>(fds[1])
    );
  }

  int64_t HHVM_FUNCTION(HH_io_response_write, const OptString& data) {
    if (is_any_cli_mode()) {
      auto stream = dyn_cast_or_null<File>(BuiltinFiles::getSTDOUT());
      if (!stream) {
        return 0;
      }
      // Per `File`'s comments:
      // - write() for PHP stream behavior
      // - writeImpl() for C-like behavior
      return stream->writeImpl(data.c_str(), data.length());
    }
    // Written length is not returned, assume it wrote all, like OutputFile
    g_context->write(data);
    return data.length();
  }

  void HHVM_FUNCTION(HH_io_response_flush) {
    g_context->flush();
  }

  RDS_LOCAL(ssize_t, request_read_offset);

  OptString HHVM_FUNCTION(HH_io_request_read, int64_t max) {
    if (is_any_cli_mode()) {
      auto stream = dyn_cast_or_null<File>(BuiltinFiles::getSTDIN());
      if (!stream) {
        return StrNR(staticEmptyString());
      }
      return stream->read(max);
    }

    const auto offset = *request_read_offset.get();
    const auto raw_post = g_context->getRawPostData();
    const auto remaining = raw_post.size() - offset;
    const auto to_read = max > remaining ? remaining : max;
    const auto result = raw_post.substr(offset, to_read);
    *request_read_offset.get() += to_read;
    return result;
  }

  struct IOExtension final : Extension {

    IOExtension() : Extension("hsl_io", "1.0", "hphp_hphpi") {}

    void moduleRegisterNative() override {
      // Clang 15 doesn't like the HHVM_FALIAS macro with \\N
      HHVM_FALIAS_FE_STR(
        "HH\\Lib\\_Private\\Native\\pipe",
        HH_io_pipe
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\response_write,
        HH_io_response_write
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\response_flush,
        HH_io_response_flush
      );
      HHVM_FALIAS(
        HH\\Lib\\_Private\\_IO\\request_read,
        HH_io_request_read
      );
    }

    void requestInit() {
      *request_read_offset.get() = 0;
    }
  } s_io_extension;

} // anonymous namespace
} // namespace HPHP
