cmake_minimum_required(VERSION 3.20)

project(deferra VERSION 0.1.0 LANGUAGES CXX)

option(JB_BUILD_SQLITE_DRIVER "Build the SQLite database driver" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)
add_subdirectory(test)
