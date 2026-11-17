cmake_minimum_required(VERSION 3.20)
project(JobUPHPInstallLayout LANGUAGES CXX)

# Exercise the production install rules with a tiny target. Layout checks must
# not rebuild the native SDK or the PHP module for every destination variant.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module.cpp" "int install_layout_fixture() { return 0; }\n")
add_library(layout MODULE "${CMAKE_CURRENT_BINARY_DIR}/module.cpp")
set_target_properties(layout PROPERTIES PREFIX "" OUTPUT_NAME jobu SUFFIX ".so")
include("${JOBU_PHP_INSTALL_MODULE}")
jobu_install_php_extension(layout)
