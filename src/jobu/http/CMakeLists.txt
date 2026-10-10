set(TARGET jobu-http)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    http_attempt_executor.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        http_attempt_executor.cpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC jobu)

jobu_install_library(${TARGET} jobu/http COMPONENT HTTP HEADERS ${HDRS})
