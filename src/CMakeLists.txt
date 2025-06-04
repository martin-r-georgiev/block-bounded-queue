cmake_minimum_required(VERSION 3.24)

project(bbq_example VERSION 0.1.0)

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


get_filename_component(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} PATH)
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${PARENT_DIR}/build")
set(BINARY_DIR "${PARENT_DIR}/bin")

set(CMAKE_BINARY_DIR ${BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BINARY_DIR})

add_subdirectory(queue)

file(GLOB SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/example/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/example/*.cc")
add_executable(${PROJECT_NAME} ${SOURCE_FILES})
add_dependencies(${PROJECT_NAME} queue)
target_link_libraries(${PROJECT_NAME} PRIVATE queue)
