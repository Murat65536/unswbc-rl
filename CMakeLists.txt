cmake_minimum_required(VERSION 3.18)
project(unswbc_rl LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The organisers' engine, patched to the 1.2.3 rules (engine/UPSTREAM.md).
add_subdirectory(engine)

# The shared core: header-only, and also compiled into the bot (C++20).
add_library(core INTERFACE)
target_include_directories(core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)

# The training-side core: what needs the engine's full state.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "core/src/*.cc")
add_library(core_train STATIC ${CORE_SOURCES})
target_link_libraries(core_train PUBLIC engine core)
target_compile_options(core_train PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

find_package(Threads REQUIRED)
target_link_libraries(core_train PUBLIC Threads::Threads)

# Python bindings. `pip install .` builds them through scikit-build-core; a
# plain CMake build drops the module next to this file so `python -m pytest`
# and `python -m rl.train` find it from the repository root.
find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(bccore NB_STATIC core/python/module.cc)
target_link_libraries(bccore PRIVATE core_train)

if(SKBUILD)
    install(TARGETS bccore LIBRARY DESTINATION .)
else()
    set_target_properties(bccore PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
