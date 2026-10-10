set(TARGET rpc)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    client.hpp
    framing.hpp
    protocol.hpp
    rpc.hpp
    server.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        client.cpp
        client_priv.hpp
        framing.cpp
        protocol.cpp
        protocol_priv.hpp
        rpc.cpp
        server.cpp
        server_priv.hpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)

jobu_install_library(${TARGET} rpc HEADERS ${HDRS})
