#include <phpcxx/string.hpp>

#include <utility>

namespace php {

String::~String() noexcept
{
    if (_string) {
        zend_string_release(_string);
    }
}

String::String(String&& other) noexcept
    : _string(other.release())
{}

auto String::operator=(String&& other) noexcept -> String&
{
    if (this != &other) {
        auto* previous = std::exchange(_string, other.release());
        if (previous) {
            zend_string_release(previous);
        }
    }
    return *this;
}

auto String::create(std::string_view bytes) -> String
{
    return adopt(zend_string_init(bytes.empty() ? "" : bytes.data(), bytes.size(), false));
}

auto String::adopt(zend_string* string) noexcept -> String
{
    String result;
    result._string = string;
    return result;
}

auto String::retain(StringView string) noexcept -> String
{
    return adopt(string._string ? zend_string_copy(string._string) : nullptr);
}

auto String::duplicate() const -> String
{
    return adopt(_string ? zend_string_dup(_string, false) : nullptr);
}

auto String::release() noexcept -> zend_string*
{
    return std::exchange(_string, nullptr);
}

} // namespace php
