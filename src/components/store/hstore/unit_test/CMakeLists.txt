cmake_minimum_required (VERSION 3.5.1 FATAL_ERROR)

project(hstore-tests CXX)

enable_language(CXX C ASM)
set(CMAKE_CXX_STANDARD 14)

find_library(GTEST_LIB NAMES GTEST PATHS /usr/local/lib,/usr/lib)

find_library(PROFILER_LIB NAMES profiler PATHS /usr/local/lib,/usr/local/lib64,/usr/lib,/usr/lib64)
if ("${PROFILER_LIB}" STREQUAL "PROFILER_LIB-NOTFOUND")
set(PROFILER "")
else()
set(PROFILER "profiler")
add_definitions(-DHAS_PROFILER=1)
endif()

add_definitions(-DCONFIG_DEBUG) # P{LOG,DEG,INF,WRN,ERR} control

include_directories(../../../../lib/common/include)
include_directories(../../../)

add_executable(hstore-test1 test1.cpp store_map.cpp)
target_link_libraries(hstore-test1 ${ASAN_LIB} common numa gtest pthread dl)
add_executable(hstore-test1s test1s.cpp store_map.cpp)
target_link_libraries(hstore-test1s ${ASAN_LIB} common numa gtest pthread dl)
add_executable(hstore-test2 test2.cpp store_map.cpp)
target_link_libraries(hstore-test2 ${ASAN_LIB} common numa gtest pthread dl)
add_executable(hstore-test2y test2y.cpp store_map.cpp)
target_link_libraries(hstore-test2y ${ASAN_LIB} common numa gtest pthread dl)
add_executable(hstore-test3 test3.cpp store_map.cpp)
target_link_libraries(hstore-test3 ${ASAN_LIB} common numa gtest pthread dl ${PROFILER})
add_executable(hstore-test4 test4.cpp store_map.cpp)
target_link_libraries(hstore-test4 ${ASAN_LIB} common numa gtest pthread dl ${PROFILER})
