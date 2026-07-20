set(TARGET net)

message("** Setting up ${TARGET}")

# Public headers exposed to library users
set(HDRS
    local_socket.hpp
    tcp_socket.hpp
)

# Private headers not exposed to library users
set(PRIV_HDRS
    local_socket_priv.hpp
    tcp_socket_priv.hpp
)

# Library source files
set(SRCS
    tcp_socket.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SRCS
        local_socket_linux.cpp
    )
endif()

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        ${SRCS}
        ${PRIV_HDRS}
    PUBLIC
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)
