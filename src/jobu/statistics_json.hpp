/// @file statistics_json.hpp
/// @brief Strict statistics request codecs and owning response conversion.
///
#pragma once

#include "management.hpp"
#include "result.hpp"
#include "statistics.hpp"

namespace jb::jobu {

/// An initial queue statistics request names one existing queue before the service binds its stable ID.
struct QueueStatisticsQuery {
    QueueSelector     selector;
    StatisticsRequest statistics;
};

/// A queue statistics continuation contains only its opaque cursor and no selector.
using QueueStatisticsListRequest = std::variant<QueueStatisticsQuery, CursorRequest>;

/// Encodes an initial system-wide query or cursor-only continuation.
[[nodiscard]] auto system_statistics_request_to_json(StatisticsListRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes system.stats params; unknown members and fields beside cursor are invalid.
[[nodiscard]] auto system_statistics_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<StatisticsListRequest, jb::core::Error>;

/// Encodes an initial queue selector with statistics filters, or a cursor-only continuation.
[[nodiscard]] auto queue_statistics_request_to_json(QueueStatisticsListRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes queue.stats params. Initial requests require exactly one queue selector.
[[nodiscard]] auto queue_statistics_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<QueueStatisticsListRequest, jb::core::Error>;

/// Encodes bounded groups, resolved window, nullable cursor, and explicit measurement provenance.
[[nodiscard]] auto statistics_page_to_json(StatisticsPage const& page)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes a statistics page into owning values, ignoring unknown response members.
[[nodiscard]] auto statistics_page_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<StatisticsPage, jb::core::Error>;

} // namespace jb::jobu
