# Preserve concrete toolchain settings, including custom dependency prefixes,
# when the independent fixtures configure their own projects outside the checkout.
set(JOBU_SDK_CONSUMER_CACHE)
foreach(variable
    CMAKE_TOOLCHAIN_FILE CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_TARGET CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
    CMAKE_SYSROOT CMAKE_FIND_ROOT_PATH CMAKE_FIND_ROOT_PATH_MODE_PACKAGE CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
    CMAKE_FIND_ROOT_PATH_MODE_INCLUDE CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT
    CMAKE_MAKE_PROGRAM CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL
    CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS
    CMAKE_EXE_LINKER_FLAGS_DEBUG CMAKE_EXE_LINKER_FLAGS_RELEASE
    CMAKE_SHARED_LINKER_FLAGS_DEBUG CMAKE_SHARED_LINKER_FLAGS_RELEASE
    fmt_DIR fmt_ROOT SQLite3_DIR SQLite3_ROOT CURL_DIR CURL_ROOT)
    if(DEFINED ${variable} AND NOT "${${variable}}" MATCHES "-NOTFOUND$")
        string(APPEND JOBU_SDK_CONSUMER_CACHE "set(${variable} [==[${${variable}}]==] CACHE STRING \"\" FORCE)\n")
    endif()
endforeach()
configure_file(jobu-sdk-cache.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/jobu-sdk-cache.cmake" @ONLY)
configure_file(jobu-sdk-test.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/jobu-sdk-test.cmake" @ONLY)
add_test(NAME jobu-sdk-test COMMAND ${CMAKE_COMMAND}
    "-Dtest_configuration=$<CONFIG>" -Dtest_name=jobu-sdk-test
    "-Dtest_script=${CMAKE_CURRENT_BINARY_DIR}/jobu-sdk-test.cmake"
    "-Devidence_directory=${PROJECT_BINARY_DIR}/jobu-test-logs/jobu-sdk-test"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/jobu-test-runner.cmake")
set_tests_properties(jobu-sdk-test PROPERTIES TIMEOUT 600
    SKIP_REGULAR_EXPRESSION "Unsupported SDK-test layout:")

# These excluded producer targets supply clangd with the real compile context for
# fixture sources. The SDK gate copies the sources and links installed imports;
# it never builds or borrows these targets to satisfy an external consumer.
add_executable(jobu-sdk-core-probe EXCLUDE_FROM_ALL sdk-consumer/core.cpp sdk-consumer/core_main.cpp)
target_link_libraries(jobu-sdk-core-probe PRIVATE JobU::core)
foreach(component client cli http sqlite)
    if(TARGET JobU::jobu-${component})
        add_executable(jobu-sdk-${component}-probe EXCLUDE_FROM_ALL sdk-consumer/${component}.cpp)
        target_link_libraries(jobu-sdk-${component}-probe PRIVATE JobU::jobu-${component})
    endif()
endforeach()
target_link_libraries(jobu-sdk-http-probe PRIVATE JobU::net-http)
