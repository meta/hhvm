# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
# URL downloads timestamp extracted files at extraction time.
if (POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/build/fbcode_builder/CMake"
  ${CMAKE_MODULE_PATH})

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  message(STATUS "setting C++ standard to C++${CMAKE_CXX_STANDARD}")
endif()

# Explicitly enable coroutine support, since GCC does not enable it
# by default when targeting C++17/20.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fcoroutines>)
endif()

if (WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DSTRICT")
endif()

option(BUILD_SHARED_LIBS
  "If enabled, build libraries as a shared library.  \
  This is generally discouraged, since we do not commit to having \
  a stable ABI."
  OFF
)
# Mark BUILD_SHARED_LIBS as an "advanced" option, since enabling it
# is generally discouraged.
mark_as_advanced(BUILD_SHARED_LIBS)

option(USE_SYS_PYTHON
  "If enabled, prefers to use the system installed python vs the user \
  installed python. Flipping this switch will change which python is used \
  during packaging and should be set to maximize portability."
  ON
)

enable_testing()
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

option(ENABLE_EDEN_SUPPORT "If enabled, add support for the Eden \
  virtual filesystem.  That requires fbthrift."
  ON)

# A nonsensical version that doesn't correspond to any manually
# released version.
set(PACKAGE_VERSION "0.0.0")
set(WATCHMAN_VERSION_OVERRIDE "" CACHE STRING "Use this version code for \
    Watchman version instead of the default computed from the repo")
set(WATCHMAN_BUILDINFO_OVERRIDE "" CACHE STRING "Use this version code for \
    Watchman build info instead of the default (nothing)")

if (WATCHMAN_VERSION_OVERRIDE)
  set(PACKAGE_VERSION "${WATCHMAN_VERSION_OVERRIDE}")
elseif(DEFINED ENV{WATCHMAN_VERSION_OVERRIDE})
  set(PACKAGE_VERSION "$ENV{WATCHMAN_VERSION_OVERRIDE}")
elseif(DEFINED ENV{FBSOURCE_DATE})
  # If set, we expect FBSOURCE_DATE to have the form "20200324.113140"
  set(PACKAGE_VERSION "$ENV{FBSOURCE_DATE}.0")
  set(BUILD_INFO "$ENV{FBSOURCE_HASH}")
else()
  find_program(GIT git)
  if(GIT)
    execute_process(
      COMMAND "${GIT}" "show" "-s" "--format=%H;%cd" "--date=format:%Y%m%d.%H%M%S.0"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      RESULT_VARIABLE git_result
      OUTPUT_VARIABLE git_data
      ERROR_VARIABLE git_err
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_result EQUAL 0)
      list(GET git_data 0 BUILD_INFO)
      list(GET git_data 1 PACKAGE_VERSION)
    endif()
  endif()
endif()
message(STATUS "PACKAGE_VERSION=${PACKAGE_VERSION}, BUILD_INFO=${BUILD_INFO}")

set(PACKAGE_NAME      "watchman")
set(PACKAGE_STRING    "${PACKAGE_NAME} ${PACKAGE_VERSION}")
set(PACKAGE_TARNAME   "${PACKAGE_NAME}-${PACKAGE_VERSION}")
set(PACKAGE_BUGREPORT "https://github.com/facebook/watchman/issues")
project(${PACKAGE_NAME} CXX C)

include(GoogleTest)
enable_testing()

include(FBThriftCppLibrary)
include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckStructHasMember)
include(CheckSymbolExists)
include(RustStaticLibrary)

# configure_file wants us to define a separate file.  I'd rather not
# have boilerplate for the same thing in two difference files, so we
# roll the checks in together with writing out the features to config.h
# ourselves here.
function(config_h LINE)
  file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h.new" "${LINE}\n")
endfunction()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h.new" "#pragma once\n")

if(NOT WIN32)
  set(WATCHMAN_STATE_DIR "${CMAKE_INSTALL_PREFIX}/var/run/watchman" CACHE STRING
    "Run-time path of the persistent state directory")
  set(INSTALL_WATCHMAN_STATE_DIR OFF CACHE BOOL
    "Whether WATCHMAN_STATE_DIR should be created by the cmake install
    target.  Disabling this is useful in the case where the CMAKE_INSTALL_PREFIX
    is owned by a non-privileged user but where the WATCHMAN_STATE_DIR requires
    administrative rights to create and set its permissions.")
  option(WATCHMAN_USE_XDG_STATE_HOME
    "If enabled, use $XDG_STATE_HOME/watchman as the Watchman state directory. \
    XDG_STATE_HOME defaults to $HOME/.local/state."
    OFF
  )
else()
  set(WATCHMAN_STATE_DIR)
  set(INSTALL_WATCHMAN_STATE_DIR)
endif()

if(WATCHMAN_STATE_DIR AND INSTALL_WATCHMAN_STATE_DIR)
  install(DIRECTORY DESTINATION ${WATCHMAN_STATE_DIR}
    DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_WRITE
    GROUP_EXECUTE WORLD_READ WORLD_WRITE WORLD_EXECUTE SETGID)
endif()

config_h("// Generated by cmake")
if(WIN32)
  config_h("#define WATCHMAN_CONFIG_FILE \
\"C:/ProgramData/facebook/watchman.json\"")
else()
  config_h("#define WATCHMAN_CONFIG_FILE \"/etc/watchman.json\"")
endif()

if(WATCHMAN_STATE_DIR)
  config_h("#define WATCHMAN_STATE_DIR \"${WATCHMAN_STATE_DIR}\"")
endif()
config_h("#define PACKAGE_VERSION \"${PACKAGE_VERSION}\"")

if(BUILD_INFO)
  config_h("#define WATCHMAN_BUILD_INFO \"${BUILD_INFO}\"")
endif ()

if(WATCHMAN_USE_XDG_STATE_HOME)
  config_h("#define WATCHMAN_USE_XDG_STATE_HOME 1")
endif()

# While most of these tests are not strictly needed on windows, it is vital
# that we probe for and find strtoll in order for the jansson build to use
# a 64-bit integer type, otherwise the mtime_us field renders as garbage
# in the integration tests.
foreach(wat_func
    accept4
    backtrace
    backtrace_symbols
    backtrace_symbols_fd
    close_range
    fdopendir
    getattrlistbulk
    inotify_init
    inotify_init1
    kqueue
    localeconv
    memmem
    mkostemp
    openat
    pipe2
    port_create
    statfs
    strtoll
    sys_siglist
)
  CHECK_FUNCTION_EXISTS(${wat_func} have_${wat_func})
  if (have_${wat_func})
    string(TOUPPER have_${wat_func} sym)
    config_h("#define ${sym} 1")
  endif()
endforeach(wat_func)

foreach(wat_header
    CoreServices/CoreServices.h
    execinfo.h
    fcntl.h
    inttypes.h
    locale.h
    port.h
    sys/event.h
    sys/inotify.h
    sys/mount.h
    sys/param.h
    sys/resource.h
    sys/socket.h
    sys/statfs.h
    sys/statvfs.h
    sys/types.h
    sys/ucred.h
    sys/vfs.h
    valgrind/valgrind.h
)
  string(TOUPPER have_${wat_header} sym)
  string(REGEX REPLACE [./] _ sym ${sym})
  CHECK_INCLUDE_FILES(${wat_header} ${sym})
  if (${sym})
    config_h("#define ${sym} 1")
  endif()
endforeach(wat_header)

CHECK_STRUCT_HAS_MEMBER(statvfs f_fstypename sys/statvfs.h
  HAVE_STRUCT_STATVFS_F_FSTYPENAME)
if (HAVE_STRUCT_STATVFS_F_BASETYPE)
  config_h("define HAVE_STRUCT_STATVFS_F_FSTYPENAME 1")
endif()

CHECK_STRUCT_HAS_MEMBER(statvfs f_basetype sys/statvfs.h
  HAVE_STRUCT_STATVFS_F_BASETYPE)
if (HAVE_STRUCT_STATVFS_F_BASETYPE)
  config_h("define HAVE_STRUCT_STATVFS_F_BASETYPE 1")
endif()

if(have_fcntl.h)
  CHECK_SYMBOL_EXISTS(O_SYMLINK fcntl.h HAVE_DECL_O_SYMLINK)
  if(HAVE_DECL_O_SYMLINK)
    config_h("#define HAVE_DECL_O_SYMLINK 1")
  endif()
endif()
find_package(PCRE2)
if(PCRE2_FOUND)
  config_h("#define HAVE_PCRE_H 1")
endif()

# Now close out config.h.  We only want to touch the file if the contents are
# different, so do a little dance to figure that out.
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h")
  file(MD5 "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h" orig_hash)
  file(MD5 "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h.new" this_hash)
  if(NOT orig_hash STREQUAL this_hash)
    file(RENAME "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h.new"
      "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h")
  endif()
else()
  file(RENAME "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h.new"
    "${CMAKE_CURRENT_BINARY_DIR}/watchman/config.h")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(OpenSSL)

include(FBFetchFromManifest)

# We indirectly depend on libevent.  An installed folly can link it as
# simply "event.lib", which fails linking, so probe for the library and
# force in the library directory for the linker.  A fetched folly
# fetches libevent itself when it isn't installed.
find_package(LibEvent)
get_filename_component(LIBEVENT_LIBDIR "${LIBEVENT_LIB}" DIRECTORY)
link_directories(${LIBEVENT_LIBDIR})

# The Boost that folly fetches has only the libraries that folly links.
# edencommon links Boost.Filesystem.
list(APPEND BOOST_INCLUDE_LIBRARIES filesystem)
fb_find_or_fetch(folly)
include_directories(${FOLLY_INCLUDE_DIR})

# After folly, so that what folly fetched satisfies these.  Config mode, as
# eden uses: with vcpkg, FindGflags defines `gflags` as an imported library
# whose file is the name of vcpkg's gflags::gflags_static target.
find_package(gflags CONFIG REQUIRED)
include_directories(SYSTEM ${GFLAGS_INCLUDE_DIR})

find_package(Glog REQUIRED)
add_compile_definitions(GLOG_NO_ABBREVIATED_SEVERITIES)

# We indirectly depend on boost.  This logic needs to match
# the same criteria used in thrift, which wants static libs
# on windows.
if(MSVC)
  set(Boost_USE_STATIC_LIBS ON) #Force static lib in msvc
endif(MSVC)
find_package(
  Boost 1.54.0 REQUIRED #1.54.0 or greater
  COMPONENTS
    context
    thread
)
find_package(fmt CONFIG REQUIRED)

# Watchman's telemetry uses fb303, and edencommon's links fb303 and fbthrift,
# so these are needed even without eden support.
fb_find_or_fetch(fizz)
fb_find_or_fetch(wangle)
fb_find_or_fetch(FBThrift)
include_directories(${FBTHRIFT_INCLUDE_DIR})
fb_find_or_fetch(fb303)
include_directories(${FB303_INCLUDE_DIR})
fb_find_or_fetch(edencommon)

if (ENABLE_EDEN_SUPPORT)
  find_package(cpptoml CONFIG QUIET)
  if (NOT cpptoml_FOUND)
    # cpptoml asks for CMake 3.1, which leaves CMP0077 unset, so its option()s
    # would clear these settings, and which CMake 4 refuses.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(CPPTOML_BUILD_EXAMPLES OFF)
    set(ENABLE_LIBCXX OFF)
    fb_fetch_from_manifest(cpptoml cpptoml)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    unset(CMAKE_POLICY_DEFAULT_CMP0077)
  endif()
endif()

fb_find_or_fetch_gmock()
include_directories(${GMOCK_INCLUDEDIR} ${LIBGMOCK_INCLUDE_DIR})
if(DEFINED ENV{NODE_BIN})
  set(NODE $ENV{NODE_BIN})
else()
  find_program(NODE node)
endif()
if(DEFINED ENV{YARN_PATH})
  set(YARN $ENV{YARN_PATH})
else()
  find_program(YARN yarn)
endif()


if(NOT WIN32)
  # Sometimes the environment has a Python installation earlier in the
  # PATH that is not suitable for building extensions. Prefer the
  # primary system installation.
  set(Python3_FIND_STRATEGY LOCATION)
  if(USE_SYS_PYTHON)
    message(STATUS "Using sys python")
    set(Python3_ROOT_DIR /usr/bin)
  else()
    message(STATUS "Using local python")
    set(Python3_ROOT_DIR /usr/local/bin)
  endif()
endif()

find_package(Python3 COMPONENTS Interpreter Development)

if(NOT Python3_Interpreter_FOUND)
  message(STATUS "python not found, using default python")
  unset(Python3_ROOT_DIR)
  unset(Python3_FIND_STRATEGY)
  find_package(Python3 COMPONENTS Interpreter Development)
endif()


if(Python3_Development_FOUND)
  set(PYOUT "${CMAKE_CURRENT_BINARY_DIR}/pywatchman")
  set(SETUP_PY "${CMAKE_CURRENT_SOURCE_DIR}/watchman/python/setup.py")
  set(PYWATCHMAN_BASE ${CMAKE_CURRENT_SOURCE_DIR}/watchman/python)
  file(GLOB PYWATCHMAN_PY_SRCS "watchman/python/pywatchman/*.py")
  file(MAKE_DIRECTORY ${PYOUT})
  add_custom_command(
    COMMENT "Building pywatchman"
    OUTPUT ${PYOUT}
    DEPENDS ${PYWATCHMAN_PY_SRCS} "watchman/python/pywatchman/bser.c"
    WORKING_DIRECTORY ${PYWATCHMAN_BASE}
    COMMAND ${CMAKE_COMMAND} -E env
      CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      CMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      ${Python3_EXECUTABLE} ${SETUP_PY} build --build-base ${PYOUT}
    COMMAND ${CMAKE_COMMAND} -E touch ${PYOUT}
  )
  add_custom_target(pybuild ALL DEPENDS ${PYOUT})
  install(CODE "
    execute_process(COMMAND
      ${CMAKE_COMMAND} -E env
        CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        ${Python3_EXECUTABLE} ${SETUP_PY} install
        --root $ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}
      WORKING_DIRECTORY ${PYWATCHMAN_BASE}
      RESULT_VARIABLE STATUS)
    if (NOT STATUS STREQUAL 0)
      message(FATAL_ERROR \"pywatchman install failed\")
    endif()
  ")
  include(FBPythonBinary)
  add_subdirectory(watchman/python)
  add_subdirectory(watchman/integration)
endif()

if(Python3_Interpreter_FOUND AND NODE AND YARN)
  add_subdirectory(watchman/node/bser)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Check target architecture
  if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "watchman requires a 64bit target architecture.")
  endif()
  add_definitions(-D_CRT_SECURE_NO_WARNINGS=1)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/watchman/winbuild)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Z7 /Zo /MP /Oi /EHsc /GL- /wd4250")
  set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} /DEBUG /OPT:NOREF")
  set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} /DEBUG /OPT:NOREF")
  set(CMAKE_MODULE_LINKER_FLAGS
    "${CMAKE_MODULE_LINKER_FLAGS} /DEBUG /OPT:NOREF")
  set(CMAKE_STATIC_LIBRARY_FLAGS
    "${CMAKE_STATIC_LIBRARY_FLAGS} /DEBUG /OPT:NOREF")
else()
  set(CMAKE_CXX_FLAGS_COMMON "-g -Wall -Wextra -std=gnu++17")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_COMMON}")  # for cmake 3.0
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_COMMON}")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_COMMON} -O3")
endif()

add_library(third_party_deps INTERFACE)
target_link_libraries(third_party_deps INTERFACE
  Folly::folly
  glog::glog
  gflags
  ${Boost_LIBRARIES}
  fb303::fb303
  fmt::fmt
  edencommon::edencommon_utils
  edencommon::edencommon_telemetry
)
target_include_directories(third_party_deps INTERFACE
  ${FOLLY_INCLUDE_DIR}
  ${GLOG_INCLUDE_DIR}
  ${GFLAGS_INCLUDE_DIR}
  ${Boost_INCLUDE_DIRS}
)
if (ENABLE_EDEN_SUPPORT)
  target_link_libraries(
    third_party_deps
    INTERFACE
    FBThrift::thriftcpp2
    cpptoml
  )
endif()
if(PCRE2_FOUND)
  target_link_libraries(third_party_deps INTERFACE ${PCRE2_LIBRARY})
  target_include_directories(third_party_deps INTERFACE ${PCRE2_INCLUDE_DIR})
  target_compile_definitions(third_party_deps INTERFACE ${PCRE2_DEFINES})
  if (WIN32)
    # The pcre headers assume that the library is a dll by default
    # but our preferred build environment only builds them as
    # static, so be sure to ask for static pcre linkage
    target_compile_definitions(third_party_deps INTERFACE PCRE2_STATIC)
  endif()
endif()
target_link_libraries(third_party_deps INTERFACE Threads::Threads)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(third_party_deps INTERFACE OpenSSL::Crypto)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(third_party_deps INTERFACE "-framework CoreServices")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_libraries(third_party_deps INTERFACE
    advapi32.lib
    dbghelp.lib
    shlwapi.lib
  )
endif()

add_library(wildmatch STATIC
  watchman/thirdparty/wildmatch/wildmatch.c
  watchman/thirdparty/wildmatch/wildmatch.h
)
add_library(log STATIC
  watchman/PubSub.cpp
  watchman/LogConfig.cpp
  watchman/Logging.cpp
  watchman/portability/Backtrace.cpp
)
target_link_libraries(log third_party_deps)
add_library(err STATIC watchman/Poison.cpp watchman/root/warnerr.cpp)
target_link_libraries(err third_party_deps)
add_library(jansson_utf STATIC watchman/thirdparty/jansson/utf.cpp)

add_library(string STATIC watchman/string.cpp)
target_link_libraries(string jansson_utf third_party_deps)

add_library(jansson STATIC
watchman/thirdparty/jansson/dump.cpp
watchman/thirdparty/jansson/error.cpp
watchman/thirdparty/jansson/load.cpp
watchman/thirdparty/jansson/strconv.cpp
watchman/thirdparty/jansson/value.cpp
)
target_link_libraries(jansson string third_party_deps)

list(APPEND testsupport_sources
watchman/ChildProcess.cpp
watchman/fs/FileDescriptor.cpp
watchman/fs/FileInformation.cpp
watchman/fs/FSDetect.cpp
watchman/FlagMap.cpp
watchman/IgnoreSet.cpp
watchman/PendingCollection.cpp
watchman/fs/Pipe.cpp
watchman/fs/WindowsTime.cpp
watchman/ThreadPool.cpp
watchman/WatchmanConfig.cpp
watchman/bser.cpp
watchman/fs/UnixDirHandle.cpp
watchman/fs/WinDirHandle.cpp
watchman/stream.cpp
watchman/stream_unix.cpp
watchman/stream_win.cpp
watchman/portability/PosixSpawn.cpp
watchman/portability/WinError.cpp
watchman/root/dir.cpp
watchman/root/file.cpp
)

add_library(testsupport STATIC ${testsupport_sources})
target_link_libraries(testsupport log string jansson third_party_deps)

if (ENABLE_EDEN_SUPPORT)
  add_fbthrift_cpp_library(
    eden_config_thrift
    eden/fs/config/eden_config.thrift
  )
  add_fbthrift_cpp_library(
    eden_service_thrift
    eden/fs/service/eden.thrift
    SERVICES
      EdenService
    DEPENDS
      eden_config_thrift
      edencommon::edencommon_utils
      fb303::fb303_thrift_cpp
  )
  add_fbthrift_cpp_library(
    streamingeden_thrift
    eden/fs/service/streamingeden.thrift
    SERVICES
      StreamingEdenService
    DEPENDS
      eden_service_thrift
      fb303::fb303_thrift_cpp
  )
endif()

list(APPEND watchman_sources
watchman/ChildProcess.cpp
watchman/Client.cpp
watchman/Clock.cpp
watchman/CloseRandomFds.cpp
watchman/Command.cpp
watchman/CommandRegistry.cpp
watchman/Connect.cpp
watchman/ContentHash.cpp
watchman/CookieSync.cpp
watchman/Errors.cpp
watchman/fs/FileDescriptor.cpp
watchman/fs/FileInformation.cpp
watchman/fs/FileSystem.cpp
watchman/FlagMap.cpp
watchman/fs/FSDetect.cpp
watchman/GroupLookup.cpp
watchman/IgnoreSet.cpp
watchman/InMemoryView.cpp
watchman/Options.cpp
watchman/PathUtils.cpp
watchman/PDU.cpp
watchman/PendingCollection.cpp
watchman/PerfSample.cpp
watchman/fs/ParallelWalk.cpp
watchman/fs/Pipe.cpp
watchman/ProcessLock.cpp
watchman/ProcessUtil.cpp
# PubSub.cpp  (in liblog)
watchman/QueryableView.cpp
watchman/SanityCheck.cpp
watchman/Shutdown.cpp
watchman/SignalHandler.cpp
watchman/SymlinkTargets.cpp
watchman/ThreadPool.cpp
watchman/TriggerCommand.cpp
watchman/fs/UnixDirHandle.cpp
watchman/fs/WindowsTime.cpp
watchman/UserDir.cpp
watchman/WatchmanConfig.cpp
watchman/XattrUtils.cpp
watchman/fs/WinDirHandle.cpp
watchman/bser.cpp
watchman/listener-user.cpp
watchman/listener.cpp
watchman/main.cpp
watchman/sockname.cpp
watchman/state.cpp
watchman/stream.cpp
watchman/stream_unix.cpp
watchman/stream_stdout.cpp
watchman/stream_win.cpp
# string.cpp (in libstring)
watchman/portability/PosixSpawn.cpp
watchman/portability/WinError.cpp
watchman/query/FileResult.cpp
watchman/query/LocalFileResult.cpp
watchman/query/GlobEscaping.cpp
watchman/query/GlobTree.cpp
watchman/query/QueryContext.cpp
watchman/query/Query.cpp
watchman/query/QueryResult.cpp
watchman/query/TermRegistry.cpp
watchman/query/base.cpp
watchman/query/dirname.cpp
watchman/query/empty.cpp
watchman/query/eval.cpp
watchman/query/fieldlist.cpp
watchman/query/glob.cpp
watchman/query/intcompare.cpp
watchman/query/match.cpp
watchman/query/name.cpp
watchman/query/parse.cpp
watchman/query/pcre.cpp
watchman/query/since.cpp
watchman/query/suffix.cpp
watchman/query/type.cpp
watchman/cmds/debug.cpp
watchman/cmds/find.cpp
# cmds/heapprof.cpp
watchman/cmds/info.cpp
watchman/cmds/log.cpp
watchman/cmds/query.cpp
watchman/cmds/since.cpp
watchman/cmds/state.cpp
watchman/cmds/subscribe.cpp
watchman/cmds/trigger.cpp
watchman/cmds/watch.cpp
watchman/root/ageout.cpp
watchman/root/dir.cpp
watchman/root/file.cpp
watchman/root/init.cpp
watchman/root/iothread.cpp
watchman/root/notifythread.cpp
# root/poison.cpp (in liberr)
watchman/root/reap.cpp
watchman/root/resolve.cpp
watchman/root/sync.cpp
watchman/root/threading.cpp
# root/warnerr.cpp (in liberr)
watchman/root/watchlist.cpp
watchman/saved_state/LocalSavedStateInterface.cpp
watchman/saved_state/SavedStateFactory.cpp
watchman/saved_state/SavedStateInterface.cpp
watchman/scm/Git.cpp
watchman/scm/Mercurial.cpp
watchman/scm/SCM.cpp
watchman/telemetry/LogEvent.cpp
watchman/telemetry/WatchmanStats.cpp
watchman/telemetry/WatchmanStructuredLogger.cpp
watchman/thirdparty/getopt/GetOpt.cpp
watchman/watcher/Watcher.cpp
watchman/watcher/WatcherRegistry.cpp
watchman/watcher/fsevents.cpp
watchman/watcher/inotify.cpp
watchman/watcher/kqueue.cpp
watchman/watcher/portfs.cpp
watchman/watcher/kqueue_and_fsevents.cpp
watchman/watcher/win32.cpp
)

if (ENABLE_EDEN_SUPPORT)
  # We currently only support talking to eden on posix systems
  list(APPEND watchman_sources watchman/watcher/eden.cpp)
endif()

add_executable(watchman ${watchman_sources})
set_target_properties(watchman PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_link_libraries(
  watchman
  log
  string
  err
  jansson
  wildmatch
  third_party_deps
)

if (WIN32)
  add_subdirectory(watchman/thirdparty/deelevate_binding)
  target_link_libraries(
    watchman
    libdeelevate
  )
  install_rust_executable(deelevate)
endif()

if (ENABLE_EDEN_SUPPORT)
  target_link_libraries(
    watchman
    streamingeden_thrift
  )
endif()

install(TARGETS watchman RUNTIME DESTINATION bin)

add_subdirectory(watchman/cli)

set(tests)
# Helper function to define a unit test executable
function(t_test NAME)
  add_executable(${NAME}.t ${ARGN})
  target_link_libraries(
    ${NAME}.t
    testsupport wildmatch third_party_deps
    ${LIBGMOCK_LIBRARIES}
  )
  target_compile_definitions(${NAME}.t
    PUBLIC WATCHMAN_TEST_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\")
  gtest_discover_tests(${NAME}.t DISCOVERY_TIMEOUT 60)
  list(APPEND tests ${NAME}.t)
endfunction()

# The `check` target runs the unit tests
add_custom_target(check
  DEPENDS ${tests}
  COMMAND ${CMAKE_CTEST_COMMAND})

if(Python3_Interpreter_FOUND)
  if (WIN32)
    add_executable(susres watchman/winbuild/susres.cpp)
    target_link_libraries(susres third_party_deps)
    add_custom_target(make_susres ALL DEPENDS susres)
  endif()

  # The `integration` target runs the unit tests and integration tests
  add_custom_target(integration
    DEPENDS pybuild check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/runtests.py
      --watchman-path ${CMAKE_CURRENT_BINARY_DIR}/watchman
      --pybuild-dir ${CMAKE_CURRENT_BINARY_DIR}/watchman/python
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

t_test(art watchman/test/ArtTest.cpp)
t_test(bser watchman/test/BserTest.cpp)
t_test(cache watchman/test/CacheTest.cpp)
t_test(childproc watchman/test/ChildProcTest.cpp)
t_test(fsdetect watchman/test/FSDetectTest.cpp)
t_test(ignore watchman/test/IgnoreTest.cpp)
# Linking this test needs the targets graph to be cleaned up.
#t_test(inmemoryview watchman/test/InMemoryViewTest.cpp)
t_test(log watchman/test/LogTest.cpp)
t_test(maputil watchman/test/MapUtilTest.cpp)
t_test(pendingcollection watchman/test/PendingCollectionTest.cpp)
# Linking this test needs the targets graph to be cleaned up.
#t_test(perfsample watchman/test/PerfSampleTest.cpp)
t_test(result watchman/test/ResultTest.cpp)
t_test(ringbuffer watchman/test/RingBufferTest.cpp)
t_test(string watchman/test/StringTest.cpp)
t_test(wildmatch watchman/test/WildmatchTest.cpp)
