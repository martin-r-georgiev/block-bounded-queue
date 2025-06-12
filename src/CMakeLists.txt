cmake_minimum_required(VERSION 3.24)

project(benchmark_runner VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-fdiagnostics-all-candidates)
add_compile_options(-Wno-interference-size)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "[Debug] Disabling inlining and frame pointer omission for profiling")
    
    add_compile_options(-fno-inline)
    add_compile_options(-fno-omit-frame-pointer)
endif()

include(FetchContent)

# Boost
set(BOOST_VER boost-1.88.0)
set(BOOST_INCLUDE_LIBRARIES thread lockfree context filesystem program_options regex system)
set(BOOST_ENABLE_CMAKE ON)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)

FetchContent_Declare(
  boost
  GIT_REPOSITORY https://github.com/boostorg/boost.git
  GIT_TAG        ${BOOST_VER}
)

if(NOT EXISTS "${FETCHCONTENT_BASE_DIR}/boost-src")
    message(STATUS "Boost library not found. Fetching boost...")
endif()

FetchContent_MakeAvailable(boost)

# Folly
find_package(glog QUIET)
find_package(folly QUIET)

get_filename_component(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PATH)
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${PARENT_DIR}/build")
set(BINARY_DIR "${PARENT_DIR}/bin")

set(CMAKE_BINARY_DIR ${BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BINARY_DIR})

add_subdirectory(queue)

file(GLOB SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*.cc")
add_executable(${PROJECT_NAME} ${SOURCE_FILES})
add_dependencies(${PROJECT_NAME} queue Boost::thread Boost::lockfree)

if(folly_FOUND AND glog_FOUND)
    message(STATUS "Found optional glog (folly dependency).")
    message(STATUS "Found optional folly library. Enabling folly benchmarks.")
    target_link_libraries(${PROJECT_NAME} PRIVATE folly glog::glog)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_FOLLY_BENCHMARKS=1)
else()
    message(STATUS "Folly library not found. Skipping folly benchmarks.")
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_FOLLY_BENCHMARKS=0)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE queue Boost::thread Boost::lockfree)
