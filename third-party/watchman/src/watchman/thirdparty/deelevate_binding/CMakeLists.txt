if(NOT WIN32)
    message( FATAL_ERROR "deelevate cannot be used on platforms other than Windows" )
endif()

find_package(Python COMPONENTS Interpreter)

rust_executable(deelevate BINARY_NAME eledo-pty-bridge)

rust_static_library(rust_deelevate CRATE deelevate)
install_rust_static_library(
  rust_deelevate
  INSTALL_DIR thirdparty
)

add_library(libdeelevate INTERFACE)
target_include_directories(libdeelevate INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
  libdeelevate
  INTERFACE
  rust_deelevate
  userenv.lib
  bcrypt.lib
  ntdll.lib
  synchronization.lib
)

install(
  TARGETS libdeelevate
)
