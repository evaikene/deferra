set(TARGET jobu-cli)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    cli_attempt_executor.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        cli_attempt_executor.cpp
        process_adapter_priv.cpp
        process_adapter_priv.hpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC jobu)

jobu_install_library(${TARGET} jobu/cli COMPONENT CLI HEADERS ${HDRS})
