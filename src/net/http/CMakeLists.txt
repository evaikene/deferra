find_package(CURL 7.85 REQUIRED)

set(TARGET net-http)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        curl_runtime_priv.cpp
        curl_runtime_priv.hpp
        system_http_client.cpp
    PUBLIC
        system_http_client.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        net
    PRIVATE
        CURL::libcurl
)
