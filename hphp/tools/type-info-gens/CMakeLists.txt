add_executable(gen-type-scanners gen-type-scanners.cpp)

if (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  target_compile_definitions(gen-type-scanners PRIVATE "CLANG_STANDALONE_DEBUG")
endif()

target_link_libraries(gen-type-scanners PRIVATE hphp_debug_parser hphp_util tbb Folly::folly glog::glog)

# hphp_util's alloc.cpp calls mallctl() during static init, but only via a weak
# reference, so it must be linked explicitly or the call jumps to address 0.
if (USE_JEMALLOC)
  target_link_libraries(gen-type-scanners PRIVATE ${JEMALLOC_LIB})
endif()
