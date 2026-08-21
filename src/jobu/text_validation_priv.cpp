#include "text_validation_priv.hpp"

#include <cstddef>

namespace jb::jobu::detail {

namespace {

auto is_continuation(unsigned char byte) noexcept -> bool
{
    return byte >= 0x80U && byte <= 0xbfU;
}

} // anonymous namespace

auto is_valid_utf8(std::string_view text) noexcept -> bool
{
    auto const* bytes = reinterpret_cast<unsigned char const*>(text.data());
    auto        index = std::size_t{0};
    while (index < text.size()) {
        auto const first = bytes[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= text.size() || !is_continuation(bytes[index + 1])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= text.size() || !is_continuation(bytes[index + 2])) {
                return false;
            }
            auto const second       = bytes[index + 1];
            auto       valid_second = is_continuation(second);
            if (first == 0xe0U) {
                valid_second = second >= 0xa0U && second <= 0xbfU;
            }
            else if (first == 0xedU) {
                valid_second = second >= 0x80U && second <= 0x9fU;
            }
            if (!valid_second) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= text.size() || !is_continuation(bytes[index + 2]) || !is_continuation(bytes[index + 3])) {
                return false;
            }
            auto const second       = bytes[index + 1];
            auto       valid_second = is_continuation(second);
            if (first == 0xf0U) {
                valid_second = second >= 0x90U && second <= 0xbfU;
            }
            else if (first == 0xf4U) {
                valid_second = second >= 0x80U && second <= 0x8fU;
            }
            if (!valid_second) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

auto has_ascii_control(std::string_view text) noexcept -> bool
{
    for (auto const character : text) {
        auto const byte = static_cast<unsigned char>(character);
        if (byte <= 0x1fU || byte == 0x7fU) {
            return true;
        }
    }
    return false;
}

} // namespace jb::jobu::detail
