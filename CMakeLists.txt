cmake_minimum_required(VERSION 3.20)

project(deferra VERSION 0.1.0 LANGUAGES CXX)

# The implemented RPC contract advances independently of the software release.
# Keep these as source-defined values, rather than user-overridable cache options.
set(JOBU_RPC_API_VERSION_MAJOR 1)
set(JOBU_RPC_API_VERSION_MINOR 7)
set(JOBU_RPC_API_VERSION "${JOBU_RPC_API_VERSION_MAJOR}.${JOBU_RPC_API_VERSION_MINOR}")

option(JB_BUILD_SQLITE_DRIVER "Build the SQLite database driver" ON)
option(JB_BUILD_EXAMPLES "Build the JobU client examples" ON)
option(JB_BUILD_PHP_CPP "Build the reusable PHP/C++ library" OFF)
option(JB_BUILD_PHP_EXTENSION "Build the JobU PHP extension" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)
include(cmake/JobUInstall.cmake)
include(cmake/JobUSystemd.cmake)
include(cmake/JobUPaths.cmake)
include(cmake/JobUAssets.cmake)
include(cmake/JobUPackaging.cmake)
jobu_package_build_options()

configure_file(src/jobu_version_priv.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated/jobu_version_priv.hpp" @ONLY)
add_library(jobu-version-private INTERFACE)
target_include_directories(jobu-version-private INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated")

add_subdirectory(src)
include(cmake/JobUSDK.cmake)
jobu_configure_packaging()
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
if(JB_BUILD_EXAMPLES)
    add_subdirectory(examples/jobu-client)
endif()
