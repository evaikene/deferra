cmake_minimum_required(VERSION 3.20)

project(deferra VERSION 0.1.0 LANGUAGES CXX)

option(JB_BUILD_SQLITE_DRIVER "Build the SQLite database driver" ON)
option(JB_BUILD_EXAMPLES "Build the JobU client examples" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)
include(cmake/JobUInstall.cmake)
include(cmake/JobUPaths.cmake)
include(cmake/JobUAssets.cmake)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
if(JB_BUILD_EXAMPLES)
    add_subdirectory(examples/jobu-client)
endif()
