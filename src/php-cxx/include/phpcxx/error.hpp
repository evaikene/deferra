#pragma once

#include <cstdint>
#include <string>

namespace php {

/// Recoverable failure category; independent of Zend and JobU.
enum class ErrorKind : std::uint8_t {
    Type,
    Value,
    ArgumentCount,
    Runtime,
    PendingException
};

/// Owned diagnostic, optionally associated with a one-based PHP argument number.
/// PendingException is a marker for the exception still owned by the current PHP
/// request; it does not retain that exception or permit carrying it across requests.
/// Messages describe requirements, not submitted values that may contain secrets.
struct Error {
    ErrorKind     kind;
    std::string   message;
    std::uint32_t argument = 0;
};

} // namespace php
