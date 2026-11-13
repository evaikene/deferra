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
    "-Dversion=${PROJECT_VERSION}" "-Dsystem=${CMAKE_SYSTEM_NAME}" "-Dnm=${CMAKE_NM}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/module-test.cmake")
foreach(scenario reflection exceptions client values attributes cli-payload http-payload queries)
    add_test(NAME jobu-php-${scenario}-test COMMAND "${php_binary}" -n
        "-dextension=$<TARGET_FILE:jobu-php>" "${CMAKE_CURRENT_SOURCE_DIR}/${scenario}.php")
endforeach()
add_test(NAME jobu-php-restart-test COMMAND ${CMAKE_COMMAND}
    "-Dphp_binary=${php_binary}" "-Dmodule=$<TARGET_FILE:jobu-php>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/restart-test.cmake")
set_tests_properties(jobu-php-module-test jobu-php-reflection-test
    jobu-php-exceptions-test jobu-php-client-test jobu-php-values-test jobu-php-attributes-test jobu-php-cli-payload-test jobu-php-http-payload-test jobu-php-queries-test jobu-php-restart-test PROPERTIES
    TIMEOUT 30 FAIL_REGULAR_EXPRESSION "Warning:;Fatal error:;memory leaks detected")

# The test executable embeds the real module hooks plus fixture-only classes.
# No fixture symbols, Embed linkage, or public Client placeholders enter jobu.so.
get_target_property(php_embed_found php-cxx PHPCXX_EMBED_FOUND)
get_target_property(php_embed_library php-cxx PHPCXX_EMBED_LIBRARY)
set(JOBU_PHP_TEST_DAEMON "" CACHE FILEPATH "Already-built matching jobud for PHP lifecycle tests")
if(TARGET jobud)
    set(jobu_php_test_daemon "$<TARGET_FILE:jobud>")
elseif(JOBU_PHP_TEST_DAEMON AND EXISTS "${JOBU_PHP_TEST_DAEMON}")
    set(jobu_php_test_daemon "${JOBU_PHP_TEST_DAEMON}")
endif()

if(php_embed_found AND jobu_php_test_daemon)
    find_package(Catch2 CONFIG REQUIRED)
    add_executable(jobu-php-lifecycle-test lifecycle-test.cpp lifecycle-bindings.cpp bridge-test.cpp resource-bindings.cpp resource-state-test.cpp queue-test.cpp page-bindings.cpp page-test.cpp list-test.cpp history-test.cpp
        "${jobu_php_source}/module.cpp" "${jobu_php_source}/request.cpp"
        "${jobu_php_source}/value_binding.cpp" "${jobu_php_source}/attribute_binding.cpp" "${jobu_php_source}/cli_payload_binding.cpp" "${jobu_php_source}/http_payload_binding.cpp"
        "${jobu_php_source}/query_binding.cpp" "${jobu_php_source}/result_binding.cpp" "${jobu_php_source}/value_snapshot.cpp" "${jobu_php_source}/resource_state.cpp" "${jobu_php_source}/queue_binding.cpp" "${jobu_php_source}/job_binding.cpp" "${jobu_php_source}/run_binding.cpp" "${jobu_php_source}/attempt_binding.cpp" "${jobu_php_source}/history_binding.cpp" "${jobu_php_source}/page_state.cpp" "${jobu_php_source}/page_binding.cpp" "${jobu_php_source}/list_binding.cpp"
        "${jobu_php_source}/client.cpp" "${jobu_php_source}/client_binding.cpp" "${jobu_php_source}/operation.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../support/temporary_directory.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../support/memory_io_device.cpp")
    get_target_property(jobu_php_libraries jobu-php LINK_LIBRARIES)
    target_link_libraries(jobu-php-lifecycle-test PRIVATE ${jobu_php_libraries}
        Catch2::Catch2WithMain "${php_embed_library}")
    target_include_directories(jobu-php-lifecycle-test PRIVATE "${jobu_php_source}" "${CMAKE_CURRENT_LIST_DIR}/..")
    target_compile_definitions(jobu-php-lifecycle-test PRIVATE
        JOBU_PHP_VERSION="${PROJECT_VERSION}"
        JOBUD_EXECUTABLE="${jobu_php_test_daemon}"
        JOBU_PHP_BINARY="${php_binary}"
        JOBU_PHP_MODULE="$<TARGET_FILE:jobu-php>"
        JOBU_PHP_JOB_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/job.php"
        JOBU_PHP_HISTORY_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/history.php"
        JOBU_PHP_LIST_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/list.php"
        JOBU_PHP_PAGE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/page.php"
        JOBU_PHP_QUEUE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/queue.php"
        JOBU_PHP_CLIENT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/client.php"
        JOBU_PHP_LIFECYCLE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/lifecycle.php")
    add_dependencies(jobu-php-lifecycle-test jobu-php)
    if(TARGET jobud)
        add_dependencies(jobu-php-lifecycle-test jobud)
    endif()
    add_test(NAME jobu-php-lifecycle-test COMMAND jobu-php-lifecycle-test "[lifecycle]")
    add_test(NAME jobu-php-bridge-test COMMAND jobu-php-lifecycle-test "[bridge],[recovery]")
    set_tests_properties(jobu-php-lifecycle-test jobu-php-bridge-test PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "memory leaks detected;Warning:;Fatal error:")
else()
    add_test(NAME jobu-php-lifecycle-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP lifecycle tests require matching Embed and JOBU_PHP_TEST_DAEMON (or the jobud target)")
    set_tests_properties(jobu-php-lifecycle-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP lifecycle")
    add_test(NAME jobu-php-bridge-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP bridge tests require matching Embed and JOBU_PHP_TEST_DAEMON (or the jobud target)")
    set_tests_properties(jobu-php-bridge-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP bridge")
endif()

if(TARGET jobu-php-lifecycle-test)
    add_test(NAME jobu-php-page-test COMMAND jobu-php-lifecycle-test "[page]")
    set_tests_properties(jobu-php-page-test PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "memory leaks detected;Warning:;Fatal error:")
else()
    add_test(NAME jobu-php-page-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP Page tests require matching Embed and JOBU_PHP_TEST_DAEMON (or the jobud target)")
    set_tests_properties(jobu-php-page-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP Page")
endif()

if(TARGET jobu-php-lifecycle-test)
    add_test(NAME jobu-php-list-test COMMAND jobu-php-lifecycle-test "[list],[list-cli]")
    set_tests_properties(jobu-php-list-test PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "memory leaks detected;Warning:;Fatal error:")
else()
    add_test(NAME jobu-php-list-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP list tests require matching Embed and JOBU_PHP_TEST_DAEMON (or the jobud target)")
    set_tests_properties(jobu-php-list-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP list")
endif()

if(TARGET jobu-php-lifecycle-test)
    add_test(NAME jobu-php-history-test COMMAND jobu-php-lifecycle-test "[history],[history-cli]")
    set_tests_properties(jobu-php-history-test PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "memory leaks detected;Warning:;Fatal error:")
else()
    add_test(NAME jobu-php-history-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP history tests require matching Embed and JOBU_PHP_TEST_DAEMON (or the jobud target)")
    set_tests_properties(jobu-php-history-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP history")
endif()
