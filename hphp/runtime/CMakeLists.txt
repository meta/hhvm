include(HHVMExtensionConfig)

set(RECURSIVE_SOURCE_SUBDIRS base debugger eval server vm)
foreach (dir ${RECURSIVE_SOURCE_SUBDIRS})
  HHVM_SELECT_SOURCES("${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
endforeach(dir ${RECURSIVE_SOURCE_SUBDIRS})
list(APPEND HEADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/version.h")

# Exclude Meta-internal code
HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES HEADER_SOURCES
  MATCHES "/facebook/")

if(NOT LINUX)
  list(REMOVE_ITEM CXX_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/vm/debug/elfwriter.cpp)
  list(REMOVE_ITEM HEADER_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/vm/debug/elfwriter.h)
endif()

# Exclude ARM JIT files on non-ARM architectures
if(NOT IS_AARCH64)
  HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES HEADER_SOURCES
    MATCHES "-arm\\.")
endif()

if(NOT ENABLE_FASTCGI)
  HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES HEADER_SOURCES
    MATCHES "/fastcgi/")
endif()

if(NOT ENABLE_PROXYGEN_SERVER)
  HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES HEADER_SOURCES
    MATCHES "/proxygen/")
endif()

if(NOT HAVE_CUSTOM_LIBEVENT)
  HHVM_REMOVE_MATCHES_FROM_LISTS(CXX_SOURCES HEADER_SOURCES
    MATCHES "/server/libevent-")
endif()

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ir-opcode-generated.h"
  COMMAND
          "INSTALL_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "./generate-ir-opcodes.sh"
  DEPENDS "${HPHP_HOME}/hphp/doc/ir.specification"
          "${HPHP_HOME}/hphp/doc/generate-ir-opcodes.pl"
          "${HPHP_HOME}/hphp/doc/generate-ir-opcodes.sh"
  WORKING_DIRECTORY "${HPHP_HOME}/hphp/doc"
  COMMENT "Generating HHIR opcode table"
  VERBATIM)

add_object_library(hphp_runtime_static
            ${CXX_SOURCES} ${C_SOURCES} ${ASM_SOURCES} ${HEADER_SOURCES}
            "${CMAKE_CURRENT_BINARY_DIR}/ir-opcode-generated.h")
auto_source_group("hphp_runtime_static" "${CMAKE_CURRENT_SOURCE_DIR}"
  ${ASM_SOURCES} ${C_SOURCES} ${CXX_SOURCES} ${HEADER_SOURCES})
add_dependencies(hphp_runtime_static hack_rust_ffi_bridge hhbc_ast_header)

include(HHVMRenderConfigSpecification)
HHVM_RENDER_CONFIG_SPECIFICATION(
  hphp_runtime_static
  TYPE loader
  OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/base/configs
)

SET_TARGET_PROPERTIES(hphp_runtime_static PROPERTIES OUTPUT_NAME "hphp_runtime")
SET_TARGET_PROPERTIES(hphp_runtime_static PROPERTIES PREFIX "lib")
SET_TARGET_PROPERTIES(hphp_runtime_static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
object_library_hphp_link(hphp_runtime_static)
object_library_ld_link_libraries(hphp_runtime_static hphp_util proxygen::proxygen mvfst::mvfst_transport mvfst::mvfst_client mvfst::mvfst_server mvfst::mvfst_codec_types vixl hphp_zend hphp_system jit_sort hphp_system)
HHVM_CONFIGURE_TARGET_FOR_EXTENSION_DEPENDENCIES(hphp_runtime_static)

if (HPHP_ENABLE_TYPE_SCANNERS AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  target_compile_options(hphp_runtime_static PRIVATE "-fstandalone-debug")
endif()

HHVM_PUBLIC_HEADERS(runtime ${HEADER_SOURCES})
