option(BUILD_HACK "True if we should build the Hack typechecker." ON)

include(CMakeParseArguments)

if (NOT BUILD_HACK)
  message(STATUS "Skipping hack")
  return()
endif()

message(STATUS "Building hack")

find_package(LZ4)
find_package(LibElf)

# native_libraries: values for `-l` flags
# lib_paths: values for `-L` flags (directories)
# extra_link_opts: opaque options passed to the linker
#
# We need extra_link_opts for:
# - static libraries
# - anything built from third-party: cmake gives us the link flags
unset(extra_include_paths)
unset(extra_native_libraries)
unset(extra_lib_paths)
unset(extra_link_opts)
unset(extra_cc_flags)

# Allows '#include "hphp/path/to/library/"' paths to start from hphp
# project directory  which is consistent with fbmake's include paths.
list(APPEND extra_include_paths ${HPHP_HOME})
list(APPEND extra_cc_flags -pthread)

# Xcode/Ninja generators undefined MAKE
if(NOT MAKE)
  set(MAKE make)
endif()

if ("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
  set(DUNE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_build")
  set(OPAM_STAMP_FILE "_build/opam.stamp")
  set(RUST_FFI_BUILD_ROOT "${DUNE_BUILD_DIR}/rust_ffi")
  set(CARGO_HOME "${DUNE_BUILD_DIR}/cargo_home")
else()
  set(DUNE_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  set(RUST_FFI_BUILD_ROOT "${CMAKE_BINARY_DIR}")
  set(CARGO_HOME "${CMAKE_CURRENT_BINARY_DIR}/cargo_home")
  set(OPAM_STAMP_FILE "opam.stamp")
endif()
set(HACK_BUILD_ROOT "${DUNE_BUILD_DIR}/default")

get_target_property(OPAM_EXECUTABLE opam IMPORTED_LOCATION)

add_custom_command(
  OUTPUT "${OPAM_STAMP_FILE}"
  DEPENDS opam opam_setup.sh
  COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/opam_setup.sh
    "${OPAM_EXECUTABLE}"
    "${DUNE_BUILD_DIR}"
    && cmake -E touch "${OPAM_STAMP_FILE}"
)
add_custom_target(opam_setup DEPENDS "${OPAM_STAMP_FILE}" opam_setup.sh)

if (SKIP_OPAM)
  set(OPAMROOT "~/.opam")
else ()
  set(OPAMROOT "${DUNE_BUILD_DIR}/opam")
endif()

if(LZ4_FOUND)
  list(APPEND extra_include_paths ${LZ4_INCLUDE_DIR})
  get_filename_component(pth ${LZ4_LIBRARY} DIRECTORY)
  list(APPEND extra_lib_paths ${pth})
  list(APPEND extra_native_libraries "lz4")
else()
  get_target_property(LZ4_INCLUDE_DIRS lz4 INTERFACE_INCLUDE_DIRECTORIES)
  list(APPEND extra_include_paths ${LZ4_INCLUDE_DIRS})
  # If LZ4_FOUND is false either we didn't find lz4 or we found it but it's the
  # wrong version.  We can't just add the new path and a native_lib because we
  # can't control the order (and -l won't accept the raw path to the lib).  By
  # doing it this way we specify the path explicitly.
  get_target_property(LZ4_LIBS lz4 INTERFACE_LINK_LIBRARIES)
  list(APPEND extra_link_opts ${LZ4_LIBS})
endif()

get_target_property(ZSTD_INCLUDE_DIRS zstd INTERFACE_INCLUDE_DIRECTORIES)
list(APPEND extra_include_paths ${ZSTD_INCLUDE_DIRS})
get_target_property(ZSTD_LIBS zstd INTERFACE_LINK_LIBRARIES)
list(APPEND extra_link_opts ${ZSTD_LIBS})

list(APPEND extra_include_paths ${LIBSQLITE3_INCLUDE_DIR})
get_filename_component(pth ${LIBSQLITE3_LIBRARY} DIRECTORY)
list(APPEND extra_lib_paths ${pth})
list(APPEND extra_native_libraries "sqlite3")

get_filename_component(RUSTC_BIN_DIR "${RUSTC_EXE}" DIRECTORY)
get_filename_component(CARGO_BIN_DIR "${CARGO_EXE}" DIRECTORY)

function(invoke_dune name target)
  add_custom_target(
      ${name}
      COMMAND
        . "${CMAKE_CURRENT_BINARY_DIR}/dev_env.sh" &&
        opam exec --
        make --makefile=Makefile.dune ${target}
        BYTECODE="${EMIT_OCAML_BYTECODE}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

  add_dependencies(${name} zstd)
  if (NOT SKIP_OPAM)
    add_dependencies(${name} opam_setup)
  endif()
endfunction()

invoke_dune(hack_dune_debug debug)
invoke_dune(hack_dune_test test)
invoke_dune(hack_dune all)

set(INVOKE_CARGO "${CMAKE_SOURCE_DIR}/hphp/hack/scripts/invoke_cargo.sh")

if(DEFINED ENV{HACKDEBUG})
  set(PROFILE "debug")
else()
  set(PROFILE "release")
endif()
set(RUST_FFI_CARGO_TARGET_DIR "${RUST_FFI_BUILD_ROOT}/hphp/hack/rust_ffi")
set(HHVM_RUST_FFI_STATICLIB_NAME "hhvm_ffi_bridge")
set(
  HHVM_RUST_FFI_WORKSPACE_DIR
  "${RUST_FFI_BUILD_ROOT}/hphp/hack/${HHVM_RUST_FFI_STATICLIB_NAME}"
)
set(HHVM_RUST_FFI_CARGO_TOML "${HHVM_RUST_FFI_WORKSPACE_DIR}/Cargo.toml")
set(HHVM_RUST_FFI_LIB_RS "${HHVM_RUST_FFI_WORKSPACE_DIR}/lib.rs")
set(
  HHVM_RUST_FFI_STATICLIB
  "${RUST_FFI_CARGO_TARGET_DIR}/${PROFILE}/${CMAKE_STATIC_LIBRARY_PREFIX}${HHVM_RUST_FFI_STATICLIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
if(DEFINED ENV{HACKDEBUG})
  set(HHVM_RUST_FFI_PROFILE_ARGS)
else()
  set(HHVM_RUST_FFI_PROFILE_ARGS --release)
endif()
file(MAKE_DIRECTORY "${HHVM_RUST_FFI_WORKSPACE_DIR}")

string(CONCAT HHVM_RUST_FFI_CARGO_TOML_CONTENT
  "[package]\n"
  "name = \"${HHVM_RUST_FFI_STATICLIB_NAME}\"\n"
  "version = \"0.0.0\"\n"
  "edition = \"2024\"\n"
  "\n"
  "[lib]\n"
  "path = \"lib.rs\"\n"
  "crate-type = [\"staticlib\"]\n"
  "\n"
  "[dependencies]\n"
  "compiler_ffi = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/ffi_bridge\" }\n"
  "hdf = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/utils/hdf\" }\n"
  "hhvm_hhbc_defs_ffi = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/hhvm_cxx/hhvm_hhbc_defs\" }\n"
  "hhvm_types_ffi = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/hhvm_cxx/hhvm_types\" }\n"
  "packages_ffi = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/packages/ffi_bridge\" }\n"
  "parser_ffi = { path = \"${CMAKE_CURRENT_SOURCE_DIR}/src/parser/ffi_bridge\" }\n"
)
string(CONCAT HHVM_RUST_FFI_LIB_RS_CONTENT
  "#![allow(unused_extern_crates)]\n"
  "\n"
  "// cxx expects a single Rust staticlib when the final "
  "link is done by CMake.\n"
  "extern crate compiler_ffi;\n"
  "extern crate hdf;\n"
  "extern crate hhvm_hhbc_defs_ffi;\n"
  "extern crate hhvm_types_ffi;\n"
  "extern crate packages_ffi;\n"
  "extern crate parser_ffi;\n"
)
file(
  GENERATE
  OUTPUT "${HHVM_RUST_FFI_CARGO_TOML}"
  CONTENT "${HHVM_RUST_FFI_CARGO_TOML_CONTENT}"
)
file(
  GENERATE
  OUTPUT "${HHVM_RUST_FFI_LIB_RS}"
  CONTENT "${HHVM_RUST_FFI_LIB_RS_CONTENT}"
)

set(RUST_OPCODES_DIR "${CMAKE_BINARY_DIR}/hphp/hack/src/hackc")
set(RUST_OPCODES "${RUST_OPCODES_DIR}/opcodes.rs")

set(HHBC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/src/hackc")
set(HHBC_AST_SRCS
  "${HHBC_PREFIX}/hhbc/adata.rs"
  "${HHBC_PREFIX}/hhbc/attribute.rs"
  "${HHBC_PREFIX}/hhbc/body.rs"
  "${HHBC_PREFIX}/hhbc/class.rs"
  "${HHBC_PREFIX}/hhbc/class_alias.rs"
  "${HHBC_PREFIX}/hhbc/coeffects.rs"
  "${HHBC_PREFIX}/hhbc/constant.rs"
  "${HHBC_PREFIX}/hhbc/function.rs"
  "${HHBC_PREFIX}/hhbc/id.rs"
  "${HHBC_PREFIX}/hhbc/instruct.rs"
  "${HHBC_PREFIX}/hhbc/method.rs"
  "${HHBC_PREFIX}/hhbc/module.rs"
  "${HHBC_PREFIX}/hhbc/param.rs"
  "${HHBC_PREFIX}/hhbc/pos.rs"
  "${HHBC_PREFIX}/hhbc/property.rs"
  "${HHBC_PREFIX}/hhbc/symbol_refs.rs"
  "${HHBC_PREFIX}/hhbc/types.rs"
  "${HHBC_PREFIX}/hhbc/type_const.rs"
  "${HHBC_PREFIX}/hhbc/typedef.rs"
  "${HHBC_PREFIX}/hhbc/typed_value.rs"
  "${HHBC_PREFIX}/hhbc/unit.rs"
  "${HHBC_PREFIX}/hhbc/unit_cbindgen.rs"  # Not in a crate.
  "${RUST_OPCODES}"
)

set(NAMING_SPECIAL_NAMES_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/src/naming")
set(NAMING_SPECIAL_NAMES_SRCS
   "${NAMING_SPECIAL_NAMES_PREFIX}/naming_special_names.rs"
   "${NAMING_SPECIAL_NAMES_PREFIX}/naming_special_names_ffi_cbindgen.rs"
)

set(FFI_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/ffi")
set(FFI_SRCS
   "${FFI_PREFIX}/lib.rs"
   "${FFI_PREFIX}/ffi_ffi_cbindgen.rs"
)

add_custom_command(
  OUTPUT ${RUST_OPCODES}
  COMMAND
    ${CMAKE_COMMAND} -E make_directory "${RUST_OPCODES_DIR}" &&
    . "${CMAKE_CURRENT_BINARY_DIR}/dev_env_rust_only.sh" &&
    ${INVOKE_CARGO} dump-opcodes dump-opcodes --exe &&
    ${INVOKE_CARGO} dump-opcodes dump-opcodes --bin dump_opcodes
      -o "${RUST_OPCODES}"
  COMMENT "Generating Rust opcodes for cbindgen"
  DEPENDS "${OPCODE_DATA}"
)

set(HHBC_AST_HEADER "${RUST_FFI_BUILD_ROOT}/hphp/hack/src/hackc/hhbc-unit.h")
set(FFI_HEADER "${RUST_FFI_BUILD_ROOT}/hphp/hack/src/hackc/ffi.h")
set(
  NAMING_SPECIAL_NAMES_HEADER
  "${RUST_FFI_BUILD_ROOT}/hphp/hack/src/naming/naming-special-names.h"
)
set(
  TYPE_CONSTRAINT_HEADER
  "${CMAKE_SOURCE_DIR}/hphp/runtime/vm/type-constraint-flags.h"
)
set(FCALL_HEADER "${CMAKE_SOURCE_DIR}/hphp/runtime/vm/fcall-args-flags.h")
set(HHBC_HEADER "${CMAKE_SOURCE_DIR}/hphp/runtime/vm/hhbc-shared.h")
set(
  ITER_ARGS_FLAGS_HEADER
  "${CMAKE_SOURCE_DIR}/hphp/runtime/vm/iter-args-flags.h"
)
set(ATTR_HEADER "${CMAKE_SOURCE_DIR}/hphp/runtime/base/attr.h")
set(FFI_EXTRA_HEADER "${CMAKE_SOURCE_DIR}/hphp/hack/src/hackc/ffi/ffi_extra.h")

add_custom_command(
  OUTPUT ${HHBC_AST_HEADER}
  COMMAND
   . "${CMAKE_CURRENT_BINARY_DIR}/dev_env_rust_only.sh" &&
    ${INVOKE_CARGO} ffi_cbindgen ffi_cbindgen --exe &&
    ${INVOKE_CARGO} ffi_cbindgen ffi_cbindgen --bin ffi_cbindgen
      --header "${FFI_HEADER}" --namespaces "HPHP,hackc"
      --includes "${FFI_EXTRA_HEADER}"
      ${FFI_SRCS} &&
    ${INVOKE_CARGO} ffi_cbindgen ffi_cbindgen --bin ffi_cbindgen
      --header "${NAMING_SPECIAL_NAMES_HEADER}" --namespaces "HPHP,hackc,hhbc"
      ${NAMING_SPECIAL_NAMES_SRCS} &&
    ${INVOKE_CARGO} ffi_cbindgen ffi_cbindgen --bin ffi_cbindgen
      --header "${HHBC_AST_HEADER}" --namespaces "HPHP,hackc,hhbc"
      --includes "${FFI_HEADER},${NAMING_SPECIAL_NAMES_HEADER},${TYPE_CONSTRAINT_HEADER},${ATTR_HEADER},${FCALL_HEADER},${HHBC_HEADER},${ITER_ARGS_FLAGS_HEADER}"
      ${HHBC_AST_SRCS}
  DEPENDS opam_setup "${RUST_OPCODES}"
  COMMENT "Generating hhbc-unit.h"
)

add_custom_target(
  "hhbc_ast_cbindgen"
  DEPENDS ${HHBC_AST_HEADER}
)

add_library("hhbc_ast_header" INTERFACE)
add_dependencies("hhbc_ast_header" "hhbc_ast_cbindgen")

add_custom_target(hackc_options)
include(HHVMRenderConfigSpecification)
HHVM_RENDER_CONFIG_SPECIFICATION(
  hackc_options
  TYPE hackc
  OUTPUT_PATH "${CMAKE_BINARY_DIR}/hphp/hack/src/hackc/compile"
)

set(
  FFI_CRATES
  "packages_ffi"
  "parser_ffi"
  "compiler_ffi"
  "hdf"
  "hhvm_types_ffi"
  "hhvm_hhbc_defs_ffi"
)

foreach(NAME ${FFI_CRATES})
  list(APPEND FFI_CXXBRIDGE_SRCS "${RUST_FFI_CARGO_TARGET_DIR}/cxxbridge/${NAME}/${NAME}.rs.cc")
endforeach()

# Propagate include paths and compiler flags from the superproject to cxxbridge interop code.
get_target_property(FOLLY_INCLUDE_DIRECTORIES Folly::folly INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(FMT_INCLUDE_DIRECTORIES fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
string(JOIN " -I" FOLLY_INCLUDE_DIRS_STR ${FOLLY_INCLUDE_DIRECTORIES})
string(JOIN " -I" FMT_INCLUDE_DIRS_STR ${FMT_INCLUDE_DIRECTORIES})
set(FFI_CXXFLAGS "${CMAKE_CXX_FLAGS} -I${FOLLY_INCLUDE_DIRS_STR} -I${FMT_INCLUDE_DIRS_STR}")

add_custom_command(
  OUTPUT ${HHVM_RUST_FFI_STATICLIB} ${FFI_CXXBRIDGE_SRCS}
  COMMAND
    ${CMAKE_COMMAND} -E make_directory "${RUST_FFI_CARGO_TARGET_DIR}"
  COMMAND
    . "${CMAKE_CURRENT_BINARY_DIR}/dev_env_rust_only.sh" &&
    cd "${HHVM_RUST_FFI_WORKSPACE_DIR}" &&
    ${CMAKE_COMMAND} -E env
      CXX=${CMAKE_CXX_COMPILER}
      CXXFLAGS=${FFI_CXXFLAGS}
      "${CARGO_EXE}" build
      --quiet
      --package "${HHVM_RUST_FFI_STATICLIB_NAME}"
      --target-dir "${RUST_FFI_CARGO_TARGET_DIR}"
      ${HHVM_RUST_FFI_PROFILE_ARGS}
  COMMAND
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rename_bundled_sqlite_symbols.sh"
      "${CMAKE_NM}" "${CMAKE_OBJCOPY}" "${HHVM_RUST_FFI_STATICLIB}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS
    hackc_options
    "${OPCODE_DATA}"
    "${HHVM_RUST_FFI_CARGO_TOML}"
    "${HHVM_RUST_FFI_LIB_RS}"
)
add_custom_target(hack_rust_ffi_rust_build DEPENDS ${HHVM_RUST_FFI_STATICLIB})
if (NOT SKIP_OPAM)
  add_dependencies(hack_rust_ffi_rust_build opam_setup)
endif()

# Some consumers expect FFI headers to be in a different place, indulge them.
set(FFI_PACKAGES_NEEDING_SYMLINKS compiler_ffi parser_ffi packages_ffi)
foreach (PACKAGE_NAME ${FFI_PACKAGES_NEEDING_SYMLINKS})
  if ("${PACKAGE_NAME}" STREQUAL "parser_ffi")
    set(SYMLINK_DIR parser)
  elseif ("${PACKAGE_NAME}" STREQUAL "packages_ffi")
    set(SYMLINK_DIR packages)
  else()
    set(SYMLINK_DIR hackc)
  endif()
  set(FFI_HEADER_SYMLINK_DEST ${CMAKE_CURRENT_BINARY_DIR}/src/${SYMLINK_DIR}/ffi_bridge/${PACKAGE_NAME}.rs.h)
  list(APPEND COMPILER_FFI_HEADER_SYMLINKS ${FFI_HEADER_SYMLINK_DEST})
  add_custom_command(
    COMMENT "Creating symlink for ${PACKAGE_NAME}.rs.h to support existing #includes"
    OUTPUT ${FFI_HEADER_SYMLINK_DEST}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/src/${SYMLINK_DIR}/ffi_bridge"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${RUST_FFI_CARGO_TARGET_DIR}/cxxbridge/${PACKAGE_NAME}/${PACKAGE_NAME}.rs.h" "${FFI_HEADER_SYMLINK_DEST}"
    DEPENDS hack_rust_ffi_rust_build
  )
endforeach()
add_custom_target(hack_rust_ffi_symlink DEPENDS ${COMPILER_FFI_HEADER_SYMLINKS})

# Finally create a single C++ static library that cyclically depends on the Rust interop code
# and generated cxxbridge sources.
# This will ultimately be linked into final HHVM executables.
add_library(
    hack_rust_ffi_bridge
    ${FFI_CXXBRIDGE_SRCS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/ffi_bridge/external_decl_provider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/hhvm_cxx/hhvm_types/as-base-ffi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hackc/hhvm_cxx/hhvm_hhbc_defs/as-hhbc-ffi.cpp"
)
add_dependencies(hack_rust_ffi_bridge hack_rust_ffi_symlink)

add_library(hack_rust_ffi_bridge_rust_part STATIC IMPORTED)
add_dependencies(hack_rust_ffi_bridge_rust_part hack_rust_ffi_bridge_rust_build)
set_target_properties(
  hack_rust_ffi_bridge_rust_part
  PROPERTIES
  IMPORTED_LOCATION ${HHVM_RUST_FFI_STATICLIB}
  # Intentionally create a link-time cyclic dependency between the Rust library
  # and the dependent C++ library so that CMake will automatically construct the link line,
  # causing the linker to scan through involved static libraries multiple times.
  IMPORTED_LINK_DEPENDENT_LIBRARIES hack_rust_ffi_bridge
)

target_link_libraries(hack_rust_ffi_bridge PRIVATE Folly::folly hack_rust_ffi_bridge_rust_part)
target_include_directories(hack_rust_ffi_bridge INTERFACE "${RUST_FFI_BUILD_ROOT}")
target_include_directories(hack_rust_ffi_bridge PRIVATE "${RUST_FFI_CARGO_TARGET_DIR}/cxxbridge")

if (NOT LZ4_FOUND)
  add_dependencies(hack_dune lz4)
  add_dependencies(hack_dune_debug lz4)
  add_dependencies(hack_dune_test lz4)
endif()

# Intentionally not using `hack_dune_debug` as it generates output files of a
# different format (bytecode instead of raw executables, which is useful if
# you're working with Hack). Keep it around, but require it to be explicitly
# used.
# hack_dune builds the typechecker (hh_server) via OCaml/dune.
# Excluded from ALL because it requires shipit file transformations
# (internal .ml files don't match OSS .mli stubs).
# The hhvm binary doesn't need hack_dune — it only needs the Rust FFI bridges.
add_custom_target(
  hack
  DEPENDS hack_dune hack_rust_ffi_bridge "${OPCODE_DATA}"
)
add_custom_target(hack_test DEPENDS hack_dune_test "${OPCODE_DATA}")

configure_file(dev_env.sh.in dev_env.sh ESCAPE_QUOTES @ONLY)
configure_file(dev_env_common.sh.in dev_env_common.sh ESCAPE_QUOTES @ONLY)
configure_file(dev_env_rust_only.sh.in dev_env_rust_only.sh ESCAPE_QUOTES @ONLY)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/bin/hh_client
  DESTINATION bin
  COMPONENT dev)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/bin/hh_server
  DESTINATION bin
  COMPONENT dev)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/bin/hackfmt
  DESTINATION bin
  COMPONENT dev)

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/bin/hh_parse
  DESTINATION bin
  COMPONENT dev)
