# Keep the installed header manifest explicit. Public templates sometimes need
# implementation support, but that does not make every private header SDK material.
function(jobu_install_library target module)
    cmake_parse_arguments(INSTALL "" "" "HEADERS;SUPPORT_HEADERS" ${ARGN})
    if(INSTALL_UNPARSED_ARGUMENTS OR NOT INSTALL_HEADERS)
        message(FATAL_ERROR "${target}: an explicit installed-header manifest is required")
    endif()

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/jb/${module}>"
    )

    install(TARGETS ${target}
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    )
    install(FILES ${INSTALL_HEADERS} ${INSTALL_SUPPORT_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jb/${module}" COMPONENT Development
    )

    # Tests use the same bounded manifest to audit installed files and compile
    # public headers without source-tree include paths. Support headers are
    # reached through their owners, rather than parsed in an invalid standalone context.
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_LIBRARIES "${target}")
    set_property(GLOBAL APPEND PROPERTY JOBU_INSTALL_MODULES "${module}")
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
