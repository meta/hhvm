# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Pass this as CMAKE_PROJECT_INCLUDE to link the release binaries with the
# static archives of system libraries, so they run without those installed.
# It runs after every project(), the fetched dependencies' too. project() sets
# this variable itself, which hides a value given on the command line.
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)
# Debian's gflags-config.cmake picks gflags_static by this.
set(GFLAGS_SHARED OFF)
