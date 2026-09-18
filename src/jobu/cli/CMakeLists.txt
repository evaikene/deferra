set(TARGET jobu-cli)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        cli_attempt_executor.cpp
        process_adapter_priv.cpp
        process_adapter_priv.hpp
    PUBLIC
        cli_attempt_executor.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC jobu)
