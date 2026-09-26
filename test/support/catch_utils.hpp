#pragma once

#include "error.hpp"
#include "result.hpp"

#include <catch2/catch_tostring.hpp>

#include <string>

namespace Catch {

/// Shows the stable code and safe message, omitting backend detail.
template <>
struct StringMaker<jb::core::Error> {
    static auto convert(jb::core::Error const& error) -> std::string { return "(" + error.code + ") " + error.message; }
};

/// Shows the outcome of a Result without exposing successful payloads.
template <typename T, typename E>
struct StringMaker<jb::core::Result<T, E>> {
    static auto convert(jb::core::Result<T, E> const& result) -> std::string
    {
        if (!result.is_initialized()) {
            return "uninitialized";
        }

        if (result.has_value()) {
            return "success";
        }

        return "error: " + ::Catch::Detail::stringify(result.error());
    }
};

} // namespace Catch
