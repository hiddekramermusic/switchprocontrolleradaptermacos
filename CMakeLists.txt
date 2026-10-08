cmake_minimum_required(VERSION 3.24)

# Must be set before project(). 10.15 is the lowest target that has
# CGPreflightPostEventAccess / CGRequestPostEventAccess; arm64 builds start at
# 11.0 regardless.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")

project(procon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# hidapi, built from source as a static library so the binaries need no
# Homebrew install and follow the deployment target above.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(hidapi
    URL https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.15.0.tar.gz
    URL_HASH SHA256=5d84dec684c27b97b921d2f3b73218cb773cf4ea915caee317ac8fc73cef8136
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(hidapi)

# Fail the build if code uses an API newer than the deployment target.
set(PROCON_WARNINGS -Wall -Wextra -Werror=unguarded-availability)

add_executable(procon_mapper procon_mapper.cpp)
target_compile_options(procon_mapper PRIVATE ${PROCON_WARNINGS})
target_link_libraries(procon_mapper PRIVATE
    hidapi::darwin
    "-framework ApplicationServices"
    "-framework CoreFoundation"
)

add_executable(procon_init procon_init.cpp)
target_compile_options(procon_init PRIVATE ${PROCON_WARNINGS})
target_link_libraries(procon_init PRIVATE hidapi::darwin)
