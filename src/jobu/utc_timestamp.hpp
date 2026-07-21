/** @file utc_timestamp.hpp
 * @brief Defines portable canonical UTC timestamp text conversion for JobU values.
 */
#pragma once

#include "error.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <string>
#include <string_view>

namespace jb::jobu {

/** Formats a UTC time point as canonical RFC 3339 text.
 *
 * Output uses `YYYY-MM-DDTHH:MM:SS.ffffffZ` with exactly six fractional digits and does not consult process timezone
 * state or a timezone database.
 *
 * @param value UTC time point to format.
 * @return Canonical text, or `jobu.time.out_of_range` when the value cannot use the four-digit-year representation.
 */
[[nodiscard]] auto format_utc_timestamp(jb::core::UtcTimePoint value) -> jb::core::Result<std::string, jb::core::Error>;

/** Parses strict RFC 3339 UTC text into a UTC time point.
 *
 * Input may omit the fraction or provide one through six digits, must end in `Z`, and must not contain a timezone
 * offset, leap second, impossible civil date, or trailing data.
 *
 * @param value Complete UTC timestamp text.
 * @return Parsed time, `jobu.time.invalid_format` for rejected syntax, or `jobu.time.out_of_range` when the value is
 * not representable by jb::core::UtcClock.
 */
[[nodiscard]] auto parse_utc_timestamp(std::string_view value)
    -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;

} // namespace jb::jobu
