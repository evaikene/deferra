# Select only the exact private-prefix defaults. A neighbouring or external
# operational path is operator policy, even when it starts with the same text.
set(JOBU_RUNTIME_LAYOUT_DIRECTORIES "")
if("${JB_JOBU_SYSCONFDIR}" STREQUAL "${CMAKE_INSTALL_PREFIX}/etc")
    list(APPEND JOBU_RUNTIME_LAYOUT_DIRECTORIES etc)
    set(JOBU_CONFIGURATION_README_DESTINATION etc)
    set(JOBU_CONFIGURATION_README_NAME README.md)
else()
    set(JOBU_CONFIGURATION_README_DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu")
    set(JOBU_CONFIGURATION_README_NAME configuration-README.md)
endif()
if("${JB_JOBU_STATEDIR}" STREQUAL "${CMAKE_INSTALL_PREFIX}/var/lib"
    OR "${JB_JOBU_RUNDIR}" STREQUAL "${CMAKE_INSTALL_PREFIX}/var/run")
    list(APPEND JOBU_RUNTIME_LAYOUT_DIRECTORIES var)
endif()
if("${JB_JOBU_STATEDIR}" STREQUAL "${CMAKE_INSTALL_PREFIX}/var/lib")
    list(APPEND JOBU_RUNTIME_LAYOUT_DIRECTORIES var/lib)
endif()
if("${JB_JOBU_RUNDIR}" STREQUAL "${CMAKE_INSTALL_PREFIX}/var/run")
    list(APPEND JOBU_RUNTIME_LAYOUT_DIRECTORIES var/run)
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/jobu_runtime_layout_install.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated/runtime-layout-install.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated/runtime-layout-install.cmake" COMPONENT Runtime)

# Shell examples borrow configuration-time paths, never DESTDIR. Single quotes
# protect spaces and shell metacharacters, including embedded single quotes.
function(jobu_shell_quote output value)
    string(REPLACE "'" "'\\''" escaped "${value}")
    set(${output} "'${escaped}'" PARENT_SCOPE)
endfunction()

jobu_shell_quote(JOBU_README_EXAMPLE_SHELL "${CMAKE_INSTALL_FULL_DATADIR}/jobu/jobud.ini.example")
jobu_shell_quote(JOBU_README_CONFIG_SHELL "${JB_JOBU_SYSCONFDIR}/jobud.ini")
jobu_shell_quote(JOBU_README_DAEMON_SHELL "${CMAKE_INSTALL_FULL_BINDIR}/jobud")
set(JOBU_CONFIGURATION_README "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated/configuration-README.md")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../packaging/configuration-readme.md.in"
    "${JOBU_CONFIGURATION_README}" @ONLY)
install(FILES "${JOBU_CONFIGURATION_README}"
    DESTINATION "${JOBU_CONFIGURATION_README_DESTINATION}" RENAME "${JOBU_CONFIGURATION_README_NAME}"
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ COMPONENT Runtime)
