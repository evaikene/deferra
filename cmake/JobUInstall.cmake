# Keep the installed header manifest explicit. Public templates sometimes need
# implementation support, but that does not make every private header SDK material.
function(jobu_install_library target module)
    cmake_parse_arguments(INSTALL "" "COMPONENT" "HEADERS;SUPPORT_HEADERS" ${ARGN})
    if(INSTALL_UNPARSED_ARGUMENTS OR NOT INSTALL_HEADERS OR NOT INSTALL_COMPONENT)
        message(FATAL_ERROR "${target}: an SDK component and explicit installed-header manifest are required")
    endif()

    add_library(JobU::${target} ALIAS ${target})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/jobu-include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/jb/${module}>"
    )

    install(TARGETS ${target}
        EXPORT JobU${INSTALL_COMPONENT}Targets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    )
    install(FILES ${INSTALL_HEADERS} ${INSTALL_SUPPORT_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jb/${module}" COMPONENT Development
    )

    # Both example build modes use <jb/...>. Forward to the original headers in
    # the producer build so mixed flat/namespaced includes share one definition.
    # These build-only wrappers and their checkout paths are never installed.
    foreach(header IN LISTS INSTALL_HEADERS INSTALL_SUPPORT_HEADERS)
        set(JOBU_HEADER_SOURCE_CXX "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
        string(REPLACE "\\" "\\\\" JOBU_HEADER_SOURCE_CXX "${JOBU_HEADER_SOURCE_CXX}")
        string(REPLACE "\"" "\\\"" JOBU_HEADER_SOURCE_CXX "${JOBU_HEADER_SOURCE_CXX}")
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/jobu_header_forward.hpp.in"
            "${PROJECT_BINARY_DIR}/jobu-include/jb/${module}/${header}" @ONLY)
    endforeach()

    # Tests use the same bounded manifest to audit installed files and compile
    # public headers without source-tree include paths. Support headers are
    # reached through their owners, rather than parsed in an invalid standalone context.
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_LIBRARIES "${target}")
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_MODULES "${module}")
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_SDK_COMPONENTS "${INSTALL_COMPONENT}")
    foreach(header IN LISTS INSTALL_HEADERS)
        set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_PUBLIC_HEADERS "${module}/${header}")
    endforeach()
    foreach(header IN LISTS INSTALL_HEADERS INSTALL_SUPPORT_HEADERS)
        set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_HEADERS "${module}/${header}")
    endforeach()
endfunction()

function(jobu_install_application target)
    install(TARGETS ${target}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
    )
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_APPLICATIONS "${target}")
endfunction()
