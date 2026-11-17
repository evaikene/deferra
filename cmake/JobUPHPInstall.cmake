# Keep installation policy local to the module. Independent helper consumers
# need PHP headers, but do not need a configured PHP INI scan directory.
function(jobu_install_php_extension target)
    set(JB_PHP_EXTENSION_DIR "${PHP_EXTENSION_DIR}" CACHE STRING "Destination for jobu.so (absolute or prefix-relative)")
    if(NOT DEFINED JB_PHP_INI_DIR)
        execute_process(COMMAND "${PHP_CONFIG}" --ini-dir
            RESULT_VARIABLE result OUTPUT_VARIABLE ini_directory ERROR_VARIABLE error
            OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 10)
        if(NOT "${result}" STREQUAL "0")
            message(FATAL_ERROR "${PHP_CONFIG} --ini-dir failed: ${error}; set JB_PHP_INI_DIR explicitly")
        endif()
        set(JB_PHP_INI_DIR "${ini_directory}" CACHE STRING "Destination for jobu.ini (absolute or prefix-relative)")
    endif()

    foreach(directory JB_PHP_EXTENSION_DIR JB_PHP_INI_DIR)
        if("${${directory}}" STREQUAL "" OR "${${directory}}" MATCHES "(^|/)[.][.](/|$)" OR
           "${${directory}}" MATCHES "[;\n\r]")
            message(FATAL_ERROR "${directory} must be one nonempty absolute or prefix-relative directory without '..'")
        endif()
    endforeach()

    # php-config normally returns absolute paths. Require explicit component
    # installation so a normal --prefix install cannot write into the live PHP
    # installation. DESTDIR stages both files without changing their destinations.
    install(TARGETS ${target}
        LIBRARY DESTINATION "${JB_PHP_EXTENSION_DIR}" COMPONENT PhpExtension EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/jobu-php/jobu.ini"
        DESTINATION "${JB_PHP_INI_DIR}" COMPONENT PhpExtension EXCLUDE_FROM_ALL)
endfunction()
