# INI strips one outer quote pair; it does not interpret C++ backslash escapes.
# Absolute paths start with '/', so surrounding them with quotes preserves spaces
# and every interior quote/backslash without changing the generic INI grammar.
set(JOBU_DATABASE_PATH_INI "\"${JB_JOBU_STATEDIR}/jobu.sqlite3\"")
set(JOBU_SOCKET_PATH_INI "\"${JB_JOBU_RUNDIR}/jobud.sock\"")
set(JOBU_CONFIG_EXAMPLE "${CMAKE_CURRENT_BINARY_DIR}/jobu-generated/jobud.ini.example")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../packaging/jobud.ini.in"
    "${JOBU_CONFIG_EXAMPLE}" @ONLY)

# Examples are data. Installing them never creates an active config or daemon
# state, provisions identities, or installs/enables a service in its manager.
install(FILES "${JOBU_CONFIG_EXAMPLE}" "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/packaging/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu" COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/systemd/jobud.service.in"
    "${PROJECT_SOURCE_DIR}/packaging/systemd/README.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu/services" COMPONENT Runtime)
install(FILES
    "${PROJECT_SOURCE_DIR}/examples/jobu-client/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/examples/jobu-client/main.cpp"
    "${PROJECT_SOURCE_DIR}/examples/jobu-client/idempotent_creation.cpp"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/jobu/examples/jobu-client" COMPONENT Development)

# User manuals only: planning and shared verification records are not package assets.
set(JOBU_MANUALS installation.md cpp-client.md jobuctl.md cron.md secrets.md)
foreach(manual IN LISTS JOBU_MANUALS)
    install(FILES "${PROJECT_SOURCE_DIR}/docs/${manual}"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT Documentation)
endforeach()
set(JOBU_PROTOCOL_MANUALS
    README.md errors.md transport.md types.md
    methods/attempt.md methods/job.md methods/queue.md methods/run.md
    methods/schedule.md methods/secret.md methods/system.md
    examples/README.md
    examples/attempt-get-max.params.json examples/attempt-list.result.json examples/attempt-output.params.json
    examples/job-create-cron-default.params.json examples/job-create-secret.params.json examples/job-create.params.json
    examples/job-list-all.params.json examples/job-list.params.json examples/job-list.result.json
    examples/queue-create.params.json examples/queue-list.result.json
    examples/run-list.params.json examples/run-list.result.json
    examples/schedule-next-default.params.json examples/schedule-next.params.json examples/schedule-next.result.json
    examples/secret-set.params.json examples/secret-set.result.json
    examples/system-stats.params.json examples/system-stats.result.json
)
foreach(manual IN LISTS JOBU_PROTOCOL_MANUALS)
    get_filename_component(manual_directory "${manual}" DIRECTORY)
    install(FILES "${PROJECT_SOURCE_DIR}/docs/protocol/${manual}"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/protocol/${manual_directory}" COMPONENT Documentation)
endforeach()
