set(TARGET jobud)

message("** Setting up ${TARGET}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../jobu_version_priv.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/jobu_version_priv.hpp
    @ONLY
)

add_executable(${TARGET} main.cpp)

target_include_directories(${TARGET}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_features(${TARGET} PRIVATE cxx_std_20)

target_link_libraries(${TARGET} PRIVATE jobu)
