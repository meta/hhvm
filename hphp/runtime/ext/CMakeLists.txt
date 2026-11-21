set(CXX_SOURCES)
set(C_SOURCES)
set(ASM_SOURCES)
set(HEADER_SOURCES)
set(PHP_SOURCES)

# Pick up all files in the base ext/ folder unconditionally
auto_sources(files "*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CXX_SOURCES ${files})
auto_sources(files "*.c" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND C_SOURCES ${files})
auto_sources(files "*.S" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND ASM_SOURCES ${files})
auto_sources(files "*.h" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND HEADER_SOURCES ${files})

# Temporary while closure/generator are being worked on
HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES
  MATCHES "ext_(closure|generator)\\.cpp")

set(HRE_LIBARIES)
include(EXTFunctions)
include(HHVMExtensionConfig)

# For each directory under ext/, either pull in standard
# source lists, or let the particular extension make that
# decision on its own
file(GLOB subdirs ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach (dir ${subdirs})
  if (IS_DIRECTORY ${dir})
    if (NOT EXISTS "${dir}/config.cmake")
      HHVM_SELECT_SOURCES(${dir})
    endif()
    # Otherwise the sources are already known.
  endif()
endforeach()
HHVM_EXTENSION_BUILD_SOURCE_LISTS()
set(EXTENSION_SYSTEMLIB_SOURCES ${PHP_SOURCES} CACHE INTERNAL "" FORCE)
set(HRE_LINK_LIBRARIES ${HRE_LIBARIES} CACHE INTERNAL "" FORCE)

add_object_library(hphp_runtime_ext
  ${CXX_SOURCES} ${C_SOURCES} ${ASM_SOURCES} ${HEADER_SOURCES})
auto_source_group("hphp_runtime_ext" "${CMAKE_CURRENT_SOURCE_DIR}"
  ${ASM_SOURCES} ${C_SOURCES} ${CXX_SOURCES} ${HEADER_SOURCES})
target_link_libraries(hphp_runtime_ext hphp_sbcc_writer hphp_runtime_static hphp_system)

if (HPHP_ENABLE_TYPE_SCANNERS AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  target_compile_options(hphp_runtime_ext PRIVATE "-fstandalone-debug")
endif()

object_library_ld_link_libraries(hphp_runtime_ext hphp_zend ${HRE_LINK_LIBRARIES})
object_library_hphp_link(hphp_runtime_ext)
HHVM_CONFIGURE_TARGET_FOR_EXTENSION_DEPENDENCIES(hphp_runtime_ext)
HHVM_PUBLIC_HEADERS(ext ${HEADER_SOURCES})

add_library(hphp_sbcc_writer STATIC
  sbcc/format/sbcc-writer.cpp
)
hphp_link(hphp_sbcc_writer)

add_executable(hphp_sbcc_compiler
  sbcc/build/main.cpp
  sbcc/build/sbcc-compiler.cpp
  sbcc/build/sbcc-file-list.cpp
  sbcc/build/sbcc-local.cpp
)
hphp_link(hphp_sbcc_compiler)
target_link_libraries(hphp_sbcc_compiler hphp_process_init)
link_object_libraries(hphp_sbcc_compiler ${HHVM_WHOLE_ARCHIVE_LIBRARIES})
# tests expect sbcc-build to live under ext/sbcc
set_target_properties(hphp_sbcc_compiler PROPERTIES
  OUTPUT_NAME sbcc-build
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sbcc)
embed_sections(hphp_sbcc_compiler ${CMAKE_CURRENT_BINARY_DIR}/sbcc/sbcc-build)
HHVM_INSTALL(hphp_sbcc_compiler bin)

set_property(
  DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}
  APPEND
  PROPERTY
  COMPILE_DEFINITIONS
  "FOLLY_XLOG_STRIP_PREFIXES=\"${CMAKE_SOURCE_DIR}\""
)
