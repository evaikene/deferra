set(TARGET core)

message("** Setting up ${TARGET}")

# Public headers exposed to library users
set(HDRS
    connectable.hpp
    event_loop.hpp
    object.hpp
    signal.hpp
)

# Private headers not exposed to library users
set(PRIV_HDRS
    object_priv.hpp
    thread_context.hpp
)

# Library source files
set(SRCS
    event_loop.cpp
    object.cpp
    thread_context.cpp
)

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

target_link_libraries(${TARGET})
