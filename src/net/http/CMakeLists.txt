find_package(CURL 7.85 REQUIRED)

set(TARGET net-http)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        curl_error_priv.cpp
        curl_error_priv.hpp
        curl_multi_priv.cpp
        curl_multi_priv.hpp
        curl_multi_test_priv.hpp
        curl_request_priv.cpp
        curl_request_priv.hpp
        curl_runtime_priv.cpp
        curl_runtime_priv.hpp
        http_url_priv.cpp
        http_url_priv.hpp
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
