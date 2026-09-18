#pragma once

#include "attribute.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace jb::jobu::detail {

struct CliExitCodeRange {
    std::uint16_t first{0};
    std::uint16_t last{0};

    auto operator==(CliExitCodeRange const&) const -> bool = default;
};

class CliExitCodeSet final {
public:
    [[nodiscard]] auto contains(int exit_code) const noexcept -> bool;

    [[nodiscard]] auto ranges() const noexcept -> std::vector<CliExitCodeRange> const& { return _ranges; }

    auto operator==(CliExitCodeSet const&) const -> bool = default;

private:
    explicit CliExitCodeSet(std::vector<CliExitCodeRange> ranges) noexcept;

    friend auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>;

    std::vector<CliExitCodeRange> _ranges;
};

[[nodiscard]] auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>;

} // namespace jb::jobu::detail
