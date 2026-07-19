set(TARGET db)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        db.cpp
    PUBLIC
        db.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} PUBLIC core)
