# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Pass this as CMAKE_PROJECT_INCLUDE to link the release binaries with the
# static archives of system libraries, so they run without those installed.
# It runs after every project(), the fetched dependencies' too. project() sets
# this variable itself, which hides a value given on the command line.
if (APPLE)
  # The SDK's own libraries, such as zlib, have only .tbd stubs.
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a .tbd .dylib)
  # Otherwise CMake puts the directories of link_directories(), such as
  # Homebrew's lib for libevent, in each binary's LC_RPATH.
  set(CMAKE_SKIP_BUILD_RPATH ON)
else ()
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)
  # folly links GNU libatomic for 16-byte atomics and asks find_library for
  # libatomic.so.1 first. GCC keeps libatomic.a in its own directory, where
  # find_library doesn't look, so set the result folly would find.
  if (CMAKE_CXX_COMPILER_ID STREQUAL GNU AND NOT ATOMIC_LIBRARY)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libatomic.a
      OUTPUT_VARIABLE libatomic
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (IS_ABSOLUTE "${libatomic}")
      set(ATOMIC_LIBRARY "${libatomic}" CACHE FILEPATH "GNU libatomic")
    endif ()
  endif ()
endif ()
# Debian's and Homebrew's gflags-config.cmake pick gflags_static by this.
set(GFLAGS_SHARED OFF)
