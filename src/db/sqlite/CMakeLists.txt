find_package(SQLite3 REQUIRED)

if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

set(TARGET db-sqlite)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        sqlite_driver.cpp
        sqlite_driver_priv.hpp
        sqlite_error.cpp
        sqlite_error.hpp
        sqlite_query.cpp
        sqlite_query_priv.hpp
    PUBLIC
        sqlite_driver.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        db
    PRIVATE
        SQLite3::SQLite3
)
