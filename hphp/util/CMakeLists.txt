set(CXX_SOURCES)
auto_sources(files "*.cpp" "RECURSE" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CXX_SOURCES ${files})

set(ASM_SOURCES)
auto_sources(files "*.S" "RECURSE" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND ASM_SOURCES ${files})

set(HEADER_SOURCES)
auto_sources(files "*.h" "RECURSE" "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND HEADER_SOURCES ${files})

# Exclude Meta-internal code
HHVM_REMOVE_MATCHES_FROM_LISTS(ASM_SOURCES CXX_SOURCES HEADER_SOURCES
  MATCHES "/facebook/")

# Disable Channeled JSON until we're sure it's going to stick
# sgolemon(2014-02-19)
HHVM_REMOVE_MATCHES_FROM_LISTS(ASM_SOURCES CXX_SOURCES HEADER_SOURCES
  MATCHES "/test/" "channeled-json-")

list(REMOVE_ITEM CXX_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/portability/fnmatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/portability/glob.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/portability/rand_r.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/portability/strfmon.cpp
  )
list(REMOVE_ITEM HEADER_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/portability/fnmatch.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portability/glob.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portability/rand_r.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portability/strfmon.h
)

if(USE_JEMALLOC)
  set(JEMALLOC_DEF_ARG "--jemalloc")
else()
  set(JEMALLOC_DEF_ARG "")
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/jemalloc-def.h
  COMMAND INSTALL_DIR=${CMAKE_CURRENT_BINARY_DIR} bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate-jemalloc-def.sh ignored ${JEMALLOC_DEF_ARG}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate-jemalloc-def.sh
)

list(APPEND HEADER_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/jemalloc-def.h)

HHVM_PUBLIC_HEADERS(util ${HEADER_SOURCES})

find_package(LibNuma)
if (LIBNUMA_INCLUDE_DIRS)
  add_definitions("-DHAVE_NUMA=1")
  include_directories(${LIBNUMA_INCLUDE_DIRS})
endif()

add_library(hphp_util STATIC ${CXX_SOURCES} ${ASM_SOURCES} ${HEADER_SOURCES})
if (HPHP_ENABLE_TYPE_SCANNERS AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  target_compile_definitions(hphp_util PRIVATE "CLANG_STANDALONE_DEBUG")
endif()

include(HHVMRenderConfigSpecification)

HHVM_RENDER_CONFIG_SPECIFICATION(
    hphp_util
    TYPE defs
    OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/configs
)

auto_source_group("hphp_util" "${CMAKE_CURRENT_SOURCE_DIR}"
  ${ASM_SOURCES} ${CXX_SOURCES} ${HEADER_SOURCES})

target_link_libraries(hphp_util brotli Folly::folly zstd magic_enum::magic_enum tbb)
if (LIBNUMA_LIBRARIES)
  target_link_libraries(hphp_util ${LIBNUMA_LIBRARIES})
endif()

# Allow accessing unstable zstd APIs.
target_compile_definitions(hphp_util PUBLIC ZSTD_STATIC_LINKING_ONLY)
