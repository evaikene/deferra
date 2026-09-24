set(TARGET jobuctl)

message("** Setting up ${TARGET}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../jobu_version_priv.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/jobu_version_priv.hpp
    @ONLY
)

# Private implementation shared by the executable and direct command parsing tests.
add_library(jobuctl-private STATIC
    command_line_priv.cpp
    command_registry_priv.cpp
    help_priv.cpp
    command_helpers_priv.cpp
    input_priv.cpp
    output_priv.cpp
    session_priv.cpp
    commands/system_commands.cpp
    commands/queue_commands.cpp
    commands/job_commands.cpp
    commands/run_commands.cpp
    commands/attempt_commands.cpp
)
target_include_directories(jobuctl-private PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(jobuctl-private PUBLIC cxx_std_20)
target_link_libraries(jobuctl-private PUBLIC jobu-client)

add_executable(${TARGET} main.cpp)

target_include_directories(${TARGET}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_features(${TARGET} PRIVATE cxx_std_20)

target_link_libraries(${TARGET} PRIVATE jobuctl-private)
