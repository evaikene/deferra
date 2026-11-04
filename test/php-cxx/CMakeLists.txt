get_target_property(php_config PhpCxx::PHP PHPCXX_PHP_CONFIG)
get_target_property(phpcxx_source php-cxx SOURCE_DIR)
get_target_property(phpcxx_build php-cxx BINARY_DIR)
execute_process(COMMAND "${php_config}" --php-binary
    RESULT_VARIABLE query_result OUTPUT_VARIABLE php_binary OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT query_result EQUAL 0 OR NOT EXISTS "${php_binary}")
    message(FATAL_ERROR "PHP tests require the CLI selected by ${php_config}; disable BUILD_TESTING for compile-only builds")
endif()

add_executable(php-cxx-build-test consumer/build.cpp)
target_link_libraries(php-cxx-build-test PRIVATE PhpCxx::PhpCxx)
add_test(NAME php-cxx-build-test COMMAND php-cxx-build-test)

add_library(php-cxx-module-test MODULE consumer/module.cpp)
target_link_libraries(php-cxx-module-test PRIVATE PhpCxx::PhpCxx)
set_target_properties(php-cxx-module-test PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
if(APPLE)
    target_link_options(php-cxx-module-test PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
add_test(NAME php-cxx-module-test COMMAND "${php_binary}" -n
    "-dextension=$<TARGET_FILE:php-cxx-module-test>" -r
    "if (!extension_loaded('phpcxx_consumer')) { exit(1); }")

configure_file(build-contract-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/build-contract-test.cmake" @ONLY)
add_test(NAME php-cxx-build-contract-test COMMAND ${CMAKE_COMMAND}
    "-Dtest_configuration=$<CONFIG>" -P "${CMAKE_CURRENT_BINARY_DIR}/build-contract-test.cmake")
set_tests_properties(php-cxx-build-contract-test PROPERTIES TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "Unsupported php-cxx install-test layout:")

configure_file(install-layout-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/install-layout-test.cmake" @ONLY)
add_test(NAME php-cxx-install-layout-test COMMAND ${CMAKE_COMMAND}
    "-Dtest_configuration=$<CONFIG>" -P "${CMAKE_CURRENT_BINARY_DIR}/install-layout-test.cmake")
set_tests_properties(php-cxx-install-layout-test PROPERTIES TIMEOUT 120)
