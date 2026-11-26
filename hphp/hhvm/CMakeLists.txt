include(HHVMExtensionConfig)

set(CXX_SOURCES)
auto_sources(files "*.cpp" "")
list(APPEND CXX_SOURCES ${files})
list(REMOVE_ITEM CXX_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/process-init.cpp)

add_library(hphp_process_init STATIC process-init.cpp)
hphp_link(hphp_process_init)

get_object_libraries_objects(additionalObjects ${HHVM_WHOLE_ARCHIVE_LIBRARIES})
add_executable(hhvm ${CXX_SOURCES} ${additionalObjects})
target_link_libraries(hhvm ${HHVM_LINK_LIBRARIES} ${EZC_LINK_LIBRARIES} ${HRE_LINK_LIBRARIES})
link_object_libraries(hhvm ${HHVM_WHOLE_ARCHIVE_LIBRARIES})

hphp_link(hhvm)
HHVM_CONFIGURE_TARGET_FOR_EXTENSION_DEPENDENCIES(hhvm)
if (NOT WINDOWS)
  target_link_libraries(
    hhvm ${LIBELF_LIBRARIES} ${LIBDWARF_LIBRARIES} ${ZLIB_LIBRARIES}
  )
endif()

# These mocks need to be added for the rust/ocaml FFI to work properly
target_sources(hhvm PUBLIC ../hack/src/utils/ocaml_ffi_mock/ocaml.c)

if (ENABLE_SPLIT_DWARF)
  message(STATUS "Enabled split DWARF files")
  target_link_libraries(hhvm debug "-Wl,--gdb-index")
endif()

if (HPHP_ENABLE_TYPE_SCANNERS AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  target_compile_options(hhvm PRIVATE "-fstandalone-debug")
endif()

embed_all_systemlibs(hhvm "${CMAKE_CURRENT_BINARY_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/hhvm")

if (HPHP_ENABLE_TYPE_SCANNERS)
  set(GENERATED_TYPE_SCANNERS ${CMAKE_CURRENT_BINARY_DIR}/generated-type-scanners.cpp)
  add_custom_command(
    COMMAND $<TARGET_FILE:gen-type-scanners>
    ARGS --source_file $<TARGET_FILE:hhvm> --output_file ${GENERATED_TYPE_SCANNERS}
    OUTPUT ${GENERATED_TYPE_SCANNERS}
    COMMENT "Generating type scanners"
    DEPENDS gen-type-scanners hhvm
    VERBATIM
  )

  add_library(hphp_type_scanners SHARED ${GENERATED_TYPE_SCANNERS})
  target_link_libraries(hphp_type_scanners PRIVATE Folly::folly_container_f14_hash)
  add_custom_command(
    TARGET hphp_type_scanners POST_BUILD
    # Write to a temporary file and rename it over hhvm instead of modifying
    # hhvm in place: other targets' POST_BUILD steps may be executing hhvm
    # concurrently (e.g. --compile-systemlib), and in-place rewrites of a
    # running executable fail with ETXTBSY. rename(2) is atomic and leaves
    # running processes on the old inode.
    COMMAND objcopy
      --remove-section=type_scanners
      --add-section type_scanners=$<TARGET_FILE:hphp_type_scanners>
      $<TARGET_FILE:hhvm> $<TARGET_FILE:hhvm>.tmp
    COMMAND ${CMAKE_COMMAND} -E rename
      $<TARGET_FILE:hhvm>.tmp $<TARGET_FILE:hhvm>
    COMMENT "Embedding type scanners in HHVM"
    VERBATIM
  )
endif()

if (CMAKE_HOST_UNIX)
  add_custom_command(TARGET hhvm POST_BUILD
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/symlinks.sh
      ${CMAKE_CURRENT_BINARY_DIR}/hhvm ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Creating symlinks for hhvm")
endif()

HHVM_INSTALL(hhvm bin)

auto_sources(files "*.h" "")
HHVM_PUBLIC_HEADERS(hhvm ${files})
