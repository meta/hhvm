include(GNUInstallDirs)

function(auto_sources RETURN_VALUE PATTERN SOURCE_SUBDIRS)

  if ("${SOURCE_SUBDIRS}" STREQUAL "RECURSE")
    SET(PATH ".")
    if (${ARGC} EQUAL 4)
      list(GET ARGV 3 PATH)
    endif ()
  endif()

  if ("${SOURCE_SUBDIRS}" STREQUAL "RECURSE")
    unset(${RETURN_VALUE})
    file(GLOB SUBDIR_FILES "${PATH}/${PATTERN}")
    list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})

    file(GLOB subdirs RELATIVE ${PATH} ${PATH}/*)

    foreach(DIR ${subdirs})
      if (IS_DIRECTORY ${PATH}/${DIR})
        if (NOT "${DIR}" STREQUAL "CMakeFiles")
          file(GLOB_RECURSE SUBDIR_FILES "${PATH}/${DIR}/${PATTERN}")
          list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})
        endif()
      endif()
    endforeach()
  else ()
    file(GLOB ${RETURN_VALUE} "${PATTERN}")

    foreach (PATH ${SOURCE_SUBDIRS})
      file(GLOB SUBDIR_FILES "${PATH}/${PATTERN}")
      list(APPEND ${RETURN_VALUE} ${SUBDIR_FILES})
    endforeach(PATH ${SOURCE_SUBDIRS})
  endif ()

  if (${FILTER_OUT})
    list(REMOVE_ITEM ${RETURN_VALUE} ${FILTER_OUT})
  endif()

  set(${RETURN_VALUE} ${${RETURN_VALUE}} PARENT_SCOPE)
endfunction(auto_sources)

macro(HHVM_SELECT_SOURCES DIR)
  auto_sources(files "*.cpp" "RECURSE" "${DIR}")
  foreach(f ${files})
    if (NOT (${f} MATCHES "(ext_hhvm|/(old-)?tests?/)"))
      list(APPEND CXX_SOURCES ${f})
    endif()
  endforeach()
  auto_sources(files "*.c" "RECURSE" "${DIR}")
  foreach(f ${files})
    if (NOT (${f} MATCHES "(ext_hhvm|/(old-)?tests?/)"))
      list(APPEND C_SOURCES ${f})
    endif()
  endforeach()
  auto_sources(files "*.S" "RECURSE" "${DIR}")
  foreach(f ${files})
    if (NOT (${f} MATCHES "(ext_hhvm|/(old-)?tests?/)"))
      list(APPEND ASM_SOURCES ${f})
    endif()
  endforeach()
  auto_sources(files "*.h" "RECURSE" "${DIR}")
  foreach(f ${files})
    if (NOT (${f} MATCHES "(/(old-)?tests?/)"))
      list(APPEND HEADER_SOURCES ${f})
    endif()
  endforeach()
endmacro(HHVM_SELECT_SOURCES)

function(CONTAINS_STRING FILE SEARCH RETURN_VALUE)
  file(STRINGS ${FILE} FILE_CONTENTS REGEX ".*${SEARCH}.*")
  if (FILE_CONTENTS)
    set(${RETURN_VALUE} True PARENT_SCOPE)
  endif()
endfunction(CONTAINS_STRING)

macro(MYSQL_SOCKET_SEARCH)
  execute_process(COMMAND mysql_config --socket OUTPUT_VARIABLE MYSQL_SOCK OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if (NOT MYSQL_SOCK)
    foreach (i
      /var/run/mysqld/mysqld.sock
      /var/tmp/mysql.sock
      /var/run/mysql/mysql.sock
      /var/lib/mysql/mysql.sock
      /var/mysql/mysql.sock
      /usr/local/mysql/var/mysql.sock
      /Private/tmp/mysql.sock
      /private/tmp/mysql.sock
      /tmp/mysql.sock
      )
      if (EXISTS ${i})
        set(MYSQL_SOCK ${i})
        break()
      endif()
    endforeach()
  endif()
  if (MYSQL_SOCK)
    set(MYSQL_UNIX_SOCK_ADDR ${MYSQL_SOCK} CACHE STRING "Path to MySQL Socket")
  endif()
endmacro()

function(append_systemlib TARGET SOURCE SECTNAME)
  if (APPLE)
    set(${TARGET}_SLIBS ${${TARGET}_SLIBS} -Wl,-sectcreate,__text,${SECTNAME},${SOURCE} PARENT_SCOPE)
  else()
    set(${TARGET}_SLIBS ${${TARGET}_SLIBS} "--add-section" "${SECTNAME}=${SOURCE}" PARENT_SCOPE)
  endif()
endfunction(append_systemlib)

function(embed_sections TARGET DEST)
  add_custom_command(TARGET ${TARGET} PRE_BUILD
    # OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated-compiler-id.txt"
    #        "${CMAKE_CURRENT_SOURCE_DIR}/generated-compiler-timestamp.txt"
    #        "${CMAKE_CURRENT_SOURCE_DIR}/generated-repo-schema-id.txt"
    #        "${CMAKE_CURRENT_SOURCE_DIR}/generated-build-id.txt"
    COMMAND
      "INSTALL_DIR=${CMAKE_BINARY_DIR}/hphp/util"
      "SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
      "${HPHP_HOME}/hphp/hhvm/generate-buildinfo.sh"
    WORKING_DIRECTORY "${HPHP_HOME}/hphp/util"
    COMMENT "Generating Repo Schema ID and Compiler ID"
    VERBATIM)

  if (APPLE)
    set(COMPILER_ID -Wl,-sectcreate,__text,"compiler_id","${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-id.txt")
    set(COMPILER_TIMESTAMP -Wl,-sectcreate,__text,"compiler_ts","${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-timestamp.txt")
    set(REPO_SCHEMA -Wl,-sectcreate,__text,"repo_schema_id","${CMAKE_BINARY_DIR}/hphp/util/generated-repo-schema-id.txt")
    set(BUILD_ID -Wl,-sectcreate,__text,"build_id","${CMAKE_BINARY_DIR}/hphp/util/generated-build-id.txt")
    target_link_libraries(${TARGET} ${${TARGET}_SLIBS} ${COMPILER_ID} ${COMPILER_TIMESTAMP} ${REPO_SCHEMA} ${BUILD_ID})
  else()
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND "objcopy"
      ARGS "--add-section" "compiler_id=${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-id.txt"
           "--add-section" "compiler_ts=${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-timestamp.txt"
           "--add-section" "repo_schema_id=${CMAKE_BINARY_DIR}/hphp/util/generated-repo-schema-id.txt"
           "--add-section" "build_id=${CMAKE_BINARY_DIR}/hphp/util/generated-build-id.txt"
           ${${TARGET}_SLIBS}
           ${DEST}
      DEPENDS "${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-id.txt"
              "${CMAKE_BINARY_DIR}/hphp/util/generated-compiler-timestamp.txt"
              "${CMAKE_BINARY_DIR}/hphp/util/generated-repo-schema-id.txt"
              "${CMAKE_BINARY_DIR}/hphp/util/generated-build-id.txt"
      COMMENT "Embedding php in ${TARGET}")
  endif()
endfunction(embed_sections)

macro(embed_systemlib_byname TARGET SLIB)
  get_filename_component(SLIB_FILENAME ${SLIB} "NAME")

  set(SLIB_EXTNAME "/:${SLIB_FILENAME}")

  string(MD5 SLIB_HASH_NAME ${SLIB_EXTNAME})
  # Some platforms limit section names to 16 characters :(
  string(SUBSTRING ${SLIB_HASH_NAME} 0 12 SLIB_HASH_NAME_SHORT)

  append_systemlib(${TARGET} ${SLIB} "ext.${SLIB_HASH_NAME_SHORT}")
endmacro()

function(embed_all_systemlibs TARGET ROOT DEST)
  add_dependencies(${TARGET} generated_systemlib)

  foreach(SLIB ${EXTENSION_SYSTEMLIB_SOURCES} ${EZC_SYSTEMLIB_SOURCES})
    get_filename_component(SLIB_FILENAME ${SLIB} NAME)
    file(RELATIVE_PATH SLIB_RELATIVE_PATH ${CMAKE_SOURCE_DIR} ${SLIB})
    list(APPEND SLIB_RELATIVE_PATHS ${SLIB_RELATIVE_PATH})
    list(
      APPEND PRECOMPILED_SYSTEMLIB_FILES
      ${CMAKE_CURRENT_BINARY_DIR}/slib/${SLIB_FILENAME}.decls
      ${CMAKE_CURRENT_BINARY_DIR}/slib/${SLIB_FILENAME}.ue
    )
  endforeach()

  add_custom_command(
    TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/slib
    COMMAND $<TARGET_FILE:hhvm> --compile-systemlib --input-dir ${CMAKE_SOURCE_DIR} --output-dir ${CMAKE_CURRENT_BINARY_DIR}/slib  ${SLIB_RELATIVE_PATHS}
    COMMENT "Precompiling systemlib files"
    VERBATIM)

  foreach(PRECOMPILED_SLIB ${PRECOMPILED_SYSTEMLIB_FILES})
    get_filename_component(PRECOMPILED_SLIB_FILENAME ${PRECOMPILED_SLIB} NAME)
    embed_systemlib_byname(${TARGET} ${PRECOMPILED_SLIB})
  endforeach()

  embed_sections(${TARGET} ${DEST})
endfunction(embed_all_systemlibs)

# Custom install function that doesn't relink, instead it uses chrpath to change it, if
# it's available, otherwise, it leaves the chrpath alone
function(HHVM_INSTALL TARGET DEST)
  get_target_property(TY ${TARGET} TYPE)
  if (FOUND_CHRPATH)
    get_target_property(RPATH ${TARGET} INSTALL_RPATH)
    if (NOT RPATH STREQUAL "RPATH-NOTFOUND")
      if (RPATH STREQUAL "")
        install(CODE "execute_process(COMMAND \"${CHRPATH}\" \"-d\" \"$<TARGET_FILE:${TARGET}>\" ERROR_QUIET)")
      else()
        install(CODE "execute_process(COMMAND \"${CHRPATH}\" \"-r\" \"${RPATH}\" \"$<TARGET_FILE:${TARGET}>\" ERROR_QUIET)")
      endif()
    endif()
  endif()
  string(TOUPPER ${DEST} DEST_UPPER)
  install(CODE "FILE(INSTALL DESTINATION \"${CMAKE_INSTALL_FULL_${DEST_UPPER}DIR}\" TYPE ${TY} FILES \"$<TARGET_FILE:${TARGET}>\")")
endfunction(HHVM_INSTALL)

function(HHVM_PUBLIC_HEADERS TARGET)
  install(
    CODE "INCLUDE(\"${HPHP_HOME}/CMake/HPHPFunctions.cmake\")
      HHVM_INSTALL_HEADERS(${TARGET} ${HPHP_HOME}
      \"${CMAKE_INSTALL_FULL_INCLUDEDIR}\" ${ARGN})"
    COMPONENT dev)
endfunction()

function(HHVM_INSTALL_HEADERS TARGET SRCROOT DEST)
  message(STATUS "Installing header files for ${TARGET}")
  foreach(src_rel ${ARGN})
    # Determine the relative directory name so that we can mirror the
    # directory structure in the output
    file(RELATIVE_PATH dest_rel ${SRCROOT} ${src_rel})
    if (IS_ABSOLUTE ${dest_rel})
      message(WARNING "${TARGET}: Header file ${dest_rel} is not inside ${SRCROOT}")
    else()
      string(FIND ${dest_rel} / slash_pos REVERSE)
      if(slash_pos EQUAL -1)
        set(dest_rel)
      else()
        string(SUBSTRING ${dest_rel} 0 ${slash_pos} dest_rel)
      endif()
      file(COPY ${src_rel}
        DESTINATION "$ENV{DESTDIR}${DEST}/${dest_rel}"
        NO_SOURCE_PERMISSIONS)
    endif()
  endforeach()
endfunction()

macro(HHVM_EXT_OPTION EXTNAME PACKAGENAME)
  if (NOT DEFINED EXT_${EXTNAME})
    # Implicit check
    find_package(${PACKAGENAME})
  elseif (EXT_${EXTNAME} STREQUAL "ON")
    # Explicit check
    find_package(${PACKAGENAME} REQUIRED)
  endif()
endmacro()

# Remove all files matching a set of patterns, and,
# optionally, not matching a second set of patterns,
# from a set of lists.
#
# Example:
# This will remove all files in the CPP_SOURCES list
# matching "/test/" or "Test.cpp$", but not matching
# "BobTest.cpp$".
# HHVM_REMOVE_MATCHES_FROM_LISTS(CPP_SOURCES MATCHES "/test/" "Test.cpp$" IGNORE_MATCHES "BobTest.cpp$")
#
# Parameters:
#
# [...]:
# The names of the lists to remove matches from.
#
# [MATCHES ...]:
# The matches to remove from the lists.
#
# [IGNORE_MATCHES ...]:
# The matches not to remove, even if they match
# the main set of matches to remove.
function(HHVM_REMOVE_MATCHES_FROM_LISTS)
  set(LISTS_TO_SEARCH)
  set(MATCHES_TO_REMOVE)
  set(MATCHES_TO_IGNORE)
  set(argumentState 0)
  foreach (arg ${ARGN})
    if ("x${arg}" STREQUAL "xMATCHES")
      set(argumentState 1)
    elseif ("x${arg}" STREQUAL "xIGNORE_MATCHES")
      set(argumentState 2)
    elseif (argumentState EQUAL 0)
      list(APPEND LISTS_TO_SEARCH ${arg})
    elseif (argumentState EQUAL 1)
      list(APPEND MATCHES_TO_REMOVE ${arg})
    elseif (argumentState EQUAL 2)
      list(APPEND MATCHES_TO_IGNORE ${arg})
    else()
      message(FATAL_ERROR "Unknown argument state!")
    endif()
  endforeach()

  foreach (theList ${LISTS_TO_SEARCH})
    foreach (entry ${${theList}})
      foreach (match ${MATCHES_TO_REMOVE})
        if (${entry} MATCHES ${match})
          set(SHOULD_IGNORE OFF)
          foreach (ign ${MATCHES_TO_IGNORE})
            if (${entry} MATCHES ${ign})
              set(SHOULD_IGNORE ON)
              break()
            endif()
          endforeach()

          if (NOT SHOULD_IGNORE)
            list(REMOVE_ITEM ${theList} ${entry})
          endif()
        endif()
      endforeach()
    endforeach()
    set(${theList} ${${theList}} PARENT_SCOPE)
  endforeach()
endfunction()

# Automatically create source_group directives for the sources passed in.
function(auto_source_group rootName rootDir)
  file(TO_CMAKE_PATH "${rootDir}" rootDir)
  string(LENGTH "${rootDir}" rootDirLength)
  set(sourceGroups)
  foreach (fil ${ARGN})
    file(TO_CMAKE_PATH "${fil}" filePath)
    string(FIND "${filePath}" "/" rIdx REVERSE)
    if (rIdx EQUAL -1)
      message(FATAL_ERROR "Unable to locate the final forward slash in '${filePath}'!")
    endif()
    string(SUBSTRING "${filePath}" 0 ${rIdx} filePath)

    string(LENGTH "${filePath}" filePathLength)
    string(FIND "${filePath}" "${rootDir}" rIdx)
    if (rIdx EQUAL 0)
      math(EXPR filePathLength "${filePathLength} - ${rootDirLength}")
      string(SUBSTRING "${filePath}" ${rootDirLength} ${filePathLength} fileGroup)

      string(REPLACE "/" "\\" fileGroup "${fileGroup}")
      set(fileGroup "\\${rootName}${fileGroup}")

      list(FIND sourceGroups "${fileGroup}" rIdx)
      if (rIdx EQUAL -1)
        list(APPEND sourceGroups "${fileGroup}")
        source_group("${fileGroup}" REGULAR_EXPRESSION "${filePath}/[^/.]+(.(tab|yy))?.(c|cc|cpp|h|hpp|ll|php|tcc|y)$")
      endif()
    endif()
  endforeach()
endfunction()

function(parse_version PREFIX VERSION)
  if (NOT ${VERSION} MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?(-.+)?$")
    message(FATAL_ERROR "VERSION must conform to X.Y(.Z)?(-.+)?")
  endif()

  string(FIND ${VERSION} "-" SUFFIX_POS)
  set(SUFFIX "")
  if (SUFFIX_POS)
    string(SUBSTRING ${VERSION} ${SUFFIX_POS} -1 SUFFIX)
    string(SUBSTRING ${VERSION} 0 ${SUFFIX_POS} NUMERIC_VERSION)
  else()
    set(NUMERIC_VERSION ${VERSION})
  endif()

  string(REPLACE "." ";" VERSION_LIST "${NUMERIC_VERSION}")
  list(GET VERSION_LIST 0 MAJOR)
  list(GET VERSION_LIST 1 MINOR)
  list(LENGTH VERSION_LIST VERSION_LIST_LENGTH)
  if (VERSION_LIST_LENGTH GREATER 2)
    list(GET VERSION_LIST 2 PATCH)
  else()
    set(PATCH 0)
  endif()

  set(${PREFIX}MAJOR ${MAJOR} PARENT_SCOPE)
  set(${PREFIX}MINOR ${MINOR} PARENT_SCOPE)
  set(${PREFIX}PATCH ${PATCH} PARENT_SCOPE)
  set(${PREFIX}SUFFIX ${SUFFIX} PARENT_SCOPE)
endfunction()

function(add_object_library libraryName)
  add_library(${libraryName} STATIC ${ARGN})
endfunction()

# Get what might be the objects of the object libraries, if needed.
function(get_object_libraries_objects targetVariable)
  set(OBJECTS)

  set(${targetVariable} ${OBJECTS} PARENT_SCOPE)
endfunction()

# Add the additional link targets for a set of object libraries,
# if needed.
function(link_object_libraries target)
  set(WHOLE_ARCHIVE_LIBS)
  foreach (fil ${ARGN})
    list(APPEND WHOLE_ARCHIVE_LIBS ${fil})
  endforeach()

  set(ANCHOR_SYMS)
  if (APPLE)
    set(ANCHOR_SYMS
      -Wl,-pagezero_size,0x00001000
      # Set the .text.keep section to be executable.
      -Wl,-segprot,.text,rx,rx)
    foreach(lib ${WHOLE_ARCHIVE_LIBS})
      # It's important to use -Xlinker and not -Wl here: ${lib} needs to be its
      # own option on the command line, since target_link_libraries will expand it
      # from its logical name here into the full .a path. (Eww.)
      list(APPEND ANCHOR_SYMS -Xlinker -force_load -Xlinker ${lib})
    endforeach()
  else()
    set(ANCHOR_SYMS -Wl,--whole-archive ${WHOLE_ARCHIVE_LIBS} -Wl,--no-whole-archive)
  endif()

  target_link_libraries(${target} ${ANCHOR_SYMS})
endfunction()

# This should be called for object libraries, rather than calling
# hphp_link directly.
function(object_library_hphp_link target)
  hphp_link(${target})
endfunction()

# If a library needs to be linked in to make GNU ld happy,
# it should be done by calling this.
function(object_library_ld_link_libraries target)
  if (${ARGC})
    target_link_libraries(${target} ${ARGN})
  endif()
endfunction()

set(
  HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX
  ""
  CACHE
  STRING
  "URL prefix containing cache of third-party dependencies"
)
set(
  HHVM_THIRD_PARTY_SOURCE_CACHE_SUFFIX
  ""
  CACHE
  STRING
  "URL suffix for third-party dependency cache"
)
set(
  HHVM_THIRD_PARTY_SOURCE_ONLY_USE_CACHE
  OFF
  CACHE
  BOOL
  "Do not download sources that are not in cache; may cause build to fail."
)
set(
  HHVM_THIRD_PARTY_SOURCE_URL_LIST_OUTPUT
  ""
  CACHE
  STRING
  "Path to a text file to put a list of sources that should be in the cache"
)

# Usage:
#  SET_HHVM_THIRD_PARTY_SOURCE_ARGS(
#    MY_VAR_NAME
#    SOURCE_URL https://example.com/
#    SOURCE_HASH SHA256=deadbeef
#  )
#  ... or ...
#  SET_HHVM_THIRD_PARTY_SOURCE_ARGS(
#    MY_VAR_NAME
#    Linux_URL https://example.com/linux.tar.bz2
#    Linux_HASH SHA256=deadbeef
#    Darwin_URL https://example.com/macos.tar.bz2
#    Darwin_HASH SHA256=deadbeef
#  )
#
macro(SET_HHVM_THIRD_PARTY_SOURCE_ARGS VAR_NAME)
  cmake_parse_arguments(
    # Prefix (FOO becomes _arg_FOO - trailing _ implied)
    _arg
    # No-arg parameter (none):
    ""
    # Single-argument parameters
    "SOURCE_URL;SOURCE_HASH;Linux_URL;Linux_HASH;Darwin_URL;Darwin_HASH;FILENAME_PREFIX"
    # Multi-argument parameters (none)
    ""
    ${ARGN}
  )

  # Try source, but fall back to platform-specific
  if (NOT "${_arg_SOURCE_URL}" STREQUAL "")
    set(_URL "${_arg_SOURCE_URL}")
    set("${VAR_NAME}" URL_HASH "${_arg_SOURCE_HASH}")
    if (NOT "${HHVM_THIRD_PARTY_SOURCE_URL_LIST_OUTPUT}" STREQUAL "")
      FILE(
        APPEND
        "${HHVM_THIRD_PARTY_SOURCE_URL_LIST_OUTPUT}"
        "${_arg_SOURCE_URL}\n"
      )
    endif()
  else()
    set(_URL "${_arg_${CMAKE_HOST_SYSTEM_NAME}_URL}")
    set("${VAR_NAME}" URL_HASH "${_arg_${CMAKE_HOST_SYSTEM_NAME}_HASH}")
    if (NOT "${HHVM_THIRD_PARTY_SOURCE_URL_LIST_OUTPUT}" STREQUAL "")
      FILE(
        APPEND
        "${HHVM_THIRD_PARTY_SOURCE_URL_LIST_OUTPUT}"
        "${_arg_Linux_URL}\n"
        "${_arg_Darwin_URL}\n"
      )
    endif()
  endif()


  get_filename_component("_URL_NAME" "${_URL}" NAME)
  if ("${HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX}" STREQUAL "")
    list(APPEND ${VAR_NAME} URL "${_URL}")
    if (${HHVM_THIRD_PARTY_SOURCE_ONLY_USE_CACHE})
      message(
        FATAL_ERROR
        "HHVM_THIRD_PARTY_ONLY_USE_CACHE is set, but cache is not configured"
      )
    endif()
  else()
    set(
      _CACHE_URL
      "${HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX}${_arg_FILENAME_PREFIX}${_URL_NAME}${HHVM_THIRD_PARTY_SOURCE_CACHE_SUFFIX}"
    )
    set(_CACHE_PATH "")

    # CMake 3.31 rejects mixed URL lists if one entry is local. For local caches,
    # pick a single source at configure time instead of passing both cache and
    # remote URLs to ExternalProject_Add.
    if ("${HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX}" MATCHES "^file://")
      string(REGEX REPLACE "^file://" "" _CACHE_PATH "${_CACHE_URL}")
    elseif ("${HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX}" MATCHES "^/")
      set(_CACHE_PATH "${HHVM_THIRD_PARTY_SOURCE_CACHE_PREFIX}${_arg_FILENAME_PREFIX}${_URL_NAME}${HHVM_THIRD_PARTY_SOURCE_CACHE_SUFFIX}")
      set(_CACHE_URL "file://${_CACHE_PATH}")
    endif()

    if (NOT "${_CACHE_PATH}" STREQUAL "")
      if (EXISTS "${_CACHE_PATH}")
        list(APPEND "${VAR_NAME}" URL "${_CACHE_URL}")
      elseif (${HHVM_THIRD_PARTY_SOURCE_ONLY_USE_CACHE})
        message(
          FATAL_ERROR
          "HHVM_THIRD_PARTY_ONLY_USE_CACHE is set, but cache entry is missing:\n"
          "  ${_CACHE_PATH}"
        )
      else()
        list(APPEND "${VAR_NAME}" URL "${_URL}")
      endif()
    else()
      list(APPEND "${VAR_NAME}" URL "${_CACHE_URL}")
      if (NOT ${HHVM_THIRD_PARTY_SOURCE_ONLY_USE_CACHE})
        list(APPEND "${VAR_NAME}" "${_URL}")
      endif()
    endif()
  endif()
  list(APPEND "${VAR_NAME}" DOWNLOAD_NAME "${_arg_FILENAME_PREFIX}${_URL_NAME}")
  message(STATUS "Download name: ${_arg_FILENAME_PREFIX}${_URL_NAME}")
endmacro()
