set(TARGET jobu-client)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    control_client.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        control_client.cpp
        control_client_methods.cpp
        control_client_priv.hpp
        control_client_results.cpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)
target_link_libraries(${TARGET} PUBLIC jobu rpc)

jobu_install_library(${TARGET} jobu/client COMPONENT Client HEADERS ${HDRS})
