set(TARGET core)

message("** Setting up ${TARGET}")

# Find {fmt}
find_package(fmt CONFIG REQUIRED)

# Public headers exposed to library users
set(HDRS
    application.hpp
    connectable.hpp
    enum_bitmask.hpp
    event_loop.hpp
    event_loop_types.hpp
    event_thread.hpp
    logging.hpp
    object.hpp
    shared_data.hpp
    signal.hpp
    timer.hpp
)

# Private headers not exposed to library users
set(PRIV_HDRS
  event_loop_backend.hpp
    object_priv.hpp
    thread_context.hpp
    timer_heap.hpp
)

# Library source files
set(SRCS
    application.cpp
    event_loop.cpp
    event_thread.cpp
    logging.cpp
    object.cpp
    signal.cpp
    thread_context.cpp
    timer.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SRCS event_loop_backend_epoll.cpp)
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin|FreeBSD|OpenBSD|NetBSD")
    list(APPEND SRCS event_loop_backend_kqueue.cpp)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
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

target_link_libraries(${TARGET} PUBLIC fmt::fmt)
