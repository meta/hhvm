install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/oss-repo-mode
  RENAME hhvm-repo-mode
  DESTINATION bin
  COMPONENT dev)

install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/hhvm_wrapper.php
  DESTINATION bin
  COMPONENT dev)

add_subdirectory(hfsort)
add_subdirectory(version)
add_subdirectory(tc-print)
add_subdirectory(gdb)
if (HPHP_ENABLE_TYPE_SCANNERS)
  add_subdirectory(debug-parser)
  add_subdirectory(type-info-gens)
endif()
