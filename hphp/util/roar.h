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

#include <cstddef>
#include <cstdint>

#ifdef __roar__
constexpr bool use_roar = true;
#else
constexpr bool use_roar = false;
#endif

typedef struct ROARPidMapEntryAPI_ {
  void* address;
  uint64_t size;
  const char* name;
} ROARPidMapEntryAPI;

extern "C"
int __roar_api_write_pid_maps(const ROARPidMapEntryAPI*, size_t);
