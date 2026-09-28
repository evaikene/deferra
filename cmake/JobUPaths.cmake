# Operational paths are compiled into applications. Installation staging does not change them.
foreach(path_name SYSCONFDIR STATEDIR RUNDIR)
    if(path_name STREQUAL "SYSCONFDIR")
        set(path_suffix "etc")
    elseif(path_name STREQUAL "STATEDIR")
        set(path_suffix "var/lib")
    else()
        set(path_suffix "var/run")
    endif()

    set(variable_name "JB_JOBU_${path_name}")
    if(NOT DEFINED ${variable_name})
        set(${variable_name} "${CMAKE_INSTALL_PREFIX}/${path_suffix}" CACHE PATH "JobU ${path_name} operational path")
    endif()
    if(NOT IS_ABSOLUTE "${${variable_name}}" OR "${${variable_name}}" MATCHES "[\r\n]")
        message(FATAL_ERROR "${variable_name} must be an absolute single-line path")
    endif()
endforeach()

# configure_file substitutes these into quoted C++ literals. Escape both C++ metacharacters.
foreach(path_name SYSCONFDIR STATEDIR RUNDIR)
    set(path_value "${JB_JOBU_${path_name}}")
    string(REPLACE "\\" "\\\\" path_value "${path_value}")
    string(REPLACE "\"" "\\\"" path_value "${path_value}")
    set(JB_JOBU_${path_name}_CPP "${path_value}")
endforeach()

set(JOBU_PATHS_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated")
file(MAKE_DIRECTORY "${JOBU_PATHS_INCLUDE_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/jobu_paths_priv.hpp.in"
    "${JOBU_PATHS_INCLUDE_DIR}/jobu_paths_priv.hpp" @ONLY)

add_library(jobu-paths-private INTERFACE)
target_include_directories(jobu-paths-private INTERFACE "${JOBU_PATHS_INCLUDE_DIR}")
