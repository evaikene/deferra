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

add_library(php-cxx-module-test MODULE consumer/module.cpp consumer/functions.cpp consumer/objects.cpp)
target_link_libraries(php-cxx-module-test PRIVATE PhpCxx::PhpCxx)
set_target_properties(php-cxx-module-test PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
if(APPLE)
    target_link_options(php-cxx-module-test PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
add_test(NAME php-cxx-module-test COMMAND "${php_binary}" -n
    "-dextension=$<TARGET_FILE:php-cxx-module-test>" -r
    "if (!extension_loaded('phpcxx_consumer')) { exit(1); }")
add_test(NAME php-cxx-conversion-consumer-test COMMAND "${php_binary}" -n
    "-dextension=$<TARGET_FILE:php-cxx-module-test>" "${CMAKE_CURRENT_SOURCE_DIR}/consumer/check.php")
add_test(NAME php-cxx-object-consumer-test COMMAND "${php_binary}" -n
    "-dextension=$<TARGET_FILE:php-cxx-module-test>" "${CMAKE_CURRENT_SOURCE_DIR}/consumer/objects-check.php")

configure_file(build-contract-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/build-contract-test.cmake" @ONLY)
add_test(NAME php-cxx-build-contract-test COMMAND ${CMAKE_COMMAND}
    "-Dtest_configuration=$<CONFIG>" -P "${CMAKE_CURRENT_BINARY_DIR}/build-contract-test.cmake")
set_tests_properties(php-cxx-build-contract-test PROPERTIES TIMEOUT 180
    SKIP_REGULAR_EXPRESSION "Unsupported php-cxx install-test layout:")

configure_file(install-layout-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/install-layout-test.cmake" @ONLY)
add_test(NAME php-cxx-install-layout-test COMMAND ${CMAKE_COMMAND}
    "-Dtest_configuration=$<CONFIG>" -P "${CMAKE_CURRENT_BINARY_DIR}/install-layout-test.cmake")
set_tests_properties(php-cxx-install-layout-test PROPERTIES TIMEOUT 120)

# Only the Embed test executables link Embed. The production archive and installed
# consumers keep their existing PHP-module linkage and need no Embed runtime.
get_target_property(php_embed_found php-cxx PHPCXX_EMBED_FOUND)
get_target_property(php_embed_library php-cxx PHPCXX_EMBED_LIBRARY)
if(php_embed_found)
    find_package(Catch2 CONFIG REQUIRED)
    add_executable(php-cxx-ownership-test embed-main.cpp ownership-test.cpp views-test.cpp)
    target_link_libraries(php-cxx-ownership-test PRIVATE PhpCxx::PhpCxx Catch2::Catch2 "${php_embed_library}")
    add_test(NAME php-cxx-ownership-test COMMAND php-cxx-ownership-test)
    set_tests_properties(php-cxx-ownership-test PROPERTIES TIMEOUT 60)
    add_executable(php-cxx-conversion-test embed-main.cpp conversion-test.cpp consumer/functions.cpp)
    target_compile_definitions(php-cxx-conversion-test PRIVATE PHPCXX_CONVERSION_TEST)
    target_link_libraries(php-cxx-conversion-test PRIVATE PhpCxx::PhpCxx Catch2::Catch2 "${php_embed_library}")
    add_test(NAME php-cxx-conversion-test COMMAND php-cxx-conversion-test)
    set_tests_properties(php-cxx-conversion-test PROPERTIES TIMEOUT 60)
    add_executable(php-cxx-object-test object-main.cpp object-test.cpp consumer/objects.cpp)
    target_compile_definitions(php-cxx-object-test PRIVATE
        PHPCXX_OBJECT_CHECK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/consumer/objects-check.php")
    target_link_libraries(php-cxx-object-test PRIVATE PhpCxx::PhpCxx Catch2::Catch2 "${php_embed_library}")
    add_test(NAME php-cxx-object-test COMMAND php-cxx-object-test)
    set_tests_properties(php-cxx-object-test PROPERTIES TIMEOUT 60)
else()
    add_test(NAME php-cxx-ownership-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP Embed is unavailable for ${php_config}; ownership execution requires matching Embed")
    set_tests_properties(php-cxx-ownership-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP Embed")
    add_test(NAME php-cxx-conversion-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP Embed is unavailable for ${php_config}; conversion execution requires matching Embed")
    set_tests_properties(php-cxx-conversion-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP Embed")
    add_test(NAME php-cxx-object-test COMMAND ${CMAKE_COMMAND} -E echo
        "SKIP: PHP Embed is unavailable for ${php_config}; object execution requires matching Embed")
    set_tests_properties(php-cxx-object-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP Embed")
endif()

# Reliability fixtures reuse generated consumer classes and conversions. Keep
# their lifecycle support private to tests, with no new production dependency.
if(php_embed_found)
    add_library(php-cxx-lifecycle-fixture STATIC lifecycle-fixture.cpp consumer/functions.cpp consumer/objects.cpp)
    target_link_libraries(php-cxx-lifecycle-fixture PUBLIC PhpCxx::PhpCxx "${php_embed_library}")
    target_compile_definitions(php-cxx-lifecycle-fixture PRIVATE
        PHPCXX_CONVERSION_CHECK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/consumer/check.php"
        PHPCXX_OBJECT_CHECK_PATH="${CMAKE_CURRENT_SOURCE_DIR}/consumer/objects-check.php")
    find_package(Threads REQUIRED)
    add_executable(php-cxx-lifecycle-test lifecycle-test.cpp)
    target_link_libraries(php-cxx-lifecycle-test PRIVATE php-cxx-lifecycle-fixture Catch2::Catch2WithMain Threads::Threads)
    add_test(NAME php-cxx-lifecycle-test COMMAND php-cxx-lifecycle-test)
    set_tests_properties(php-cxx-lifecycle-test PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "memory leaks detected;Warning:;Fatal error:")

    add_executable(php-cxx-bailout-test bailout-main.cpp)
    target_link_libraries(php-cxx-bailout-test PRIVATE php-cxx-lifecycle-fixture)
    foreach(scenario bailout fatal)
        add_test(NAME php-cxx-${scenario}-isolation-test COMMAND ${CMAKE_COMMAND}
            "-Dexecutable=$<TARGET_FILE:php-cxx-bailout-test>" "-Dscenario=${scenario}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/bailout-test.cmake")
        set_tests_properties(php-cxx-${scenario}-isolation-test PROPERTIES TIMEOUT 30)
    endforeach()
else()
    foreach(scenario lifecycle bailout-isolation fatal-isolation)
        add_test(NAME php-cxx-${scenario}-test COMMAND ${CMAKE_COMMAND} -E echo
            "SKIP: PHP Embed is unavailable for ${php_config}; ${scenario} requires matching Embed")
        set_tests_properties(php-cxx-${scenario}-test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: PHP Embed")
    endforeach()
endif()
