#include "cli_exit_policy_priv.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t   kMaximumRetryExitCodeSelectors{64};
constexpr std::uint16_t kMaximumExitCode{255};

auto parse_exit_code(std::string_view text) noexcept -> std::optional<std::uint16_t>
{
    if (text.empty() || (text.size() > 1U && text.front() == '0')) {
        return std::nullopt;
    }

    auto value = std::uint16_t{0};
    for (char const character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }

        value = static_cast<std::uint16_t>((value * 10U) + static_cast<unsigned int>(character - '0'));
        if (value > kMaximumExitCode) {
            return std::nullopt;
        }
    }
    return value;
}

auto parse_selector(std::string_view selector) noexcept -> std::optional<CliExitCodeRange>
{
    auto const separator = selector.find('-');
    if (separator == std::string_view::npos) {
        auto code = parse_exit_code(selector);
        if (!code) {
            return std::nullopt;
        }
        return CliExitCodeRange{.first = *code, .last = *code};
    }
    if (selector.find('-', separator + 1U) != std::string_view::npos) {
        return std::nullopt;
    }

    auto first = parse_exit_code(selector.substr(0, separator));
    auto last  = parse_exit_code(selector.substr(separator + 1U));
    if (!first || !last || *first >= *last) {
        return std::nullopt;
    }
    return CliExitCodeRange{.first = *first, .last = *last};
}

} // anonymous namespace

CliExitCodeSet::CliExitCodeSet(std::vector<CliExitCodeRange> ranges) noexcept
    : _ranges{std::move(ranges)}
{}

auto CliExitCodeSet::contains(int exit_code) const noexcept -> bool
{
    if (exit_code < 0 || exit_code > kMaximumExitCode) {
        return false;
    }

    auto const code = static_cast<std::uint16_t>(exit_code);
    for (auto const range : _ranges) {
        if (code < range.first) {
            return false;
        }
        if (code <= range.last) {
            return true;
        }
    }
    return false;
}

auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>
{
    if (selectors.size() > kMaximumRetryExitCodeSelectors) {
        return std::nullopt;
    }

    auto ranges = std::vector<CliExitCodeRange>{};
    ranges.reserve(selectors.size());
    for (auto const& value : selectors) {
        auto const* selector = std::get_if<std::string>(&value.data);
        if (selector == nullptr) {
            return std::nullopt;
        }

        auto range = parse_selector(*selector);
        if (!range) {
            return std::nullopt;
        }
        ranges.push_back(*range);
    }

    std::ranges::sort(ranges, {}, &CliExitCodeRange::first);
    auto normalized = std::vector<CliExitCodeRange>{};
    normalized.reserve(ranges.size());
    for (auto const range : ranges) {
        if (normalized.empty()) {
            normalized.push_back(range);
            continue;
        }

        auto& previous = normalized.back();

        // Reject aliases before merging adjacency so duplicate or overlapping
        // policy cannot disappear inside the normalized membership set.
        if (range.first <= previous.last) {
            return std::nullopt;
        }
        if (range.first == previous.last + 1U) {
            previous.last = range.last;
            continue;
        }
        normalized.push_back(range);
    }
    return CliExitCodeSet{std::move(normalized)};
}

} // namespace jb::jobu::detail
