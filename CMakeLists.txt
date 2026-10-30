cmake_minimum_required(VERSION 3.21)
project(ChipTestPlatform VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DM_BUILD_HOST    "Build the Qt6 host application (DeviceManager)" ON)
option(DM_BUILD_PLUGINS "Build plugins under devices/"                  ON)
option(DM_BUILD_TESTS   "Build ABI smoke tests (no Qt required)"        ON)

# All binaries land in build/bin; plugins land in build/devices/<id>/ next to a
# copy of their plugin.json/page.json so the build tree is a runnable layout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(DM_DEVICES_BUILD_DIR ${CMAKE_BINARY_DIR}/devices)
set(DM_DEVICES_SOURCE_DIR ${CMAKE_SOURCE_DIR}/devices)

if(MINGW)
    # Ship a self-contained runtime so plugins and host work outside the MinGW shell.
    add_link_options(-static-libgcc -static-libstdc++)
endif()

add_subdirectory(sdk)

if(DM_BUILD_TESTS)
    enable_testing()            # before the plugins: some of them register their own tests
endif()

if(DM_BUILD_PLUGINS)
    add_subdirectory(devices)
endif()

if(DM_BUILD_TESTS)
    add_subdirectory(sdk/tests)
endif()

if(DM_BUILD_HOST)
    add_subdirectory(DeviceManager)
endif()
