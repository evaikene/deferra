#pragma once

/// @file
/// C-linkage PHP/Zend headers and the common php-cxx integration contract.
///
/// php-cxx is an independent C++20 static library for PHP extensions, supporting
/// GCC 10 and newer and PHP 8.1 or newer with a 64-bit zend_long, in NTS or ZTS mode.
/// Link the CMake target PhpCxx::PhpCxx; it supplies public include paths and the
/// selected PHP headers. The library has no JobU or production PHP Embed dependency.
/// PHP/Zend symbols must be provided by the host PHP runtime (or matching Embed
/// runtime when used in a standalone executable).
///
/// Select one PHP installation using PHP_CONFIG, the absolute path to php-config.
/// Headers, the library archive, and the host runtime must match in module API,
/// debug mode, ZTS mode, and zend_long width. Use a separate build per ABI.
/// php::build_info() reports the archive's metadata; it does not validate the running host.
/// There is no cross-version C++ ABI guarantee for this unreleased library.
///
/// A source consumer can add_subdirectory() src/php-cxx and link PhpCxx::PhpCxx.
/// An installed consumer uses find_package(PhpCxx 0.1.0 EXACT CONFIG REQUIRED),
/// with CMAKE_PREFIX_PATH pointing to its installation and PHP_CONFIG selecting
/// the matching PHP installation, then links the same target.

/// @namespace php
/// Explicit Zend owners and borrowed read-only views for an initialized PHP request.
///
/// Create, use, and destroy Zend-backed owners/views on their owning PHP execution
/// thread within that request's lifetime. ZTS support does not make values safe to
/// share between threads. Even an interned/immutable backing allocation does not
/// authorize carrying a wrapper into another request. build_info() is the explicit
/// exception: it is request-independent and thread-safe.
///
/// adopt() consumes ownership, retain() acquires ownership, release() transfers it,
/// and view() borrows without extending lifetime. duplicate() follows the individual
/// type's documented duplication rules. Raw pointers returned by get() are borrowed;
/// never release or change reference counts through them. Use Array::writable()
/// before array mutation and obey its copy-on-write restrictions.
///
/// Native destructors do not throw C++ exceptions. PHP cleanup/conversion handlers
/// may execute PHP code and set a pending PHP exception. PHP zend_bailout() can bypass
/// C++ destructors; these wrappers provide no bailout recovery boundary. Ordinary
/// operations do not translate PHP diagnostics or pending exceptions into C++ exceptions.
extern "C" {
#include <Zend/zend_modules.h> // IWYU pragma: export
#include <php.h>               // IWYU pragma: export
}
