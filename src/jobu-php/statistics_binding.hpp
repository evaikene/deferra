#pragma once

#include <jb/jobu/statistics.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

namespace jb::php {
/// Project complete, detached group snapshots; reject counts outside PHP's integer range.
/// The borrowed Client is unused: neither groups nor their children retain a session.
auto statistics_groups_to_php(::php::ValueView client, jobu::StatisticsPage const& page) -> ::php::Result<::php::Value>;
/// Copy resolved window, grouping and provenance, including for an empty grouped page.
auto statistics_metadata_to_php(jobu::StatisticsPage const& page) -> ::php::Result<::php::Value>;
} // namespace jb::php
