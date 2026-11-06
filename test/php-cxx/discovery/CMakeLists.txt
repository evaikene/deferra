cmake_minimum_required(VERSION 3.20)
project(PhpCxxDiscovery LANGUAGES CXX)
include("${PHPCXX_CMAKE_MODULE}")
phpcxx_find_php()
if(NOT PHP_FOUND)
    message(FATAL_ERROR "${PHP_NOT_FOUND_MESSAGE}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/php-metadata.txt"
    "${PHP_MODULE_API};${PHP_DEBUG};${PHP_ZTS};${PHP_ZEND_LONG_SIZE}")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/php-embed.txt"
    "${PHP_EMBED_FOUND};${PHP_EMBED_LIBRARY}")
