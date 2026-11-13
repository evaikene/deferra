cmake_minimum_required(VERSION 3.20)

# The standalone entry point rebuilds only PHP-dependent code. Native libraries
# come from the matching, relocated SDK produced once by the native build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(JobUPHP VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    include(CTest)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../php-cxx" php-cxx)
endif()

# Discovery variables belong to the helper's directory scope. Reuse its selected
# installation explicitly rather than inspecting an ambient PHP executable.
get_target_property(PHP_CONFIG PhpCxx::PHP PHPCXX_PHP_CONFIG)
phpcxx_find_php()
if(NOT PHP_FOUND)
    message(FATAL_ERROR "${PHP_NOT_FOUND_MESSAGE}")
endif()
if(PHP_ZTS)
    message(FATAL_ERROR "The JobU PHP extension requires NTS PHP; select an NTS PHP_CONFIG. php-cxx alone supports ZTS.")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    find_package(JobU CONFIG REQUIRED COMPONENTS Client)
    set(jobu_php_client JobU::jobu-client)
else()
    set(jobu_php_client jobu-client)
endif()

add_library(jobu-php MODULE module.cpp request.cpp client.cpp client_binding.cpp operation.cpp value_binding.cpp attribute_binding.cpp cli_payload_binding.cpp http_payload_binding.cpp query_binding.cpp result_binding.cpp value_snapshot.cpp resource_state.cpp)
target_compile_features(jobu-php PRIVATE cxx_std_20)
target_link_libraries(jobu-php PRIVATE PhpCxx::PhpCxx ${jobu_php_client})
target_compile_definitions(jobu-php PRIVATE JOBU_PHP_VERSION="${PROJECT_VERSION}")
set_target_properties(jobu-php PROPERTIES
    PREFIX "" OUTPUT_NAME jobu SUFFIX ".so"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON CXX_EXTENSIONS OFF)
if(APPLE)
    target_link_options(jobu-php PRIVATE "LINKER:-undefined,dynamic_lookup" "LINKER:-exported_symbol,_get_module")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Native SDK archives may have public symbols. Only the PHP entry point is
    # exported by the module; its statically linked implementation stays private.
    # libstdc++ templates can explicitly request default visibility even in our
    # own translation units. Keep those symbols private along with SDK archives.
    target_link_options(jobu-php PRIVATE "LINKER:--exclude-libs,ALL"
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
    set_property(TARGET jobu-php APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()

if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../test/jobu-php" test)
endif()
