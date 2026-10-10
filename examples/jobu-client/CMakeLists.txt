cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(JobUClientExample LANGUAGES CXX)
    find_package(JobU CONFIG REQUIRED COMPONENTS Client)
endif()

add_executable(jobu-client-example
    main.cpp
    idempotent_creation.cpp
)

target_link_libraries(jobu-client-example PRIVATE JobU::jobu-client)
