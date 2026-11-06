# Standalone module builds include the unchanged independent helper suite.
# Keep a common test root so CI's JUnit gate sees both sets of checks.
get_target_property(jobu_php_source jobu-php SOURCE_DIR)
if(CMAKE_SOURCE_DIR STREQUAL jobu_php_source)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../php-cxx" php-cxx)
endif()

get_target_property(php_binary PhpCxx::PHP PHPCXX_PHP_BINARY)
configure_file(configure-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/configure-test.cmake" @ONLY)
add_test(NAME jobu-php-configure-test COMMAND ${CMAKE_COMMAND}
    -P "${CMAKE_CURRENT_BINARY_DIR}/configure-test.cmake")
set_tests_properties(jobu-php-configure-test PROPERTIES TIMEOUT 60)
add_test(NAME jobu-php-module-test COMMAND ${CMAKE_COMMAND}
    "-Dphp_binary=${php_binary}" "-Dmodule=$<TARGET_FILE:jobu-php>"
    "-Dversion=${PROJECT_VERSION}" -P "${CMAKE_CURRENT_SOURCE_DIR}/module-test.cmake")
foreach(scenario reflection exceptions)
    add_test(NAME jobu-php-${scenario}-test COMMAND "${php_binary}" -n
        "-dextension=$<TARGET_FILE:jobu-php>" "${CMAKE_CURRENT_SOURCE_DIR}/${scenario}.php")
endforeach()
add_test(NAME jobu-php-restart-test COMMAND ${CMAKE_COMMAND}
    "-Dphp_binary=${php_binary}" "-Dmodule=$<TARGET_FILE:jobu-php>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/restart-test.cmake")
set_tests_properties(jobu-php-module-test jobu-php-reflection-test
    jobu-php-exceptions-test jobu-php-restart-test PROPERTIES
    TIMEOUT 30 FAIL_REGULAR_EXPRESSION "Warning:;Fatal error:;memory leaks detected")
