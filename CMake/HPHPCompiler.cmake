include_guard(GLOBAL)

# Do this until cmake has a define for ARMv8
execute_process(COMMAND uname -m
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE UNAME_ARCH)
if(UNAME_ARCH STREQUAL "x86_64")
  set(IS_X64 ON)
elseif(UNAME_ARCH STREQUAL "aarch64")
  set(IS_AARCH64 ON)
endif()

function(AARCH64_HAS_CRC HAS_CRC)
  if(NOT AARCH64_TARGET_CPU)
    set(CMAKE_REQUIRED_FLAGS "-mcpu=native")
  endif()
  CHECK_CXX_SOURCE_COMPILES("
    int main() {
      unsigned x;
      unsigned ret;
      __asm__ (\"crc32cx %w0, %w1, %x1\"
      : \"=r\"(ret)
      : \"r\"(x)
     );  return ret;
  }" FOUND_CRC)
  set(HAS_CRC ${FOUND_CRC} PARENT_SCOPE)
endfunction()

# using Clang or GCC
if (HPHP_COMPILER_CLANG OR HPHP_COMPILER_GCC)
  # Warnings to disable by name, -Wno-${name}
  set(DISABLED_NAMED_WARNINGS)
  list(APPEND DISABLED_NAMED_WARNINGS
    "error=array-bounds"
    "error=switch"
    "attributes"
    "deprecated"
    "invalid-offsetof"
    "register"
    "sign-compare"
    "strict-aliasing"
    "unused-function"
    "unused-local-typedefs"
    "unused-result"
    "write-strings"
  )

  # Warnings to disable by name when building C code.
  set(DISABLED_C_NAMED_WARNINGS)
  list(APPEND DISABLED_C_NAMED_WARNINGS
    "missing-field-initializers"
    "sign-compare"
  )

  # General options to pass to both C & C++ compilers
  set(GENERAL_OPTIONS)

  # General options to pass to the C++ compiler
  set(GENERAL_CXX_OPTIONS)
  list(APPEND GENERAL_CXX_OPTIONS
    "fno-omit-frame-pointer"
    "Wall"
    "Werror=format-security"
    "Wno-unused-variable"
    "Wno-unused-value"
    "Wno-comment"
    "Wno-class-memaccess"
    "Wno-address"
    "Wno-error=stringop-overflow"
    "Wno-changes-meaning"
  )

  # Options to pass for debug mode to the C++ compiler
  set(DEBUG_CXX_OPTIONS)

  # Options to pass for release mode to the C++ compiler
  set(RELEASE_CXX_OPTIONS)

  # Suboption of -g in debug mode
  set(GDB_SUBOPTION)

  # Enable GCC/LLVM stack-smashing protection
  if(HPHP_ENABLE_HARDENING)
    list(APPEND GENERAL_OPTIONS
      # Enable stack protection and stack-clash protection.
      # This needs two dashes in the name, so put one here.
      "-param=ssp-buffer-size=4"
      "fstack-protector-strong"
      "fstack-clash-protection"

      # Use hardened equivalents of various glibc functions
      # to guard against buffer overflows.
      "D_FORTIFY_SOURCE=3"

      # https://isisblogs.poly.edu/2011/06/01/relro-relocation-read-only/
      "Wl,-z,relro,-z,now"
      # Mark stack as non-executable.
      "Wl,-z,noexecstack"
      # Separate ELF code into its own segment.
      "Wl,-z,separate-code"
    )

    # Enable control-flow / branch protection.
    if (IS_X64)
      list(APPEND GENERAL_OPTIONS "fcf-protection")
    elseif (IS_AARCH64)
      list(APPEND GENERAL_OPTIONS "mbranch-protection=standard")
    endif()

    # Enable C++ standard library assertions.
    if (CLANG_FORCE_LIBCPP)
      list(APPEND GENERAL_CXX_OPTIONS "D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE")
    else()
      list(APPEND GENERAL_CXX_OPTIONS "D_GLIBCXX_ASSERTIONS")
    endif()
  endif()

  # HHVM doesn't support building as a PIE.
  list(APPEND GENERAL_OPTIONS "no-pie")

  if (IS_X64)
    list(APPEND GENERAL_CXX_OPTIONS "march=x86-64-v3")
    set(CMAKE_ASM_FLAGS  "${CMAKE_ASM_FLAGS} -march=x86-64-v3")
  endif()

  if (HPHP_COMPILER_CLANG) # using Clang
    list(APPEND GENERAL_CXX_OPTIONS
      "Qunused-arguments"
    )
    list(APPEND DISABLED_C_NAMED_WARNINGS
      "unused-command-line-argument"
    )
    list(APPEND DISABLED_NAMED_WARNINGS
      "return-type-c-linkage"
      "unknown-warning-option"
      "unused-command-line-argument"
      "nontrivial-memcall"
      "nullability-completeness"
    )

    if(HPHP_FORCE_LIBCPP)
      list(APPEND GENERAL_CXX_OPTIONS "stdlib=libc++")
    endif()
  else() # using GCC
    list(APPEND DISABLED_NAMED_WARNINGS
      "deprecated-declarations"
      "maybe-uninitialized"
      "bool-compare"
    )
    list(APPEND DISABLED_C_NAMED_WARNINGS
      "maybe-uninitialized"
      "old-style-declaration"
    )
    list(APPEND GENERAL_OPTIONS
      "ffunction-sections"
      "fno-delete-null-pointer-checks"
      "DFOLLY_HAVE_MALLOC_H"
    )
    list(APPEND GENERAL_CXX_OPTIONS
      "fdata-sections"
      "fno-gcse"
      "fno-canonical-system-headers"
      "Wvla"
      "Wno-misleading-indentation"
    )
    list(APPEND RELEASE_CXX_OPTIONS
      "-param max-inline-insns-auto=100"
      "-param early-inlining-insns=200"
      "-param max-early-inliner-iterations=50"
      "-param=inline-unit-growth=200"
      "-param=large-unit-insns=10000"
    )

    # X64
    if(IS_X64)
      list(APPEND GENERAL_CXX_OPTIONS "mcrc32")
    endif()

    # ARM64
    if(IS_AARCH64)
      # Force char type to be signed, which is not the case on aarch64.
      list(APPEND GENERAL_OPTIONS "fsigned-char")

      # If a CPU was specified, build a -mcpu option for the compiler.
      set(AARCH64_TARGET_CPU "" CACHE STRING "CPU to tell gcc to optimize for (-mcpu)")
      if(AARCH64_TARGET_CPU)
        list(APPEND GENERAL_OPTIONS "mcpu=${AARCH64_TARGET_CPU}")
        set(CMAKE_ASM_FLAGS_INIT  "${CMAKE_ASM_FLAGS_INIT} -mcpu=${AARCH64_TARGET_CPU}")
      endif()

      AARCH64_HAS_CRC(HAS_CRC)
      if(NOT ENABLE_AARCH64_CRC)
        if(HAS_CRC)
          message(STATUS "AARCH64 Host supports crc instruction.")
          message(STATUS "Consider adding -DENABLE_AARCH64_CRC")
        endif()
      endif()
    endif()

    if(STATIC_CXX_LIB)
      set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
    endif()

    if (ENABLE_SPLIT_DWARF)
      set(GDB_SUBOPTION "split-dwarf")
    endif()
  endif()

  # No optimizations for debug builds.
  # -Og enables some optimizations, but makes debugging harder by optimizing
  # away some functions and locals. -O0 is more debuggable.
  # -O0-ggdb was reputed to cause gdb to crash (github #4450)
  set(CMAKE_C_FLAGS_DEBUG_INIT            "-O0 -g${GDB_SUBOPTION}")
  set(CMAKE_CXX_FLAGS_DEBUG_INIT          "-O0 -g${GDB_SUBOPTION}")
  set(CMAKE_C_FLAGS_DEBUGOPT_INIT         "-O2 -g${GDB_SUBOPTION}")
  set(CMAKE_CXX_FLAGS_DEBUGOPT_INIT       "-O2 -g${GDB_SUBOPTION}")
  # CMake seeds flags from _INIT only for its builtin configurations, so the
  # custom DebugOpt configuration needs explicit cache entries.
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_FLAGS_DEBUGOPT)
      set(CMAKE_${lang}_FLAGS_DEBUGOPT "${CMAKE_${lang}_FLAGS_DEBUGOPT_INIT}"
          CACHE STRING "Flags used by the ${lang} compiler during DebugOpt builds.")
    endif()
  endforeach()
  set(CMAKE_C_FLAGS_MINSIZEREL_INIT       "-Os")
  set(CMAKE_CXX_FLAGS_MINSIZEREL_INIT     "-Os")
  set(CMAKE_C_FLAGS_RELEASE_INIT          "-O3")
  set(CMAKE_CXX_FLAGS_RELEASE_INIT        "-O3")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT   "-O2 -g${GDB_SUBOPTION}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-O2 -g${GDB_SUBOPTION}")
  set(CMAKE_C_FLAGS_INIT                  "${CMAKE_C_FLAGS_INIT} -W -Werror=implicit-function-declaration")

  mark_as_advanced(CMAKE_C_FLAGS_DEBUGOPT CMAKE_CXX_FLAGS_DEBUGOPT)

  foreach(opt ${DISABLED_NAMED_WARNINGS})
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -Wno-${opt}")
  endforeach()

  foreach(opt ${DISABLED_C_NAMED_WARNINGS})
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -Wno-${opt}")
  endforeach()

  foreach(opt ${GENERAL_OPTIONS})
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -${opt}")
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -${opt}")
  endforeach()

  foreach(opt ${GENERAL_CXX_OPTIONS})
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -${opt}")
  endforeach()

  foreach(opt ${DEBUG_CXX_OPTIONS})
    set(CMAKE_CXX_FLAGS_DEBUG_INIT "${CMAKE_CXX_FLAGS_DEBUG_INIT} -${opt}")
  endforeach()

  foreach(opt ${RELEASE_CXX_OPTIONS})
    set(CMAKE_CXX_FLAGS_RELEASE_INIT "${CMAKE_CXX_FLAGS_RELEASE_INIT} -${opt}")
  endforeach()

  # Some BFD versions overflow AArch64 branch relocations once a link gets into
  # the multi-gigabyte range, which every non-trivial HHVM link does. Prefer
  # lld for all link types rather than patching individual targets.
  if(IS_AARCH64)
    find_program(LLD_EXECUTABLE ld.lld)
    mark_as_advanced(LLD_EXECUTABLE)
    if(LLD_EXECUTABLE)
      foreach(linker_flags
              CMAKE_EXE_LINKER_FLAGS_INIT
              CMAKE_SHARED_LINKER_FLAGS_INIT
              CMAKE_MODULE_LINKER_FLAGS_INIT)
        set(${linker_flags} "${${linker_flags}} -fuse-ld=lld")
      endforeach()
    else()
      message(WARNING
        "ld.lld not found; older BFD linkers may fail with AArch64 branch "
        "relocation overflows.")
    endif()
  endif()
else()
  message("Warning: unknown/unsupported compiler, things may go wrong")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/ThinArchives.cmake")
