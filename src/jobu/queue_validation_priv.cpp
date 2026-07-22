#include "queue_validation_priv.hpp"

#include "text_validation_priv.hpp"

#include <cstddef>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t kMaximumQueueNameBytes      = 128;
constexpr std::size_t kMaximumIdempotencyKeyBytes = 128;

auto is_hexadecimal(char character) noexcept -> bool
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

auto is_uuid_text(std::string_view text) noexcept -> bool
{
    if (text.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (text[index] != '-') {
                return false;
            }
        }
        else if (!is_hexadecimal(text[index])) {
            return false;
        }
    }
    return true;
}

auto has_reserved_deletion_suffix(std::string_view name) noexcept -> bool
{
    constexpr std::string_view marker{"-deleted#"};
    auto const                 position = name.rfind(marker);
    return position != std::string_view::npos && is_uuid_text(name.substr(position + marker.size()));
}

} // anonymous namespace

auto is_valid_queue_name(std::string_view name) noexcept -> bool
{
    return !name.empty() && name.size() <= kMaximumQueueNameBytes && is_valid_utf8(name) && !has_ascii_control(name) &&
           !has_reserved_deletion_suffix(name);
}

auto is_valid_idempotency_key(std::string_view key) noexcept -> bool
{
    return !key.empty() && key.size() <= kMaximumIdempotencyKeyBytes && is_valid_utf8(key);
}

} // namespace jb::jobu::detail
