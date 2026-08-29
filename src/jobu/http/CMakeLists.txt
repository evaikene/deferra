set(TARGET jobu-http)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        http_attempt_executor.cpp
    PUBLIC
        http_attempt_executor.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC jobu)
