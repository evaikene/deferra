set(TARGET core)

message("** Setting up ${TARGET}")

# Find {fmt}
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Public headers exposed to library users
set(HDRS
    application.hpp
    byte_buffer.hpp
    command_line_parser.hpp
    connection.hpp
    error.hpp
    enum_bitmask.hpp
    event.hpp
    event_loop.hpp
    event_loop_types.hpp
    event_thread.hpp
    file.hpp
    ini_file.hpp
    io_device.hpp
    json.hpp
    logging.hpp
    object.hpp
    process.hpp
    result.hpp
    shared_data.hpp
    signal.hpp
    timer.hpp
    time_source.hpp
    utils.hpp
    uuid.hpp
)

# Private headers not exposed to library users
set(PRIV_HDRS
    application_priv.hpp
    event_loop_backend.hpp
    event_loop_backend_kqueue_priv.hpp
    event_thread_priv.hpp
    file_priv.hpp
    io_device_priv.hpp
    object_priv.hpp
    process_priv.hpp
    process_request_priv.hpp
    signal_priv.hpp
    thread_context.hpp
    timer_heap.hpp
    timer_priv.hpp
)

# Library source files
set(SRCS
    application.cpp
    command_line_parser.cpp
    event_loop.cpp
    event_thread.cpp
    file.cpp
    ini_file.cpp
    io_device.cpp
    json.cpp
    logging.cpp
    object.cpp
    process.cpp
    process_request_priv.cpp
    thread_context.cpp
    timer.cpp
    time_source.cpp
    utils.cpp
    uuid.cpp
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

target_link_libraries(${TARGET}
    PUBLIC fmt::fmt
    PRIVATE nlohmann_json::nlohmann_json
)
