find_package(Catch2 CONFIG REQUIRED)

enable_testing()

# Object test
set(TARGET "object-test")
add_executable(${TARGET} object-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Signal test
set(TARGET "signal-test")
add_executable(${TARGET} signal-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Application test
set(TARGET "application-test")
add_executable(${TARGET} application-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Threaded event loop test
set(TARGET "event-thread-test")
add_executable(${TARGET} event-thread-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Shared data test
set(TARGET "shared-data-test")
add_executable(${TARGET} shared-data-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Enum bitmask test
set(TARGET "enum-bitmask-test")
add_executable(${TARGET} enum-bitmask-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Event loop test
set(TARGET "event-loop-test")
add_executable(${TARGET} event-loop-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Timer test
set(TARGET "timer-test")
add_executable(${TARGET} timer-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
