cmake_minimum_required (VERSION 3.5.1 FATAL_ERROR)

project(ccpm CXX C)

set(CMAKE_CXX_STANDARD 14)
add_compile_options(-g -pedantic -Wall -Werror -Wextra -Wcast-align -Wcast-qual -Wconversion -Weffc++ -Wold-style-cast -Wredundant-decls -Wshadow -Wtype-limits -Wunused-parameter -Wwrite-strings -Wformat=2)
add_compile_options("$<$<CONFIG:Debug>:-O0>")

include(../../../mk/clang-dev-tools.cmake)

# find_library(LIB_JEMALLOC libjemalloc.so HINTS ${CMAKE_INSTALL_PREFIX}/lib)
find_library(LIB_NDCTL libndctl.so HINTS ${CMAKE_INSTALL_PREFIX}/lib)

include_directories(./include)
include_directories(${CMAKE_SOURCE_DIR}/src/components)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/libccpm/include)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/libpmem/include)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/libpmem/common)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/libpmem)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/libadoproto/include)
include_directories(${CMAKE_SOURCE_DIR}/src/lib/EASTL/include)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src")
include_directories("../common/include")
include_directories(${CMAKE_INSTALL_PREFIX}/include) # daxctl

link_directories(${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}) # tbb tbbmalloc
link_directories(${CMAKE_INSTALL_PREFIX}/lib) # tbb tbbmalloc

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fopenmp")
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib:${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")

add_definitions(-DCONFIG_DEBUG) # P{LOG,DEG,INF,WRN,ERR} control

set(CCPM_SOURCES src/area_ctl.cpp src/area_top.cpp src/atomic_word.cpp src/cca.cpp src/ccpm.cpp src/doubt.cpp src/log.cpp src/tracker_log.cpp src/value_tracked.cpp src/container_cc.cpp)
add_library(ccpm SHARED ${CCPM_SOURCES})

target_compile_options(${PROJECT_NAME} PUBLIC "$<$<CONFIG:Debug>:-O0>")
target_link_libraries(${PROJECT_NAME} stdc++fs common numa ${LIB_NDCTL} dl tbb tbbmalloc pmem gcov)

add_subdirectory(unit_test)

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION lib)
install(DIRECTORY "src/" DESTINATION "include/ccpm" FILES_MATCHING PATTERN "./include/ccpm/*")
#install(DIRECTORY "include/ccpm" DESTINATION include FILES_MATCHING PATTERN "*.h*")

# # force early install because others depend on this.  this seems a hack. is there
# # a better way?
# #
# add_custom_command(
#   TARGET ${PROJECT_NAME} POST_BUILD
#   COMMAND mkdir -p ${CMAKE_INSTALL_PREFIX}/include/ccpm
#   COMMAND mkdir -p ${CMAKE_INSTALL_PREFIX}/lib
#   COMMAND cp ${PROJECT_SOURCE_DIR}/src/*.h ${CMAKE_INSTALL_PREFIX}/include/ccpm
#   COMMAND cp -r ${PROJECT_SOURCE_DIR}/src/x86_64 ${CMAKE_INSTALL_PREFIX}/include/ccpm
#   COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/libccpm.so ${CMAKE_INSTALL_PREFIX}/lib
#   )

