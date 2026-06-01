set(TARGET net)

message("** Setting up ${TARGET}")

# Public headers exposed to library users
set(HDRS
    tcp_socket.hpp
)

# Library source files
set(SRCS
    tcp_socket.cpp
)

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        ${SRCS}
    PUBLIC
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)
