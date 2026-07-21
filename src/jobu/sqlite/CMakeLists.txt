set(TARGET jobu-sqlite)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        sqlite_schema.cpp
        sqlite_schema_priv.hpp
    PUBLIC
        sqlite_schema.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        jobu
        db-sqlite
)
