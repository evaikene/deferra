set(TARGET rpc)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

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
    PUBLIC
        client.hpp
        framing.hpp
        protocol.hpp
        rpc.hpp
        server.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)
