#pragma once

#include <jb/core/event_loop_types.hpp>
#include <jb/core/uuid.hpp>
#include <jb/jobu/history.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <functional>
#include <string>
#include <variant>

namespace jb::php {
/// Absence ends enumeration; names and opaque cursors remain distinct from UUIDs.
using PageContinuation = std::variant<std::monostate, core::Uuid, std::string, jobu::CursorRequest>;

/// Fully prepared replacement. Items must be an indexed PHP array.
struct PageBatch {
    ::php::Value     items;
    PageContinuation continuation;
    ::php::Value     statistics_metadata{::php::Value::null()};
};

/// Private native adapter owning its initial query. Captures must contain no Zend
/// references or connection pointers; client is borrowed only during each call.
/// Fetching and projection must not execute PHP callbacks or mutate Page state.
using PageSource = std::function<::php::Result<PageBatch>(::php::ValueView, PageContinuation const&, core::TimePoint)>;

/// Request-local, before-first page position. Fetch/convert completes before any
/// state is replaced, so errors preserve both the current items and retry boundary.
class PageState final {
public:
    PageState(::php::Value client, PageSource source, PageBatch first) noexcept;
    auto trace(::php::GcVisitor& visitor) const noexcept -> void;
    auto client() const noexcept -> ::php::ValueView;
    auto has_next() const noexcept -> bool;
    /// Return a separate array reference, preserving contained object identities.
    auto items() const -> ::php::Value;
    /// Metadata describes the prefetched/current batch even when its items are empty.
    auto statistics_metadata() const -> ::php::Value;
    /// Requires has_next(); the PHP binding reports bounds errors before calling.
    auto next(core::TimePoint deadline) -> ::php::Result<void>;

private:
    ::php::Value _client;
    PageSource   _source;
    PageBatch    _page;
    bool         _before_first{true};
};
} // namespace jb::php
