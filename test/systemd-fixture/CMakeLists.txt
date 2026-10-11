cmake_minimum_required(VERSION 3.20)
project(jobu_systemd_fixture LANGUAGES NONE)

if(NOT DEFINED jobu_source)
    message(FATAL_ERROR "jobu_source is required")
endif()
# Exercise the production asset/install functions without another JobU build or
# dependency discovery. Explicit libdir avoids an architecture lookup in NONE.
set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "Fixture library directory")
include(GNUInstallDirs)
set(PROJECT_SOURCE_DIR "${jobu_source}")
include("${jobu_source}/cmake/JobUSystemd.cmake")
include("${jobu_source}/cmake/JobUPaths.cmake")
include("${jobu_source}/cmake/JobUAssets.cmake")
