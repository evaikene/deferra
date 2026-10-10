set(TARGET db)

message("** Setting up ${TARGET}")

# Public headers and the explicit installed-header manifest.
set(HDRS
    database.hpp
    db.hpp
    driver.hpp
    driver_query.hpp
    query.hpp
    record.hpp
    transaction.hpp
    value.hpp
)

add_library(${TARGET} STATIC)

target_sources(${TARGET}
    PRIVATE
        database.cpp
        database_priv.hpp
        db.cpp
        driver.cpp
        driver_query.cpp
        query.cpp
        query_priv.hpp
        record.cpp
        transaction.cpp
        transaction_priv.hpp
        value.cpp
        ${HDRS}
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)

jobu_install_library(${TARGET} db COMPONENT Db HEADERS ${HDRS})

if(JB_BUILD_SQLITE_DRIVER)
    add_subdirectory(sqlite)
endif()
