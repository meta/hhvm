#
#   +----------------------------------------------------------------------+
#   | HipHop for PHP                                                       |
#   +----------------------------------------------------------------------+
#   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
#   | Copyright (c) 1997-2010 The PHP Group                                |
#   +----------------------------------------------------------------------+
#   | This source file is subject to version 3.01 of the PHP license,      |
#   | that is bundled with this package in the file LICENSE, and is        |
#   | available through the world-wide-web at the following url:           |
#   | http://www.php.net/license/3_01.txt                                  |
#   | If you did not receive a copy of the PHP license and are unable to   |
#   | obtain it through the world-wide-web, please send a note to          |
#   | license@php.net so we can mail you a copy immediately.               |
#   +----------------------------------------------------------------------+
#

include(CheckFunctionExists)

if (GETDEPS_INSTALL_DIR)
  file(GLOB _getdeps_children "${GETDEPS_INSTALL_DIR}/*")
  foreach(_child ${_getdeps_children})
    if (IS_DIRECTORY "${_child}")
      list(APPEND CMAKE_PREFIX_PATH "${_child}")
    endif()
  endforeach()
endif()

set(FREEBSD FALSE)
if("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
  set(FREEBSD TRUE)
endif()
set(LINUX FALSE)
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
  set(LINUX TRUE)
endif()
set(DARWIN FALSE)
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  set(DARWIN TRUE)
endif()
set(WINDOWS FALSE)
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
  set(WINDOWS TRUE)
endif()

find_program(RUSTC_EXE rustc)
find_program(CARGO_EXE cargo)

# libdl
find_package(LibDL)
if (LIBDL_INCLUDE_DIRS)
  add_definitions("-DHAVE_LIBDL")
  include_directories(${LIBDL_INCLUDE_DIRS})
  if (LIBDL_NEEDS_UNDERSCORE)
    add_definitions("-DLIBDL_NEEDS_UNDERSCORE")
  endif()
endif()

# inotify checks
find_package(Libinotify)
if (LIBINOTIFY_INCLUDE_DIR)
  include_directories(${LIBINOTIFY_INCLUDE_DIR})
endif()

# pcre checks
find_package(PCRE)
include_directories(${PCRE_INCLUDE_DIR})

# libevent checks
find_package(LibEvent REQUIRED)
include_directories(${LIBEVENT_INCLUDE_DIR})

set(CMAKE_REQUIRED_LIBRARIES "${LIBEVENT_LIB}")
CHECK_FUNCTION_EXISTS("evhttp_bind_socket_with_fd" HAVE_CUSTOM_LIBEVENT)
if(HAVE_CUSTOM_LIBEVENT)
        message("Using custom LIBEVENT")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHAVE_CUSTOM_LIBEVENT")
endif()
set(CMAKE_REQUIRED_LIBRARIES)

# libXed
if (HHVM_ENABLE_XED)
  find_package(LibXed)
  if (LibXed_FOUND)
    include_directories(${LibXed_INCLUDE_DIR})
  endif()
endif()
if (HHVM_REQUIRE_XED)
  add_definitions(-DHAVE_XED=1)
endif()

# CURL checks
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIR})
if (CURL_STATIC)
  add_definitions("-DCURL_STATICLIB")
endif()

# LibXML2 checks
find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})
add_definitions(${LIBXML2_DEFINITIONS})

# libsqlite3
find_package(LibSQLite REQUIRED)
if (LIBSQLITE3_INCLUDE_DIR)
  include_directories(${LIBSQLITE3_INCLUDE_DIR})
endif ()

# fastlz
find_package(FastLZ)
if (FASTLZ_INCLUDE_DIR)
  include_directories(${FASTLZ_INCLUDE_DIR})
endif()

# ldap
find_package(Ldap)

# ICU
find_package(ICU REQUIRED)
if (ICU_FOUND)
  if (ICU_VERSION VERSION_LESS "4.2")
    unset(ICU_FOUND CACHE)
    unset(ICU_INCLUDE_DIRS CACHE)
    unset(ICU_LIBRARIES CACHE)
    message(FATAL_ERROR "ICU is too old, found ${ICU_VERSION} and we need 4.2")
  endif ()
  include_directories(${ICU_INCLUDE_DIRS})
  if (ICU_STATIC)
    add_definitions("-DU_EXPORT=")
    add_definitions("-DU_IMPORT=")
  endif()
  # Everything is either in the `icu61` namespace or `icu` namespace, depending
  # on another definition. There's an implicit `using namespace WHATEVER;` in
  # ICU4c < 61.1, but now that's opt-in rather than opt-out.
  add_definitions("-DU_USING_ICU_NAMESPACE=1")
endif (ICU_FOUND)

# OpenSSL libs
find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})

# LibreSSL explicitly refuses to support RAND_egd()
SET(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
SET(CMAKE_REQUIRED_LIBRARIES ${OPENSSL_LIBRARIES})
INCLUDE(CheckCXXSourceCompiles)
CHECK_CXX_SOURCE_COMPILES("#include <openssl/rand.h>
int main() {
  return RAND_egd(\"/dev/null\");
}" OPENSSL_HAVE_RAND_EGD)
if (NOT OPENSSL_HAVE_RAND_EGD)
  add_definitions("-DOPENSSL_NO_RAND_EGD")
endif()
CHECK_CXX_SOURCE_COMPILES("#include <openssl/ssl.h>
int main() {
  return SSL_set_alpn_protos(nullptr, nullptr, 0);
}" OPENSSL_HAVE_ALPN)
SET(CMAKE_REQUIRED_INCLUDES)
SET(CMAKE_REQUIRED_LIBRARIES)


# ZLIB
find_package(ZLIB REQUIRED)
include_directories(${ZLIB_INCLUDE_DIR})

# libpthreads
find_package(PThread REQUIRED)
include_directories(${LIBPTHREAD_INCLUDE_DIRS})
if (LIBPTHREAD_STATIC)
  add_definitions("-DPTW32_STATIC_LIB")
endif()

OPTION(
  NON_DISTRIBUTABLE_BUILD
  "Use libraries which may result in a binary that can not be legally distributed"
  OFF
)

# Either Readline or Editline (for hphpd)
if(NON_DISTRIBUTABLE_BUILD)
  find_package(Readline)
endif()
if (NOT READLINE_INCLUDE_DIR)
  find_package(Editline)
endif()

if (NON_DISTRIBUTABLE_BUILD AND READLINE_INCLUDE_DIR)
  if (READLINE_STATIC)
    add_definitions("-DREADLINE_STATIC")
  endif()
  include_directories(${READLINE_INCLUDE_DIR})
elseif (EDITLINE_INCLUDE_DIRS)
  add_definitions("-DUSE_EDITLINE")
  include_directories(${EDITLINE_INCLUDE_DIRS})
else()
  message(FATAL_ERROR "Could not find Readline or Editline")
endif()

if (NOT WINDOWS)
  find_package(LibDwarf REQUIRED)
  include_directories(${LIBDWARF_INCLUDE_DIRS})
  if (LIBDWARF_CONST_NAME)
    add_definitions("-DLIBDWARF_CONST_NAME")
  endif()
  if (LIBDWARF_USE_INIT_C)
    add_definitions("-DLIBDWARF_USE_INIT_C")
  endif()
  if (NOT LIBDWARF_PRODUCER_FOUND)
    message(FATAL_ERROR
      "Found libdwarf without the producer API HHVM expects. "
      "Build/install the producer-capable OSS libdwarf and pass "
      "-DHHVM_OSS_LIBDWARF_ROOT=<prefix>."
    )
  endif()

  find_package(LibElf REQUIRED)
  include_directories(${LIBELF_INCLUDE_DIRS})
  if (ELF_GETSHDRSTRNDX)
    add_definitions("-DHAVE_ELF_GETSHDRSTRNDX")
  endif()
endif()

FIND_LIBRARY(CRYPT_LIB NAMES xcrypt crypt crypto)
if (LINUX OR FREEBSD)
  FIND_LIBRARY (RT_LIB rt)
endif()

if (LINUX)
  FIND_LIBRARY (CAP_LIB cap)

  if (NOT CAP_LIB)
    message(FATAL_ERROR "You need to install libcap")
  endif()
endif()

if (LINUX OR APPLE)
  FIND_LIBRARY (DL_LIB dl)
  FIND_LIBRARY (RESOLV_LIB resolv)
endif()

if (FREEBSD)
  FIND_LIBRARY (EXECINFO_LIB execinfo)
  if (NOT EXECINFO_LIB)
    message(FATAL_ERROR "You need to install libexecinfo")
  endif()
endif()

if (APPLE)
  find_library(KERBEROS_LIB NAMES gssapi_krb5)
endif()

if (LINUX)
  find_package(systemd REQUIRED)
  find_package(Bpf REQUIRED)
  find_package(LibUnwind REQUIRED)
endif()

# This is required by Homebrew's libc. See
# https://github.com/facebook/hhvm/pull/5728#issuecomment-124290712
# for more info.
find_package(Libpam)
if (PAM_INCLUDE_PATH)
  include_directories(${PAM_INCLUDE_PATH})
endif()

set(
  BOOST_COMPONENTS
  chrono
  context
  date_time
  fiber
  filesystem
  iostreams
  program_options
  regex
  system
  thread
)

set(
  BOOST_COMPONENTS
  atomic
  chrono
  context
  date_time
  fiber
  filesystem
  iostreams
  program_options
  regex
  system
  thread
  headers
)
find_package(Boost 1.70.0  REQUIRED COMPONENTS ${BOOST_COMPONENTS})
if("${Boost_VERSION}" EQUAL "107000")
  # https://github.com/boostorg/variant/issues/69
  message(FATAL_ERROR "boost is blacklisted version")
endif()

add_library(boost INTERFACE)

if(Boost_FOUND)
  message(STATUS "Using system boost")
  target_include_directories(boost BEFORE INTERFACE ${Boost_INCLUDE_DIRS})
  # Not just using ${Boost_LIBRARIES} as this includes imported targets, and
  # third-party dependents (e.g folly) may need the actual path
  foreach(COMPONENT ${BOOST_COMPONENTS})
    string(TOUPPER ${COMPONENT} COMPONENT)
    if(EXISTS "${Boost_${COMPONENT}_LIBRARY_RELEASE}")
      target_link_libraries(boost INTERFACE ${Boost_${COMPONENT}_LIBRARY_RELEASE})
    elseif(EXISTS "${Boost_${COMPONENT}_LIBRARY_DEBUG}")
      target_link_libraries(boost INTERFACE ${Boost_${COMPONENT}_LIBRARY_DEBUG})
    endif()
  endforeach()
endif()

# jemalloc
if (USE_JEMALLOC)
  add_definitions(-DUSE_JEMALLOC=1)

  find_library(JEMALLOC_LIB NAMES jemalloc)
  find_path(JEMALLOC_INCLUDE_DIR NAMES jemalloc/jemalloc.h)
  set(CMAKE_REQUIRED_INCLUDES "${JEMALLOC_INCLUDE_DIR}")
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("
  #include <jemalloc/jemalloc.h>

  #if JEMALLOC_VERSION_MAJOR < 5 || (JEMALLOC_VERSION_MAJOR == 5 && JEMALLOC_VERSION_MINOR < 3)
  # error jemalloc version >= 5.3 required
  #endif

  int main(void) { return 0; }" JEMALLOC_VERSION_MINIMUM)
  set(CMAKE_REQUIRED_INCLUDES)

  if (JEMALLOC_VERSION_MINIMUM)
    message(STATUS "Found jemalloc: ${JEMALLOC_LIB} ${JEMALLOC_INCLUDE_DIR}")
    include_directories(BEFORE SYSTEM "${JEMALLOC_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "jemalloc >=5.3.0 is required")
  endif()
else()
  add_definitions(-DNO_JEMALLOC=1)
endif()

# Needed for hfsort.
find_package(ZLIB REQUIRED)
add_library(zlib INTERFACE)
target_include_directories(zlib INTERFACE ${ZLIB_INCLUDE_DIRS})
target_link_libraries(zlib INTERFACE ${ZLIB_LIBRARIES})

find_library(BROTLIDEC_LIBRARY brotlidec)
find_library(BROTLIENC_LIBRARY brotlienc)
find_library(BROTLICOMMON_LIBRARY brotlicommon)
find_path(BROTLI_INCLUDE_DIR brotli/decode.h)

if(BROTLIDEC_LIBRARY AND BROTLIENC_LIBRARY AND BROTLICOMMON_LIBRARY AND BROTLI_INCLUDE_DIR)
  add_library(brotli INTERFACE)
  message(STATUS "Using system brotli: ${BROTLIDEC_LIBRARY}")
  target_include_directories(brotli INTERFACE "${BROTLI_INCLUDE_DIR}")
  target_link_libraries(brotli INTERFACE
    "${BROTLIDEC_LIBRARY}" "${BROTLIENC_LIBRARY}" "${BROTLICOMMON_LIBRARY}")
else()
  message(FATAL_ERROR "Could not find brotli")
endif()

find_library(ZSTD_LIB NAMES zstd)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)

if(ZSTD_LIB AND ZSTD_INCLUDE_DIR)
  set(CMAKE_REQUIRED_INCLUDES "${ZSTD_INCLUDE_DIR}")
  check_cxx_source_compiles("
#include <zstd.h>
  int main() {
  static_assert(ZSTD_VERSION_MAJOR == 1, \"\");
  static_assert(ZSTD_VERSION_MINOR >= 4, \"\");
    return 0;
  }
  " CAN_USE_SYSTEM_ZSTD)
  set(CMAKE_REQUIRED_INCLUDES)
endif()

if (CAN_USE_SYSTEM_ZSTD)
  add_library(zstd INTERFACE)
  target_include_directories(zstd INTERFACE ${ZSTD_INCLUDE_DIR})
  target_link_libraries(zstd INTERFACE ${ZSTD_LIB})
else()
  message(FATAL_ERROR "zstd >=1.4.x is required")
endif ()

find_package(fmt CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(LibLZMA MODULE REQUIRED)
find_package(Snappy CONFIG REQUIRED)
find_package(FLEX REQUIRED)
find_package(BISON 3.0 REQUIRED)
find_package(LibZip REQUIRED)
find_package(LibSodium 1.0.9 REQUIRED)
if (LIBSODIUM_INCLUDE_DIRS)
  include_directories(${LIBSODIUM_INCLUDE_DIRS})
endif()
find_package(LZ4 REQUIRED)
if (LZ4_INCLUDE_DIR)
  add_library(lz4 INTERFACE)
  target_include_directories(lz4 INTERFACE ${LZ4_INCLUDE_DIR})
  target_link_libraries(lz4 INTERFACE ${LZ4_LIBRARY})
endif()
find_package(re2 CONFIG QUIET)
if(NOT TARGET re2::re2)
  find_package(RE2 MODULE REQUIRED)
endif()

# Use the real xplat/usdt implementation for fbsource OSS builds so
# verification doesn't silently compile away probes.
if(EXISTS "${CMAKE_SOURCE_DIR}/xplat/usdt/usdt.h")
  include_directories("${CMAKE_SOURCE_DIR}/xplat")
else()
  include_directories("${CMAKE_SOURCE_DIR}/third-party/forks")
endif()

if(LINUX)
  # Folly symbolizer implementation details
  find_package(Libiberty REQUIRED)
endif()

# Meta first-party deps: use pre-built libraries via find_package().
# These are built by getdeps.py and found via CMAKE_PREFIX_PATH.
find_package(folly CONFIG REQUIRED)
find_package(fizz CONFIG REQUIRED)
find_package(wangle CONFIG REQUIRED)
find_package(mvfst CONFIG REQUIRED)
# proxygen headers are needed even when FASTCGI is OFF (transport.h includes them)
find_package(proxygen CONFIG REQUIRED)
find_package(FBThrift CONFIG REQUIRED)
if (ENABLE_MCROUTER)
  find_package(mcrouter CONFIG REQUIRED)
endif()

if (HPHP_ENABLE_TYPE_SCANNERS)
  find_package(LLVM REQUIRED CONFIG)
endif()

include_directories(${HPHP_HOME}/hphp)

macro(hphp_link target)
  if (${ARGC} GREATER 1)
    set(VISIBILITY "${ARGV1}")
  else ()
    # We actually want PUBLIC, but specifying PUBLIC is an error if there is
    # another target_link_libraries(${target} ) without PUBLIC/PRIVATE/INTERFACE
    # anywhere else, so keep this for now for backwards compatibility
    set(VISIBILITY "")
  endif ()
  # oniguruma must be linked first for MacOS's linker to do the right thing -
  # that's handled in HPHPSetup.cmake
  #
  # That only handles linking - we still need to make sure that:
  # - oniguruma is built first, if needed (so we have the header files)
  # - we build with the header files in the include path
  if(APPLE)
    if (NOT "${VISIBILITY}" STREQUAL "INTERFACE")
      add_dependencies(${target} ${VISIBILITY} onig)
      target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:onig,INTERFACE_INCLUDE_DIRECTORIES>)
    endif ()
  else()
    # Otherwise, the linker does the right thing, which sometimes means putting it after things that use it
    target_link_libraries(${target} ${VISIBILITY} onig)
  endif()

  if (LIBDL_LIBRARIES)
    target_link_libraries(${target} ${VISIBILITY} ${LIBDL_LIBRARIES})
  endif ()

  if (JEMALLOC_ENABLED)
    target_link_libraries(${target} ${VISIBILITY} ${JEMALLOC_LIB})
  endif ()

  if (GOOGLE_HEAP_PROFILER_ENABLED OR GOOGLE_CPU_PROFILER_ENABLED)
    target_link_libraries(${target} ${VISIBILITY} ${GOOGLE_PROFILER_LIB})
  endif()

  if (GOOGLE_HEAP_PROFILER_ENABLED)
    target_link_libraries(${target} ${VISIBILITY} ${GOOGLE_TCMALLOC_FULL_LIB})
  elseif (GOOGLE_TCMALLOC_ENABLED)
    target_link_libraries(${target} ${VISIBILITY} ${GOOGLE_TCMALLOC_MIN_LIB})
  endif()

  target_link_libraries(${target} ${VISIBILITY} ${LIBSODIUM_LIBRARIES})

  target_link_libraries(${target} ${VISIBILITY} ${PCRE_LIBRARY})
  target_link_libraries(${target} ${VISIBILITY} ${ICU_DATA_LIBRARIES} ${ICU_I18N_LIBRARIES} ${ICU_LIBRARIES})
  target_link_libraries(${target} ${VISIBILITY} ${LIBEVENT_LIB})
  target_link_libraries(${target} ${VISIBILITY} ${CURL_LIBRARIES})

  if (LINUX)
    target_link_libraries(${target} ${VISIBILITY} ${BPF_LIBRARIES} ${SYSTEMD_LIBRARIES})
  endif()

  if (LIBINOTIFY_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${LIBINOTIFY_LIBRARY})
  endif()

  if (LINUX)
    target_link_libraries(${target} ${VISIBILITY} ${CAP_LIB})
  endif()

  if (LINUX OR APPLE)
    target_link_libraries(${target} ${VISIBILITY} ${RESOLV_LIB})
    target_link_libraries(${target} ${VISIBILITY} ${DL_LIB})
  endif()

  if (FREEBSD)
    target_link_libraries(${target} ${VISIBILITY} ${EXECINFO_LIB})
  endif()

  if (APPLE)
    target_link_libraries(${target} ${VISIBILITY} ${LIBINTL_LIBRARIES})
    target_link_libraries(${target} ${VISIBILITY} ${KERBEROS_LIB})
  endif()

  if (PAM_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${PAM_LIBRARY})
  endif()

  if (LIBPTHREAD_LIBRARIES)
    target_link_libraries(${target} ${VISIBILITY} ${LIBPTHREAD_LIBRARIES})
  endif()

  target_link_libraries(${target} ${VISIBILITY} ${OPENSSL_LIBRARIES})
  target_link_libraries(${target} ${VISIBILITY} ${ZLIB_LIBRARIES})

  target_link_libraries(${target} ${VISIBILITY} ${LIBXML2_LIBRARIES})

  target_link_libraries(${target} ${VISIBILITY} ${LBER_LIBRARIES})

  if (CRYPT_LIB)
    target_link_libraries(${target} ${VISIBILITY} ${CRYPT_LIB})
  endif()

  if (LINUX OR FREEBSD)
    target_link_libraries(${target} ${VISIBILITY} ${RT_LIB})
  endif()

  if (LIBSQLITE3_FOUND AND LIBSQLITE3_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${LIBSQLITE3_LIBRARY})
  else()
    target_link_libraries(${target} ${VISIBILITY} sqlite3)
  endif()

  target_link_libraries(${target} ${VISIBILITY} lz4)
  target_link_libraries(${target} ${VISIBILITY} ${LIBZIP_LIBRARY})

  if (PCRE_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${PCRE_LIBRARY})
  else()
    target_link_libraries(${target} ${VISIBILITY} pcre)
  endif()

  if (LIBFASTLZ_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${LIBFASTLZ_LIBRARY})
  else()
    target_link_libraries(${target} ${VISIBILITY} fastlz)
  endif()

  target_link_libraries(${target} ${VISIBILITY} re2::re2)

  target_link_libraries(${target} ${VISIBILITY} timelib)
  target_link_libraries(${target} ${VISIBILITY} Folly::folly)
  target_link_libraries(${target} ${VISIBILITY} ${JEMALLOC_LIB})
  target_link_libraries(${target} ${VISIBILITY} wangle::wangle)
  target_link_libraries(${target} ${VISIBILITY} fizz::fizz)
  target_link_libraries(${target} ${VISIBILITY} brotli)
  target_link_libraries(${target} ${VISIBILITY} magic_enum::magic_enum)
  target_link_libraries(${target} ${VISIBILITY} hhbc_ast_header)
  target_link_libraries(${target} ${VISIBILITY} hack_rust_ffi_bridge)

  target_link_libraries(${target} ${VISIBILITY} tbb)

  target_link_libraries(${target} ${VISIBILITY} afdt)
  target_link_libraries(${target} ${VISIBILITY} mbfl)

  target_link_libraries(${target} ${VISIBILITY} ${LIBLZMA_LIBRARIES} Snappy::snappy)

  if (LINUX)
    target_link_libraries(${target} ${VISIBILITY} ${LIBIBERTY_LIBRARIES} ${LIBELF_LIBRARIES} ${LIBDWARF_LIBRARIES} ${LIBUNWIND_LIBRARIES})
  endif()

  if (EDITLINE_LIBRARIES)
    target_link_libraries(${target} ${VISIBILITY} ${EDITLINE_LIBRARIES})
  elseif (READLINE_LIBRARY)
    target_link_libraries(${target} ${VISIBILITY} ${READLINE_LIBRARY})
  endif()

  find_library(ATOMIC_LIBRARY NAMES atomic libatomic.so.1)
  if (ATOMIC_LIBRARY STREQUAL "ATOMIC_LIBRARY-NOTFOUND")
    # -latomic should be available for gcc even when libatomic.so.1 is not
    # in the library search path
    target_link_libraries(${target} ${VISIBILITY} atomic)
  else()
    target_link_libraries(${target} ${VISIBILITY} ${ATOMIC_LIBRARY})
  endif()

  if (HHVM_REQUIRE_XED)
    if (LibXed_FOUND)
        target_link_libraries(${target} ${VISIBILITY} ${LibXed_LIBRARY})
    else()
        target_link_libraries(${target} ${VISIBILITY} xed)
    endif()
  endif()
endmacro()
