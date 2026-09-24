add_executable(jobu-client-example
    main.cpp
    idempotent_creation.cpp
)

target_link_libraries(jobu-client-example PRIVATE jobu-client)
