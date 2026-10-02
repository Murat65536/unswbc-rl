cmake_minimum_required(VERSION 3.18)
project(engine)

include(FetchContent)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TESTING OFF)
set(WITH_OPENSSL OFF CACHE STRING "Build Cap'n Proto with OpenSSL")
set(WITH_ZLIB OFF CACHE STRING "Build Cap'n Proto with zlib")

FetchContent_Declare(
  capnproto
  URL https://github.com/capnproto/capnproto/archive/refs/tags/v1.5.0.tar.gz
  URL_HASH SHA256=d5ebdf858e9885c33d4b3f765006d68bd66e9b002bf4d607ff4317ef9c1aac6a
  SOURCE_SUBDIR c++
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(capnproto)

# SYSTEM includes only hide warnings in headers we include. Cap'n Proto's own
# .c++ still compiles as part of this build, and GCC 15 is noisy about it.
function(unswbc_silence_dir dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(tgt IN LISTS _targets)
        get_target_property(_type ${tgt} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        target_compile_options(${tgt} PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-w>
        )
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS _subdirs)
        unswbc_silence_dir("${sub}")
    endforeach()
endfunction()

unswbc_silence_dir("${capnproto_SOURCE_DIR}/c++")

unset(CAPNP_EXECUTABLE CACHE)
unset(CAPNPC_CXX_EXECUTABLE CACHE)
unset(CAPNP_INCLUDE_DIRECTORY CACHE)

capnp_generate_cpp(REPLAY_SOURCES REPLAY_HEADERS replay.capnp)

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cc")
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS "include/*.h")

add_library(engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS} ${REPLAY_SOURCES})
target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(engine PUBLIC CapnProto::capnp)
