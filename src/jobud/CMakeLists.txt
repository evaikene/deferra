set(TARGET jobud)

message("** Setting up ${TARGET}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../jobu_version_priv.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/jobu_version_priv.hpp
    @ONLY
)

add_executable(${TARGET} main.cpp composition_priv.cpp)

add_library(jobud-runtime STATIC runtime_priv.cpp startup_priv.cpp)
target_include_directories(jobud-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(jobud-runtime PUBLIC jobu net rpc)
target_link_libraries(${TARGET} PRIVATE jobud-runtime)

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(JOBUD_SIGNAL_PIPE_SOURCE shutdown_signal_pipe_macos.cpp)
    else()
        set(JOBUD_SIGNAL_PIPE_SOURCE shutdown_signal_pipe_linux.cpp)
    endif()
    add_library(jobud-signal STATIC shutdown_signal_posix.cpp ${JOBUD_SIGNAL_PIPE_SOURCE})
    target_link_libraries(jobud-signal PUBLIC core)
    target_include_directories(jobud-signal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${TARGET} PRIVATE jobud-signal)
endif()

target_include_directories(${TARGET}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_features(${TARGET} PRIVATE cxx_std_20)

target_link_libraries(${TARGET}
    PRIVATE
        jobu-sqlite
        jobu-http
        jobu-cli
        net-http
)
