set(TARGET db)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        database.cpp
        database_priv.hpp
        db.cpp
        query.cpp
        query_priv.hpp
        record.cpp
        transaction.cpp
        transaction_priv.hpp
        value.cpp
    PUBLIC
        database.hpp
        db.hpp
        driver.hpp
        driver_query.hpp
        query.hpp
        record.hpp
        transaction.hpp
        value.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)

if(JB_BUILD_SQLITE_DRIVER)
    add_subdirectory(sqlite)
endif()
