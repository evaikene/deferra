#include "attribute.hpp"

namespace jb::jobu {

namespace {

auto is_segment_start(char character) noexcept -> bool
{
    return character >= 'a' && character <= 'z';
}

auto is_segment_character(char character) noexcept -> bool
{
    return is_segment_start(character) || (character >= '0' && character <= '9') || character == '_';
}

} // anonymous namespace

auto is_valid_attribute_name(std::string_view name) noexcept -> bool
{
    if (name.empty()) {
        return false;
    }

    bool segment_start{true};
    for (auto const character : name) {
        if (character == '.') {
            if (segment_start) {
                return false;
            }
            segment_start = true;
            continue;
        }
        if (segment_start ? !is_segment_start(character) : !is_segment_character(character)) {
            return false;
        }
        segment_start = false;
    }
    return !segment_start;
}

} // namespace jb::jobu
