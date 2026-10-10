# Package settings do not alter ordinary developer diagnostics. Distributable
# producers opt into path mapping before compiling any application or archive.
function(jobu_package_build_options)
    option(JB_PACKAGE_REMAP_PATHS "Remove producer checkout/build paths from compiled artifacts" OFF)
    set(JB_PACKAGE_SOURCE_REVISION "" CACHE STRING "Source revision override for an exported source tree")
    set(JB_PACKAGE_SOURCE_STATE "" CACHE STRING "Source state override: clean, modified, or unknown")
    if(NOT JB_PACKAGE_REMAP_PATHS)
        return()
    endif()

    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-ffile-prefix-map=${PROJECT_SOURCE_DIR}=." JOBU_HAS_FILE_PREFIX_MAP)
    if(NOT JOBU_HAS_FILE_PREFIX_MAP)
        message(FATAL_ERROR "JB_PACKAGE_REMAP_PATHS needs a compiler supporting -ffile-prefix-map")
    endif()
    add_compile_options("-ffile-prefix-map=${PROJECT_SOURCE_DIR}=."
        "-ffile-prefix-map=${PROJECT_BINARY_DIR}=./build")
endfunction()

function(jobu_package_source_identity)
    # Provenance describes configuration time. Release producers keep the
    # checkout unchanged through compilation and packaging; no refresh occurs.
    set(revision "${JB_PACKAGE_SOURCE_REVISION}")
    set(state "${JB_PACKAGE_SOURCE_STATE}")
    if(state AND NOT state MATCHES "^(clean|modified|unknown)$")
        message(FATAL_ERROR "JB_PACKAGE_SOURCE_STATE must be clean, modified, or unknown")
    endif()
    if(NOT revision)
        # Git is optional and read-only here. In a colocated jj checkout, HEAD
        # identifies the base and modified records that the working tree differs.
        # Exported source trees can supply both fields explicitly.
        find_program(JOBU_PACKAGE_GIT git)
        if(JOBU_PACKAGE_GIT AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
            execute_process(COMMAND "${JOBU_PACKAGE_GIT}" -c "safe.directory=${PROJECT_SOURCE_DIR}"
                rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                RESULT_VARIABLE result OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(result EQUAL 0 AND NOT state)
                execute_process(COMMAND "${JOBU_PACKAGE_GIT}" -c "safe.directory=${PROJECT_SOURCE_DIR}"
                    status --porcelain --untracked-files=normal WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE changes ERROR_QUIET)
                if(result EQUAL 0)
                    if(changes STREQUAL "")
                        set(state clean)
                    else()
                        set(state modified)
                    endif()
                endif()
            elseif(NOT result EQUAL 0)
                set(revision unknown)
            endif()
        endif()
    endif()
    if(NOT revision)
        set(revision unknown)
    endif()
    if(NOT state)
        set(state unknown)
    endif()
    if(NOT revision MATCHES "^([0-9a-fA-F]+|unknown)$")
        message(FATAL_ERROR "Package source revision must be a hexadecimal revision or unknown")
    endif()
    set(JOBU_PACKAGE_SOURCE_REVISION "${revision}" PARENT_SCOPE)
    set(JOBU_PACKAGE_SOURCE_STATE "${state}" PARENT_SCOPE)
endfunction()

function(jobu_configure_packaging)
    jobu_package_source_identity()
    foreach(dependency FMT JSON CURL SQLITE)
        get_property(version GLOBAL PROPERTY JOBU_${dependency}_VERSION)
        if(NOT version)
            set(version "not-built-or-unknown")
        endif()
        set(JOBU_PACKAGE_${dependency}_VERSION "${version}")
    endforeach()

    # Runtime ABI metadata comes from the selected compiler's standard-library
    # headers, rather than assuming its vendor also determines the C++ library.
    set(JOBU_PACKAGE_CXX_RUNTIME unknown)
    file(WRITE "${PROJECT_BINARY_DIR}/jobu-generated/cxx-runtime-probe.cpp" "#include <version>\n")
    separate_arguments(compiler_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${compiler_flags} -std=c++20 -dM -E
        "${PROJECT_BINARY_DIR}/jobu-generated/cxx-runtime-probe.cpp"
        RESULT_VARIABLE runtime_result OUTPUT_VARIABLE macros ERROR_QUIET)
    if(runtime_result EQUAL 0)
        if(macros MATCHES "#define _GLIBCXX_RELEASE ([0-9]+)")
            set(JOBU_PACKAGE_CXX_RUNTIME "libstdc++ release ${CMAKE_MATCH_1}")
            if(macros MATCHES "#define __GLIBCXX__ ([0-9]+)")
                string(APPEND JOBU_PACKAGE_CXX_RUNTIME "; headers ${CMAKE_MATCH_1}")
            endif()
            if(macros MATCHES "#define _GLIBCXX_USE_CXX11_ABI ([01])")
                string(APPEND JOBU_PACKAGE_CXX_RUNTIME "; CXX11 ABI ${CMAKE_MATCH_1}")
            endif()
        elseif(macros MATCHES "#define _LIBCPP_VERSION ([0-9]+)")
            set(JOBU_PACKAGE_CXX_RUNTIME "libc++ ${CMAKE_MATCH_1}")
        endif()
    endif()

    # os-release identifies the concrete producer; it is not a portability or
    # minimum supported-version claim. Cross builds retain an unknown runtime.
    set(JOBU_PACKAGE_PLATFORM "${CMAKE_SYSTEM_NAME}")
    set(JOBU_PACKAGE_OS "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}")
    set(JOBU_PACKAGE_RUNTIME unknown)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
        if(EXISTS /etc/os-release)
            file(STRINGS /etc/os-release os_lines REGEX "^(ID|VERSION_ID)=")
            foreach(line IN LISTS os_lines)
                string(REGEX REPLACE "^[^=]+=\"?([^\"]*)\"?$" "\\1" value "${line}")
                if(line MATCHES "^ID=")
                    set(os_id "${value}")
                else()
                    set(os_version "${value}")
                endif()
            endforeach()
            set(JOBU_PACKAGE_PLATFORM "${CMAKE_SYSTEM_NAME}-${os_id}${os_version}")
            set(JOBU_PACKAGE_OS "${os_id} ${os_version}; kernel ${CMAKE_SYSTEM_VERSION}")
        endif()
        execute_process(COMMAND ldd --version RESULT_VARIABLE result
            OUTPUT_VARIABLE runtime_output ERROR_VARIABLE runtime_error)
        string(REGEX MATCH "[^\r\n]+" JOBU_PACKAGE_RUNTIME "${runtime_output}${runtime_error}")
        # musl prints its version on the next line; retain it without raw paths.
        if(JOBU_PACKAGE_RUNTIME MATCHES "musl")
            string(REGEX MATCH "Version [^\r\n]+" musl_version "${runtime_output}${runtime_error}")
            string(APPEND JOBU_PACKAGE_RUNTIME " ${musl_version}")
        endif()
        if(NOT JOBU_PACKAGE_RUNTIME)
            set(JOBU_PACKAGE_RUNTIME unknown)
        endif()
    endif()
    string(REGEX REPLACE "[^A-Za-z0-9_.-]" "-" JOBU_PACKAGE_PLATFORM "${JOBU_PACKAGE_PLATFORM}")
    set(JOBU_PACKAGE_STEM "JobU-${PROJECT_VERSION}-${JOBU_PACKAGE_PLATFORM}-${CMAKE_SYSTEM_PROCESSOR}")

    foreach(component Runtime Development Documentation)
        string(TOLOWER "${component}" component_name)
        set(JOBU_PACKAGE_COMPONENT "${component}")
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/jobu_build_info.txt.in"
            "${PROJECT_BINARY_DIR}/jobu-generated/build-info-${component_name}.txt" @ONLY)
        install(FILES "${PROJECT_BINARY_DIR}/jobu-generated/build-info-${component_name}.txt"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu/package" COMPONENT ${component})
    endforeach()

    get_property(JOBU_PACKAGE_APPLICATIONS GLOBAL PROPERTY JOBU_INSTALL_APPLICATIONS)
    get_property(JOBU_PACKAGE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/jobu_cpack_check.cmake.in"
        "${PROJECT_BINARY_DIR}/jobu-generated/cpack-check.cmake" @ONLY)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/jobu_cpack_inventory.cmake.in"
        "${PROJECT_BINARY_DIR}/jobu-generated/cpack-inventory.cmake" @ONLY)

    set(CPACK_PACKAGE_NAME JobU)
    set(CPACK_PACKAGE_VENDOR "JobU contributors")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "JobU job scheduler and component SDK")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/evaikene/deferra")
    set(CPACK_PACKAGE_FILE_NAME "${JOBU_PACKAGE_STEM}")
    set(CPACK_GENERATOR TGZ)
    set(CPACK_COMPONENTS_ALL Runtime Development Documentation)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_GROUPING IGNORE)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    # Every archive is relative to one extraction prefix. This staging policy
    # never rewrites the separately compiled absolute operational defaults.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_VERBATIM_VARIABLES YES)
    set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_BINARY_DIR}/jobu-generated/cpack-check.cmake")
    set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_BINARY_DIR}/jobu-generated/cpack-inventory.cmake")
    foreach(component Runtime Development Documentation)
        string(TOLOWER "${component}" component_name)
        string(TOUPPER "${component}" component_upper)
        set(CPACK_ARCHIVE_${component_upper}_FILE_NAME "${JOBU_PACKAGE_STEM}-${component_name}")
    endforeach()
    # Source archives would traverse the checkout instead of the reviewed
    # installation manifests. This phase supplies binary component artifacts.
    set(CPACK_SOURCE_GENERATOR "")
    foreach(generator 7Z CYGWIN RPM TBZ2 TGZ TXZ TZ ZIP)
        set(CPACK_SOURCE_${generator} OFF)
    endforeach()
    include(CPack)
    set(JOBU_PACKAGE_STEM "${JOBU_PACKAGE_STEM}" PARENT_SCOPE)
    set(JOBU_PACKAGE_MULTI_CONFIG "${JOBU_PACKAGE_MULTI_CONFIG}" PARENT_SCOPE)
endfunction()
