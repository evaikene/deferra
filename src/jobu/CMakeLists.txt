set(TARGET jobu)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        attribute.cpp
        attribute_codec_priv.cpp
        attribute_codec_priv.hpp
        attribute_registry.cpp
        domain_storage_priv.cpp
        domain_storage_priv.hpp
        jobu.cpp
        system_info.cpp
        system_info_priv.hpp
        utc_timestamp.cpp
    PUBLIC
        attempt.hpp
        attribute.hpp
        attribute_registry.hpp
        job.hpp
        jobu.hpp
        queue.hpp
        run.hpp
        secret.hpp
        system_info.hpp
        utc_timestamp.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        core
        net
        db
        rpc
)
