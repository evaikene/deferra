set(TARGET jobu-sqlite)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    sqlite_schema.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        sqlite_schema.cpp
        sqlite_schema_priv.hpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        jobu
        db-sqlite
)

jobu_install_library(${TARGET} jobu/sqlite COMPONENT SQLite HEADERS ${HDRS})
