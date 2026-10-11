# Selecting a service profile is explicit. Generic developer/install defaults
# remain available without implying a deployable system service.
set(JB_JOBU_SYSTEMD_PROFILE none CACHE STRING "JobU systemd assets: none, system, or custom")
set_property(CACHE JB_JOBU_SYSTEMD_PROFILE PROPERTY STRINGS none system custom)
if(NOT JB_JOBU_SYSTEMD_PROFILE MATCHES "^(none|system|custom)$")
    message(FATAL_ERROR "JB_JOBU_SYSTEMD_PROFILE must be none, system, or custom")
endif()
if(NOT JB_JOBU_SYSTEMD_PROFILE STREQUAL "none")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT JB_BUILD_SQLITE_DRIVER)
        message(FATAL_ERROR "A JobU systemd profile requires Linux and JB_BUILD_SQLITE_DRIVER=ON")
    endif()
    if(JB_JOBU_SYSTEMD_PROFILE STREQUAL "system" AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        message(FATAL_ERROR "The system systemd profile requires CMAKE_INSTALL_PREFIX=/usr")
    endif()
endif()

function(jobu_systemd_argument output path)
    if(NOT IS_ABSOLUTE "${path}" OR path MATCHES ";")
        message(FATAL_ERROR "Systemd profile paths must be absolute and contain no CMake list separators")
    endif()
    # Control characters cannot be safely shared with the INI/install manifests.
    foreach(code RANGE 1 31)
        string(ASCII ${code} character)
        string(FIND "${path}" "${character}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Systemd profile paths must contain no control characters")
        endif()
    endforeach()
    string(ASCII 127 character)
    string(FIND "${path}" "${character}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "Systemd profile paths must contain no control characters")
    endif()

    # ExecStart is parsed by systemd, with C-style quoting followed by specifier
    # and environment expansion. Shell quoting alone does not preserve paths.
    string(REPLACE "\\" "\\\\" escaped "${path}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "%" "%%" escaped "${escaped}")
    string(REPLACE "$" "$$" escaped "${escaped}")
    set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

function(jobu_configure_systemd_assets)
    if(JB_JOBU_SYSTEMD_PROFILE STREQUAL "none")
        return()
    endif()

    # CMake's generated install scripts embed these destinations in quoted
    # literals. Reject layouts they cannot preserve, instead of producing a
    # valid unit next to an unusable or expansion-dependent installation.
    foreach(directory CMAKE_INSTALL_PREFIX CMAKE_INSTALL_BINDIR CMAKE_INSTALL_DATADIR)
        set(path "${${directory}}")
        if(path MATCHES "[\"\\\\]" OR path MATCHES "\\$\\{|\\$ENV\\{")
            message(FATAL_ERROR "Systemd profile install destinations cannot contain quotes, backslashes, or CMake variable expressions")
        endif()
    endforeach()
    jobu_systemd_argument(unused "${CMAKE_INSTALL_FULL_DATADIR}")

    # Systemd validates the executable before argument environment expansion.
    # Quotes/backslashes are forbidden there, and $$ would change the actual
    # executable pathname rather than preserve a literal dollar sign.
    if(CMAKE_INSTALL_FULL_BINDIR MATCHES "[$'\"\\\\]")
        message(FATAL_ERROR "Systemd executable destinations cannot contain dollar signs, quotes, or backslashes")
    endif()
    jobu_systemd_argument(JOBU_SYSTEMD_EXECUTABLE "${CMAKE_INSTALL_FULL_BINDIR}/jobud")
    jobu_systemd_argument(JOBU_SYSTEMD_CONFIG "${JB_JOBU_SYSCONFDIR}/jobud.ini")
    # Validate the other profile paths even though ExecStart does not contain
    # them: the generated INI and compiled defaults belong to this same profile.
    jobu_systemd_argument(unused "${JB_JOBU_STATEDIR}")
    jobu_systemd_argument(unused "${JB_JOBU_RUNDIR}")

    set(JOBU_SYSTEMD_DIRECTORIES "")
    if(JB_JOBU_SYSTEMD_PROFILE STREQUAL "system")
        set(JOBU_SYSTEMD_DIRECTORIES "RuntimeDirectory=jobu\nRuntimeDirectoryMode=0700\nStateDirectory=jobu\nStateDirectoryMode=0700\n")
    endif()
    set(unit "${PROJECT_BINARY_DIR}/jobu-generated/jobud.service")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../packaging/systemd/jobud.service.in" "${unit}" @ONLY)
    install(FILES "${unit}" DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu/services" COMPONENT Runtime)
    if(JB_JOBU_SYSTEMD_PROFILE STREQUAL "system")
        # This is the vendor unit directory, independent of multiarch libdir.
        # A prefix-relative destination keeps DESTDIR and TGZ staging isolated.
        install(FILES "${unit}" DESTINATION lib/systemd/system COMPONENT Runtime)
    endif()
endfunction()
