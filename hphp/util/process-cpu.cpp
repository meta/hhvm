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

#include "hphp/util/process-cpu.h"

#include <folly/system/HardwareConcurrency.h>

#include <cassert>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

int Process::GetCPUCount() {
  return folly::available_concurrency();
}

#ifdef __x86_64__
static __inline void do_cpuid(u_int ax, u_int *p) {
  asm volatile ("cpuid"
                : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3])
                : "0" (ax));
}
#elif defined(_M_X64)
#include <intrin.h>
static ALWAYS_INLINE void do_cpuid(int func, uint32_t* p) {
  __cpuid((int*)p, func);
}
#endif

#ifdef __aarch64__
std::string aarch64ImplementerName(uint8_t implementer) {
  switch (implementer) {
    case 0x00:
      return "Reserved";
    case 0x41:
      return "Arm Limited";
    case 0x42:
      return "Broadcom Corporation";
    case 0x43:
      return "Cavium Inc";
    case 0x44:
      return "Digital Equipment Corporation";
    case 0x46:
      return "Fujitsu Ltd";
    case 0x49:
      return "Infineon Technologies AG";
    case 0x4D:
      return "Motorola or Freescale Semiconductor Inc";
    case 0x4E:
      return "NVIDIA Corporation";
    case 0x50:
      return "Applied Micro Circuits Corporation";
    case 0x51:
      return "Qualcomm Inc";
    case 0x56:
      return "Marvell International Ltd";
    case 0x69:
      return "Intel Corporation";
    case 0xC0:
      return "Ampere Computing";
    default:
      {
        std::ostringstream os;
        os << "Unknown aarch64 0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)implementer;
        return os.str();
      }
  }
}
#endif

std::string Process::GetCPUModel() {
#if defined(__x86_64__) || defined(_M_X64)
  uint32_t regs[4];
  do_cpuid(0, regs);

  const int vendor_size = sizeof(regs[1])*3;
  std::swap(regs[2], regs[3]);
  uint32_t cpu_exthigh = 0;
  if (memcmp(regs + 1, "GenuineIntel", vendor_size) == 0 ||
      memcmp(regs + 1, "AuthenticAMD", vendor_size) == 0) {
    do_cpuid(0x80000000, regs);
    cpu_exthigh = regs[0];
  }

  char cpu_brand[3 * sizeof(regs) + 1];
  char *brand = cpu_brand;
  if (cpu_exthigh >= 0x80000004) {
    for (u_int i = 0x80000002; i < 0x80000005; i++) {
      do_cpuid(i, regs);
      memcpy(brand, regs, sizeof(regs));
      brand += sizeof(regs);
    }
  }
  *brand = '\0';
  assert(brand - cpu_brand < sizeof(cpu_brand));
  return cpu_brand;

#elif defined(__aarch64__)

  uint64_t midr_el1;
  asm volatile ("mrs %0, midr_el1" : "=r" (midr_el1) : : );

  // top 32 bits are reserved
  uint8_t implementer = (midr_el1 >> 24) & 0xFF;
  uint8_t variant = (midr_el1 >> 20) & 0x0F;
  uint16_t partnum = (midr_el1 >> 4) & 0x0FFF;
  uint8_t revision = midr_el1 & 0x0F;

  std::ostringstream os;
  os << aarch64ImplementerName(implementer)
     << " 0x" << std::uppercase << std::hex << std::setw(1) << std::setfill('0') << (int)variant
     << " 0x" << std::uppercase << std::hex << std::setw(3) << std::setfill('0') << (int)partnum
     << " 0x" << std::uppercase << std::hex << std::setw(1) << std::setfill('0') << (int)revision;
  std::string model = os.str();

  return model;

#else
  // On non-x64, fall back to calling uname
  std::string model = "Unknown ";
  struct utsname uname_buf;
  uname(&uname_buf);
  model.append(uname_buf.machine);
  return model;

#endif  // __x86_64__
}

///////////////////////////////////////////////////////////////////////////////
}
