cmake_minimum_required(VERSION 3.20)

# This entry point intentionally avoids the parent project's packages and runtime.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PhpCxx VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    include(CTest)
endif()
include(GNUInstallDirs)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PhpConfig.cmake")
phpcxx_find_php()
if(NOT PHP_FOUND)
    message(FATAL_ERROR "${PHP_NOT_FOUND_MESSAGE}")
endif()
phpcxx_add_php_target()
message(STATUS "php-cxx: PHP ${PHP_VERSION}, module API ${PHP_MODULE_API}, debug=${PHP_DEBUG}, ZTS=${PHP_ZTS}")

add_library(php-cxx STATIC src/build.cpp src/string.cpp src/array.cpp src/value.cpp src/views.cpp)
add_library(PhpCxx::PhpCxx ALIAS php-cxx)
target_compile_features(php-cxx PUBLIC cxx_std_20)
target_include_directories(php-cxx PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(php-cxx PUBLIC PhpCxx::PHP)
set_target_properties(php-cxx PROPERTIES
    EXPORT_NAME PhpCxx
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_EXTENSIONS OFF)
# Tests are a sibling directory in integrated builds. Carry discovery metadata
# across that scope without exporting it or adding any production link dependency.
set_target_properties(php-cxx PROPERTIES
    PHPCXX_EMBED_FOUND "${PHP_EMBED_FOUND}"
    PHPCXX_EMBED_LIBRARY "${PHP_EMBED_LIBRARY}")

include(CMakePackageConfigHelpers)
set(PHPCXX_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/PhpCxx")
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PhpCxxConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhpCxxConfig.cmake"
    INSTALL_DESTINATION "${PHPCXX_INSTALL_CMAKEDIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PhpCxxConfigVersion.cmake"
    VERSION 0.1.0 COMPATIBILITY ExactVersion)
install(TARGETS php-cxx EXPORT PhpCxxTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT PhpCxxDevelopment)
install(FILES include/phpcxx/build.hpp include/phpcxx/zend.hpp
    include/phpcxx/views.hpp include/phpcxx/string.hpp include/phpcxx/array.hpp include/phpcxx/value.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/phpcxx" COMPONENT PhpCxxDevelopment)
install(EXPORT PhpCxxTargets NAMESPACE PhpCxx::
    DESTINATION "${PHPCXX_INSTALL_CMAKEDIR}" COMPONENT PhpCxxDevelopment)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PhpCxxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhpCxxConfigVersion.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/../../cmake/PhpConfig.cmake"
    DESTINATION "${PHPCXX_INSTALL_CMAKEDIR}" COMPONENT PhpCxxDevelopment)

if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../test/php-cxx" "${CMAKE_CURRENT_BINARY_DIR}/test")
endif()
