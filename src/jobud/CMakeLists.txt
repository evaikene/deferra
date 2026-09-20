set(TARGET jobud)

message("** Setting up ${TARGET}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../jobu_version_priv.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/jobu_version_priv.hpp
    @ONLY
)

add_executable(${TARGET} main.cpp startup_priv.cpp composition_priv.cpp)

# Signal ownership is activated by the Stage 7.13 runtime composition.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(jobud-signal STATIC shutdown_signal_linux.cpp)
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
