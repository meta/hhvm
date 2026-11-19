#set(CMAKE_BUILD_TYPE Debug)

option(ALWAYS_ASSERT "Enabled asserts in a release build" OFF)
option(STATIC_CXX_LIB "Statically link libstd++ and libgcc." OFF)
option(ENABLE_AARCH64_CRC "Enable the use of CRC instructions" OFF)
option(ENABLE_FASTCGI "Enable the FastCGI interface." ON)

option(EXECUTION_PROFILER "Enable the execution profiler" OFF)

option(USE_JEMALLOC "Use jemalloc" ON)

option(DISABLE_HARDWARE_COUNTERS "Disable hardware counters (for XenU systems)" OFF)

option(ENABLE_TRACE "Enable tracing in release build" OFF)

option(GETDEPS_INSTALL_DIR "Path to a directory containing getdeps-installed artifacts" OFF)

include(CheckCXXSymbolExists)

# Mcrouter uses Folly's AtomicSharedPtr, which only supports libstdc++
# See https://github.com/facebook/hhvm/blob/156a77d5a301033200601ddefb4dcaf26eb1ff9c/third-party/folly/src/folly/concurrency/detail/AtomicSharedPtr-detail.h#L28-L34
check_cxx_symbol_exists(__GLIBCXX__ cstdlib DEFAULT_STDLIB_IS_LIBSTDCXX)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CLANG_FORCE_LIBSTDCXX OR DEFAULT_STDLIB_IS_LIBSTDCXX)
  option(ENABLE_MCROUTER "Build the mcrouter library and extension" ON)
else()
  option(ENABLE_MCROUTER "Build the mcrouter library and extension" OFF)
endif()

option(ENABLE_PROXYGEN_SERVER "Build the Proxygen HTTP server" ON)

option(ENABLE_SPLIT_DWARF "Reduce linker memory usage by putting debugging information into .dwo files" OFF)

option(HPHP_ENABLE_TYPE_SCANNERS "Generate and embed type scanners for precise heap scanning in the GC (requires LLVM)" OFF)

IF (NOT DEFAULT_CONFIG_DIR)
  set(DEFAULT_CONFIG_DIR "/etc/hhvm/" CACHE STRING
    "Default directory to find php.ini")
ENDIF()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HHVM_SYSTEM_PROCESSOR)
option(ENABLE_XED "Enable Intel XED disassembler support" OFF)
set(HHVM_REQUIRE_XED OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   HHVM_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(HHVM_REQUIRE_XED ON)
endif()
set(HHVM_ENABLE_XED ${HHVM_REQUIRE_XED})
if(ENABLE_XED)
  set(HHVM_ENABLE_XED ON)
endif()

option(ENABLE_SYSTEM_LOCALE_ARCHIVE "Use system locale archive as the default LOCALE_ARCHIVE for nix patched glibc." OFF)
