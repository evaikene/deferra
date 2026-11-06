include_guard(GLOBAL)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# Discovery only compiles against headers. In particular, a container may use
# headers installed on a newer host without being able to execute that host's PHP.
function(phpcxx_find_php)
    set(PHP_FOUND FALSE PARENT_SCOPE)
    find_program(PHP_CONFIG NAMES php-config DOC "php-config from the selected PHP installation")
    if(NOT PHP_CONFIG OR NOT EXISTS "${PHP_CONFIG}")
        set(PHP_NOT_FOUND_MESSAGE
            "PHP development tools were not found; set PHP_CONFIG to an installed /path/to/php-config"
            PARENT_SCOPE)
        return()
    endif()

    foreach(query
        PREFIX:--prefix VERSION:--version VERSION_ID:--vernum INCLUDE_FLAGS:--includes
        INCLUDE_DIR:--include-dir BINARY:--php-binary EXTENSION_DIR:--extension-dir
        LDFLAGS:--ldflags LIBS:--libs SAPIS:--php-sapis)
        string(REPLACE ":" ";" fields "${query}")
        list(GET fields 0 name)
        list(GET fields 1 option)
        execute_process(COMMAND "${PHP_CONFIG}" "${option}"
            RESULT_VARIABLE result OUTPUT_VARIABLE PHP_${name} ERROR_VARIABLE error
            OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 10)
        if(NOT "${result}" STREQUAL "0")
            set(PHP_NOT_FOUND_MESSAGE "${PHP_CONFIG} ${option} failed (${result}): ${error}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    if(NOT PHP_VERSION_ID MATCHES "^[0-9]+$" OR PHP_VERSION_ID LESS 80100)
        set(PHP_NOT_FOUND_MESSAGE "php-cxx requires PHP 8.1 or newer; ${PHP_CONFIG} reports ${PHP_VERSION}"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT PHP_PREFIX OR NOT PHP_VERSION OR NOT PHP_INCLUDE_DIR)
        set(PHP_NOT_FOUND_MESSAGE "${PHP_CONFIG} returned incomplete PHP installation metadata" PARENT_SCOPE)
        return()
    endif()

    separate_arguments(include_flags UNIX_COMMAND "${PHP_INCLUDE_FLAGS}")
    set(PHP_INCLUDE_DIRS)
    foreach(flag IN LISTS include_flags)
        if(NOT flag MATCHES "^-I(.+)$")
            set(PHP_NOT_FOUND_MESSAGE "Unsupported token in ${PHP_CONFIG} --includes: ${flag}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND PHP_INCLUDE_DIRS "${CMAKE_MATCH_1}")
    endforeach()
    if(NOT PHP_INCLUDE_DIRS OR NOT EXISTS "${PHP_INCLUDE_DIR}/Zend/zend_modules.h")
        set(PHP_NOT_FOUND_MESSAGE "${PHP_CONFIG} does not select usable installed PHP/Zend headers" PARENT_SCOPE)
        return()
    endif()

    file(STRINGS "${PHP_INCLUDE_DIR}/Zend/zend_modules.h" module_api
        REGEX "^#define[ \t]+ZEND_MODULE_API_NO[ \t]+[0-9]+")
    if(NOT module_api MATCHES "ZEND_MODULE_API_NO[ \t]+([0-9]+)")
        set(PHP_NOT_FOUND_MESSAGE "Cannot read ZEND_MODULE_API_NO from the selected PHP headers" PARENT_SCOPE)
        return()
    endif()
    set(PHP_MODULE_API "${CMAKE_MATCH_1}")

    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${PHP_INCLUDE_DIRS}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(headers "#include <php.h>\n#include <Zend/zend_modules.h>\n")

    # These checks are deliberately refreshed when PHP_CONFIG changes in an
    # existing build directory; cached results from another ABI are unsafe.
    foreach(probe PHPCXX_HEADERS_VALID PHPCXX_LONG_IS_64 PHPCXX_PHP_DEBUG PHPCXX_PHP_ZTS)
        unset(${probe} CACHE)
    endforeach()
    check_cxx_source_compiles("${headers}
        static_assert(PHP_VERSION_ID == ${PHP_VERSION_ID}, \"php-config/header version mismatch\");
        static_assert(ZEND_MODULE_API_NO == ${PHP_MODULE_API});
        int main() { return 0; }" PHPCXX_HEADERS_VALID)
    if(PHPCXX_HEADERS_VALID)
        check_cxx_source_compiles("${headers}
            static_assert(sizeof(zend_long) == 8); int main() { return 0; }" PHPCXX_LONG_IS_64)
        check_cxx_source_compiles("${headers}
            static_assert(ZEND_DEBUG == 1); int main() { return 0; }" PHPCXX_PHP_DEBUG)
        check_cxx_source_compiles("${headers}
            #ifndef ZTS
            #error NTS installation
            #endif
            int main() { return 0; }" PHPCXX_PHP_ZTS)
    endif()
    cmake_pop_check_state()

    if(NOT PHPCXX_HEADERS_VALID)
        set(PHP_NOT_FOUND_MESSAGE
            "Selected PHP headers cannot compile as C++20 or disagree with ${PHP_CONFIG}; inspect the CMake configure log"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT PHPCXX_LONG_IS_64)
        set(PHP_NOT_FOUND_MESSAGE "php-cxx requires a 64-bit zend_long in the selected PHP headers" PARENT_SCOPE)
        return()
    endif()

    set(PHP_ZEND_LONG_SIZE 8)
    set(PHP_DEBUG 0)
    set(PHP_ZTS 0)
    if(PHPCXX_PHP_DEBUG)
        set(PHP_DEBUG 1)
    endif()
    if(PHPCXX_PHP_ZTS)
        set(PHP_ZTS 1)
    endif()

    # Embed is optional and never becomes a production link dependency. Keep
    # these paths confined to the selected installation for later test harnesses.
    set(PHP_EMBED_INCLUDE_DIR "")
    if(EXISTS "${PHP_INCLUDE_DIR}/sapi/embed/php_embed.h")
        set(PHP_EMBED_INCLUDE_DIR "${PHP_INCLUDE_DIR}/sapi/embed")
    endif()
    unset(PHP_EMBED_LIBRARY)
    unset(PHP_EMBED_LIBRARY CACHE)
    # ZendPHP on Ubuntu installs several ABIs under /usr, each with a versioned
    # CLI and Embed library. Never fall back to another installation's libphp.so
    # when that versioned library is absent. Alpine and source installs use php.
    get_filename_component(php_binary_name "${PHP_BINARY}" NAME)
    set(embed_name php)
    if(php_binary_name MATCHES "^php[0-9]+[.][0-9]+-zend$")
        set(embed_name "${php_binary_name}")
    endif()
    find_library(PHP_EMBED_LIBRARY NAMES "${embed_name}"
        PATHS "${PHP_PREFIX}/lib" "${PHP_PREFIX}/lib64" NO_DEFAULT_PATH)
    set(PHP_EMBED_FOUND FALSE)
    if(PHP_EMBED_INCLUDE_DIR AND PHP_EMBED_LIBRARY)
        set(PHP_EMBED_FOUND TRUE)
    endif()

    foreach(name CONFIG PREFIX VERSION VERSION_ID INCLUDE_DIR INCLUDE_DIRS BINARY EXTENSION_DIR
        LDFLAGS LIBS SAPIS MODULE_API ZEND_LONG_SIZE DEBUG ZTS EMBED_INCLUDE_DIR EMBED_LIBRARY EMBED_FOUND)
        set(PHP_${name} "${PHP_${name}}" PARENT_SCOPE)
    endforeach()
    set(PHP_FOUND TRUE PARENT_SCOPE)
    set(PHP_NOT_FOUND_MESSAGE "" PARENT_SCOPE)
endfunction()

# Keep PHP include paths out of installed export files. Installed consumers
# discover their matching PHP and recreate this imported usage-requirement target.
function(phpcxx_add_php_target)
    if(TARGET PhpCxx::PHP)
        get_target_property(selected_config PhpCxx::PHP PHPCXX_PHP_CONFIG)
        if(NOT selected_config STREQUAL PHP_CONFIG)
            message(FATAL_ERROR "Select one PHP installation per CMake build directory; PhpCxx::PHP already uses ${selected_config}")
        endif()
        return()
    endif()
    add_library(PhpCxx::PHP INTERFACE IMPORTED GLOBAL)
    set_target_properties(PhpCxx::PHP PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PHP_INCLUDE_DIRS}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PHP_INCLUDE_DIRS}"
        PHPCXX_PHP_CONFIG "${PHP_CONFIG}"
        PHPCXX_PHP_BINARY "${PHP_BINARY}")
endfunction()
