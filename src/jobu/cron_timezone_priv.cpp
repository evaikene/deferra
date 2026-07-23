#include "cron_timezone_priv.hpp"

#include "file.hpp"
#include "text_validation_priv.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace jb::jobu {

namespace {

using TimezoneResult = jb::core::Result<TimezoneData, jb::core::Error>;

constexpr std::size_t   kMaximumTimezoneFileSize = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumTransitionCount  = 1000000U;
constexpr std::uint32_t kMaximumTypeCount        = 256U;
constexpr std::uint32_t kMaximumAbbreviationSize = 64U * 1024U;
constexpr std::int32_t  kMinimumUtcOffset        = -89999;
constexpr std::int32_t  kMaximumUtcOffset        = 93599;
constexpr std::int32_t  kMaximumPosixHour        = 167;

struct TzifCounts {
    std::uint32_t utc_local_indicators{};
    std::uint32_t standard_wall_indicators{};
    std::uint32_t leap_seconds{};
    std::uint32_t transitions{};
    std::uint32_t local_time_types{};
    std::uint32_t abbreviation_bytes{};
};

struct TzifHeader {
    char       version{};
    TzifCounts counts;
};

struct ParsedSection {
    std::vector<TimezoneTransition>    transitions;
    std::vector<TimezoneLocalTimeType> local_time_types;
    std::uint8_t                       default_local_time_type{};
};

class ByteReader {
public:
    explicit ByteReader(std::string_view bytes)
        : _bytes{bytes}
    {}

    [[nodiscard]] auto remaining() const noexcept -> std::size_t { return _bytes.size() - _position; }

    auto read_u8(std::uint8_t& value) noexcept -> bool
    {
        if (remaining() < 1) {
            return false;
        }
        value = static_cast<std::uint8_t>(static_cast<unsigned char>(_bytes[_position++]));
        return true;
    }

    auto read_u32(std::uint32_t& value) noexcept -> bool
    {
        if (remaining() < 4) {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | static_cast<std::uint32_t>(static_cast<unsigned char>(_bytes[_position + index]));
        }
        _position += 4;
        return true;
    }

    auto read_i32(std::int32_t& value) noexcept -> bool
    {
        std::uint32_t encoded{};
        if (!read_u32(encoded)) {
            return false;
        }
        value = std::bit_cast<std::int32_t>(encoded);
        return true;
    }

    auto read_i64(std::int64_t& value) noexcept -> bool
    {
        if (remaining() < 8) {
            return false;
        }
        std::uint64_t encoded{};
        for (std::size_t index = 0; index < 8; ++index) {
            encoded =
                (encoded << 8U) | static_cast<std::uint64_t>(static_cast<unsigned char>(_bytes[_position + index]));
        }
        _position += 8;
        value      = std::bit_cast<std::int64_t>(encoded);
        return true;
    }

    auto read_bytes(std::size_t size, std::string_view& value) noexcept -> bool
    {
        if (size > remaining()) {
            return false;
        }
        value      = _bytes.substr(_position, size);
        _position += size;
        return true;
    }

    auto skip(std::size_t size) noexcept -> bool
    {
        if (size > remaining()) {
            return false;
        }
        _position += size;
        return true;
    }

    [[nodiscard]] auto remainder() const noexcept -> std::string_view { return _bytes.substr(_position); }

private:
    std::string_view _bytes;
    std::size_t      _position{};
};

auto invalid_timezone() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.schedule.invalid_timezone",
        .message  = "Timezone is invalid",
        .detail   = {},
    };
}

auto invalid_timezone_data(std::string detail) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.schedule.invalid_timezone_data",
        .message  = "Timezone data is invalid",
        .detail   = std::move(detail),
    };
}

auto checked_add(std::size_t& total, std::size_t value) noexcept -> bool
{
    if (value > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

auto checked_add_product(std::size_t& total, std::uint32_t count, std::size_t width) noexcept -> bool
{
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        return false;
    }
    return checked_add(total, static_cast<std::size_t>(count) * width);
}

auto valid_counts(TzifCounts const& counts) noexcept -> bool
{
    if (counts.transitions > kMaximumTransitionCount || counts.local_time_types == 0 ||
        counts.local_time_types > kMaximumTypeCount || counts.abbreviation_bytes == 0 ||
        counts.abbreviation_bytes > kMaximumAbbreviationSize) {
        return false;
    }

    auto const valid_standard_count =
        counts.standard_wall_indicators == 0 || counts.standard_wall_indicators == counts.local_time_types;
    auto const valid_utc_count =
        counts.utc_local_indicators == 0 || counts.utc_local_indicators == counts.local_time_types;
    return valid_standard_count && valid_utc_count;
}

auto read_header(ByteReader& reader, TzifHeader& header) noexcept -> bool
{
    std::string_view magic;
    std::string_view reserved;
    std::uint8_t     version{};
    if (!reader.read_bytes(4, magic) || magic != "TZif" || !reader.read_u8(version) ||
        !reader.read_bytes(15, reserved) || reserved.find_first_not_of('\0') != std::string_view::npos ||
        !reader.read_u32(header.counts.utc_local_indicators) ||
        !reader.read_u32(header.counts.standard_wall_indicators) || !reader.read_u32(header.counts.leap_seconds) ||
        !reader.read_u32(header.counts.transitions) || !reader.read_u32(header.counts.local_time_types) ||
        !reader.read_u32(header.counts.abbreviation_bytes)) {
        return false;
    }

    header.version = static_cast<char>(version);
    return (header.version == '\0' || header.version == '2' || header.version == '3' || header.version == '4') &&
           valid_counts(header.counts);
}

auto section_size(TzifCounts const& counts, std::size_t time_width) noexcept -> std::optional<std::size_t>
{
    std::size_t size{};
    if (!checked_add_product(size, counts.transitions, time_width) ||
        !checked_add_product(size, counts.transitions, 1) || !checked_add_product(size, counts.local_time_types, 6) ||
        !checked_add(size, counts.abbreviation_bytes) ||
        !checked_add_product(size, counts.leap_seconds, time_width + 4) ||
        !checked_add(size, counts.standard_wall_indicators) || !checked_add(size, counts.utc_local_indicators)) {
        return std::nullopt;
    }
    return size;
}

auto read_transition_time(ByteReader& reader, std::size_t time_width, std::int64_t& value) noexcept -> bool
{
    if (time_width == 8) {
        return reader.read_i64(value);
    }

    std::int32_t narrow{};
    if (!reader.read_i32(narrow)) {
        return false;
    }
    value = narrow;
    return true;
}

auto abbreviation_is_terminated(std::string_view abbreviations, std::uint8_t index) noexcept -> bool
{
    if (index >= abbreviations.size()) {
        return false;
    }
    return abbreviations.find('\0', index) != std::string_view::npos;
}

auto parse_section(ByteReader& reader, TzifCounts const& counts, std::size_t time_width, char version)
    -> std::optional<ParsedSection>
{
    auto const encoded_size = section_size(counts, time_width);
    if (!encoded_size || *encoded_size > reader.remaining()) {
        return std::nullopt;
    }

    std::vector<std::int64_t> transition_times(counts.transitions);
    for (std::size_t index = 0; index < transition_times.size(); ++index) {
        if (!read_transition_time(reader, time_width, transition_times[index]) ||
            (index != 0 && transition_times[index] <= transition_times[index - 1])) {
            return std::nullopt;
        }
    }

    std::vector<std::uint8_t> transition_types(counts.transitions);
    for (auto& type : transition_types) {
        if (!reader.read_u8(type) || type >= counts.local_time_types) {
            return std::nullopt;
        }
    }

    struct EncodedType {
        TimezoneLocalTimeType value;
        std::uint8_t          abbreviation_index{};
    };

    std::vector<EncodedType> encoded_types(counts.local_time_types);
    for (auto& type : encoded_types) {
        std::uint8_t daylight{};
        if (!reader.read_i32(type.value.utc_offset_seconds) || !reader.read_u8(daylight) ||
            !reader.read_u8(type.abbreviation_index) || type.value.utc_offset_seconds < kMinimumUtcOffset ||
            type.value.utc_offset_seconds > kMaximumUtcOffset || daylight > 1) {
            return std::nullopt;
        }
        type.value.daylight = daylight != 0;
    }

    std::string_view abbreviations;
    if (!reader.read_bytes(counts.abbreviation_bytes, abbreviations)) {
        return std::nullopt;
    }
    for (auto const& type : encoded_types) {
        if (!abbreviation_is_terminated(abbreviations, type.abbreviation_index)) {
            return std::nullopt;
        }
    }

    std::int64_t previous_leap_time{};
    std::int32_t previous_correction{};
    for (std::uint32_t index = 0; index < counts.leap_seconds; ++index) {
        std::int64_t leap_time{};
        std::int32_t correction{};
        if (!read_transition_time(reader, time_width, leap_time) || !reader.read_i32(correction) || leap_time < 0 ||
            (index != 0 && leap_time <= previous_leap_time)) {
            return std::nullopt;
        }

        if (index == 0) {
            if (version != '4' && correction != -1 && correction != 1) {
                return std::nullopt;
            }
        }
        else {
            auto const delta = static_cast<std::int64_t>(correction) - previous_correction;
            auto const expiration =
                version == '4' && index + 1 == counts.leap_seconds && correction == previous_correction;
            if (!expiration && delta != -1 && delta != 1) {
                return std::nullopt;
            }
        }
        previous_leap_time  = leap_time;
        previous_correction = correction;
    }

    std::vector<std::uint8_t> standard_indicators(counts.local_time_types);
    for (std::uint32_t index = 0; index < counts.standard_wall_indicators; ++index) {
        if (!reader.read_u8(standard_indicators[index]) || standard_indicators[index] > 1) {
            return std::nullopt;
        }
    }
    for (std::uint32_t index = 0; index < counts.utc_local_indicators; ++index) {
        std::uint8_t indicator{};
        if (!reader.read_u8(indicator) || indicator > 1 || (indicator != 0 && standard_indicators[index] == 0)) {
            return std::nullopt;
        }
    }

    ParsedSection parsed;
    parsed.local_time_types.reserve(encoded_types.size());
    for (auto const& type : encoded_types) {
        parsed.local_time_types.push_back(type.value);
    }
    parsed.transitions.reserve(transition_times.size());
    for (std::size_t index = 0; index < transition_times.size(); ++index) {
        parsed.transitions.push_back(
            {.unix_seconds = transition_times[index], .local_time_type = transition_types[index]});
    }

    // TZif assigns type zero to the indefinite period before the first
    // transition. Do not substitute a guessed standard-time type.
    return parsed;
}

constexpr auto ascii_alpha(char value) noexcept -> bool
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

constexpr auto ascii_digit(char value) noexcept -> bool
{
    return value >= '0' && value <= '9';
}

constexpr auto valid_quoted_abbreviation_character(char value) noexcept -> bool
{
    return ascii_alpha(value) || ascii_digit(value) || value == '+' || value == '-';
}

class PosixReader {
public:
    explicit PosixReader(std::string_view text)
        : _text{text}
    {}

    [[nodiscard]] auto at_end() const noexcept -> bool { return _position == _text.size(); }

    [[nodiscard]] auto peek() const noexcept -> char { return at_end() ? '\0' : _text[_position]; }

    auto consume(char expected) noexcept -> bool
    {
        if (peek() != expected) {
            return false;
        }
        ++_position;
        return true;
    }

    auto abbreviation() -> std::optional<std::string>
    {
        auto const begin = _position;
        if (consume('<')) {
            auto const content_begin = _position;
            while (!at_end() && valid_quoted_abbreviation_character(peek())) {
                ++_position;
            }
            if (!consume('>') || _position - content_begin - 1 < 3) {
                return std::nullopt;
            }
            return std::string{_text.substr(content_begin, _position - content_begin - 1)};
        }

        while (!at_end() && ascii_alpha(peek())) {
            ++_position;
        }
        if (_position - begin < 3) {
            return std::nullopt;
        }
        return std::string{_text.substr(begin, _position - begin)};
    }

    auto unsigned_number(unsigned minimum, unsigned maximum) -> std::optional<unsigned>
    {
        auto const begin = _position;
        while (!at_end() && ascii_digit(peek())) {
            ++_position;
        }
        if (begin == _position) {
            return std::nullopt;
        }

        unsigned   value{};
        auto const first  = _text.data() + begin;
        auto const last   = _text.data() + _position;
        auto const parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last || value < minimum || value > maximum) {
            return std::nullopt;
        }
        return value;
    }

    auto signed_time() -> std::optional<std::int32_t>
    {
        auto sign = 1;
        if (consume('-')) {
            sign = -1;
        }
        else {
            static_cast<void>(consume('+'));
        }

        auto const hours = unsigned_number(0, kMaximumPosixHour);
        if (!hours) {
            return std::nullopt;
        }

        auto minutes = 0U;
        auto seconds = 0U;
        if (consume(':')) {
            auto parsed_minutes = unsigned_number(0, 59);
            if (!parsed_minutes) {
                return std::nullopt;
            }
            minutes = *parsed_minutes;
            if (consume(':')) {
                auto parsed_seconds = unsigned_number(0, 59);
                if (!parsed_seconds) {
                    return std::nullopt;
                }
                seconds = *parsed_seconds;
            }
        }

        auto const magnitude = static_cast<std::int32_t>(*hours * 3600U + minutes * 60U + seconds);
        return sign * magnitude;
    }

    auto utc_offset() -> std::optional<std::int32_t>
    {
        auto const posix_offset = signed_time();
        if (!posix_offset) {
            return std::nullopt;
        }
        // POSIX offsets are added to local time to obtain UTC. Cached offsets
        // use TZif's inverse, local-minus-UTC convention.
        auto const utc_offset = -*posix_offset;
        if (utc_offset < kMinimumUtcOffset || utc_offset > kMaximumUtcOffset) {
            return std::nullopt;
        }
        return utc_offset;
    }

    auto date_rule() -> std::optional<PosixDateRule>
    {
        PosixDateRule rule;
        if (consume('M')) {
            auto const month = unsigned_number(1, 12);
            if (!month || !consume('.')) {
                return std::nullopt;
            }
            auto const week = unsigned_number(1, 5);
            if (!week || !consume('.')) {
                return std::nullopt;
            }
            auto const weekday = unsigned_number(0, 6);
            if (!weekday) {
                return std::nullopt;
            }
            rule.kind    = PosixDateRuleKind::MonthWeekday;
            rule.month   = static_cast<std::uint8_t>(*month);
            rule.week    = static_cast<std::uint8_t>(*week);
            rule.weekday = static_cast<std::uint8_t>(*weekday);
        }
        else if (consume('J')) {
            auto const day = unsigned_number(1, 365);
            if (!day) {
                return std::nullopt;
            }
            rule.kind = PosixDateRuleKind::JulianWithoutLeapDay;
            rule.day  = static_cast<std::uint16_t>(*day);
        }
        else {
            auto const day = unsigned_number(0, 365);
            if (!day) {
                return std::nullopt;
            }
            rule.kind = PosixDateRuleKind::JulianWithLeapDay;
            rule.day  = static_cast<std::uint16_t>(*day);
        }

        if (consume('/')) {
            auto const transition_time = signed_time();
            if (!transition_time) {
                return std::nullopt;
            }
            rule.transition_time_seconds = *transition_time;
        }
        return rule;
    }

private:
    std::string_view _text;
    std::size_t      _position{};
};

auto parse_posix_footer(std::string_view footer) -> std::optional<std::optional<PosixFutureRule>>
{
    if (footer.empty()) {
        return std::optional<PosixFutureRule>{};
    }

    PosixReader reader{footer};
    auto const  standard_abbreviation = reader.abbreviation();
    auto const  standard_offset       = reader.utc_offset();
    if (!standard_abbreviation || !standard_offset) {
        return std::nullopt;
    }

    PosixFutureRule parsed{
        .standard_abbreviation       = *standard_abbreviation,
        .standard_utc_offset_seconds = *standard_offset,
        .daylight                    = std::nullopt,
    };
    if (reader.at_end()) {
        return parsed;
    }

    auto const daylight_abbreviation = reader.abbreviation();
    if (!daylight_abbreviation) {
        return std::nullopt;
    }

    std::int32_t daylight_offset{};
    if (reader.peek() == ',' || reader.at_end()) {
        auto const candidate = static_cast<std::int64_t>(*standard_offset) + 3600;
        if (candidate < kMinimumUtcOffset || candidate > kMaximumUtcOffset) {
            return std::nullopt;
        }
        daylight_offset = static_cast<std::int32_t>(candidate);
    }
    else {
        auto const explicit_offset = reader.utc_offset();
        if (!explicit_offset) {
            return std::nullopt;
        }
        daylight_offset = *explicit_offset;
    }

    if (!reader.consume(',')) {
        return std::nullopt;
    }
    auto const start = reader.date_rule();
    if (!start || !reader.consume(',')) {
        return std::nullopt;
    }
    auto const end = reader.date_rule();
    if (!end || !reader.at_end()) {
        return std::nullopt;
    }

    parsed.daylight = PosixDaylightRule{
        .abbreviation       = *daylight_abbreviation,
        .utc_offset_seconds = daylight_offset,
        .start              = *start,
        .end                = *end,
    };
    return parsed;
}

auto valid_timezone_name(std::string_view timezone) noexcept -> bool
{
    if (timezone.empty() || timezone.size() > 255 || !detail::is_valid_utf8(timezone) ||
        detail::has_ascii_control(timezone) || timezone.find('\\') != std::string_view::npos ||
        timezone.front() == '/') {
        return false;
    }

    std::size_t begin{};
    while (begin <= timezone.size()) {
        auto const separator = timezone.find('/', begin);
        auto const component =
            timezone.substr(begin, separator == std::string_view::npos ? separator : separator - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1;
    }
    return true;
}

auto is_below(std::filesystem::path const& root, std::filesystem::path const& candidate) -> bool
{
    // Components distinguish the root from string-prefix siblings such as
    // "zoneinfo-old".
    auto root_component      = root.begin();
    auto candidate_component = candidate.begin();
    for (; root_component != root.end(); ++root_component, ++candidate_component) {
        if (candidate_component == candidate.end() || *root_component != *candidate_component) {
            return false;
        }
    }
    return candidate_component != candidate.end();
}

} // namespace

auto parse_timezone_data(std::string_view bytes) -> jb::core::Result<TimezoneData, jb::core::Error>
{
    if (bytes.empty() || bytes.size() > kMaximumTimezoneFileSize) {
        return TimezoneResult::failure(invalid_timezone_data("file size"));
    }

    ByteReader reader{bytes};
    TzifHeader first_header;
    if (!read_header(reader, first_header)) {
        return TimezoneResult::failure(invalid_timezone_data("first header"));
    }

    auto first_section = parse_section(reader, first_header.counts, 4, first_header.version);
    if (!first_section) {
        return TimezoneResult::failure(invalid_timezone_data("32-bit section"));
    }

    if (first_header.version == '\0') {
        if (reader.remaining() != 0) {
            return TimezoneResult::failure(invalid_timezone_data("trailing v1 data"));
        }
        return TimezoneResult::success({
            .transitions             = std::move(first_section->transitions),
            .local_time_types        = std::move(first_section->local_time_types),
            .default_local_time_type = first_section->default_local_time_type,
            .future_rule             = std::nullopt,
        });
    }

    TzifHeader second_header;
    if (!read_header(reader, second_header) || second_header.version != first_header.version ||
        second_header.version == '\0') {
        return TimezoneResult::failure(invalid_timezone_data("64-bit header"));
    }

    // The compatibility section cannot authorize any allocation or cursor
    // movement in the independently checked preferred section.
    auto second_section = parse_section(reader, second_header.counts, 8, second_header.version);
    if (!second_section) {
        return TimezoneResult::failure(invalid_timezone_data("64-bit section"));
    }

    auto const footer_bytes = reader.remainder();
    if (footer_bytes.size() < 2 || footer_bytes.front() != '\n' || footer_bytes.back() != '\n' ||
        footer_bytes.substr(1, footer_bytes.size() - 2).find('\n') != std::string_view::npos) {
        return TimezoneResult::failure(invalid_timezone_data("POSIX footer framing"));
    }

    auto const future_rule = parse_posix_footer(footer_bytes.substr(1, footer_bytes.size() - 2));
    if (!future_rule) {
        return TimezoneResult::failure(invalid_timezone_data("POSIX footer"));
    }

    return TimezoneResult::success({
        .transitions             = std::move(second_section->transitions),
        .local_time_types        = std::move(second_section->local_time_types),
        .default_local_time_type = second_section->default_local_time_type,
        .future_rule             = std::move(*future_rule),
    });
}

auto load_timezone_data(std::string_view timezone) -> jb::core::Result<TimezoneData, jb::core::Error>
{
    static auto const roots = std::array{
        std::filesystem::path{"/usr/share/zoneinfo"},
        std::filesystem::path{"/usr/share/lib/zoneinfo"},
    };
    return load_timezone_data(timezone, roots);
}

auto load_timezone_data(std::string_view timezone, std::span<std::filesystem::path const> roots)
    -> jb::core::Result<TimezoneData, jb::core::Error>
{
    if (!valid_timezone_name(timezone)) {
        return TimezoneResult::failure(invalid_timezone());
    }

    std::optional<std::filesystem::path> resolved;
    for (auto const& root : roots) {
        std::error_code error;
        auto const      canonical_root = std::filesystem::canonical(root, error);
        if (error) {
            continue;
        }

        auto const candidate           = canonical_root / std::filesystem::path{std::string{timezone}};
        auto const canonical_candidate = std::filesystem::canonical(candidate, error);
        if (error || !is_below(canonical_root, canonical_candidate)) {
            continue;
        }
        if (!std::filesystem::is_regular_file(canonical_candidate, error) || error) {
            continue;
        }

        resolved = canonical_candidate;
        break;
    }

    if (!resolved) {
        return TimezoneResult::failure(invalid_timezone());
    }

    jb::core::File file;
    if (!file.open(*resolved, jb::core::OpenMode::ReadOnly)) {
        return TimezoneResult::failure(invalid_timezone_data("file open"));
    }

    auto const size = file.size();
    if (file.error() != jb::core::IOError::NoError || size > kMaximumTimezoneFileSize) {
        return TimezoneResult::failure(invalid_timezone_data("file size"));
    }

    auto bytes = file.read_all();
    if (file.error() != jb::core::IOError::NoError || bytes.size() != size) {
        return TimezoneResult::failure(invalid_timezone_data("file read"));
    }
    return parse_timezone_data(bytes);
}

} // namespace jb::jobu
