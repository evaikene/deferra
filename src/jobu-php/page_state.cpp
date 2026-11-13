#include "page_state.hpp"

#include <utility>

namespace jb::php {
PageState::PageState(::php::Value client, PageSource source, PageBatch first) noexcept
    : _client(std::move(client))
    , _source(std::move(source))
    , _page(std::move(first))
{}

auto PageState::trace(::php::GcVisitor& visitor) const noexcept -> void
{
    visitor.add(_client.view());
    visitor.add(_page.items.view());
    visitor.add(_page.statistics_metadata.view());
}

auto PageState::client() const noexcept -> ::php::ValueView
{
    return _client.view();
}

auto PageState::has_next() const noexcept -> bool
{
    if (_before_first) {
        return !_page.items.view().get_array().empty();
    }
    return !std::holds_alternative<std::monostate>(_page.continuation);
}

auto PageState::items() const -> ::php::Value
{
    if (_before_first) {
        zval empty;
        array_init(&empty);
        return ::php::Value::adopt(empty);
    }
    return ::php::Value::retain(_page.items.view());
}

auto PageState::statistics_metadata() const -> ::php::Value
{
    return ::php::Value::retain(_page.statistics_metadata.view());
}

auto PageState::next(core::TimePoint deadline) -> ::php::Result<void>
{
    if (_before_first) {
        _before_first = false;
        return {};
    }

    auto replacement = _source(_client.view(), _page.continuation, deadline);
    if (!replacement) {
        return replacement.error();
    }

    // Install all fields before releasing old items. Zend cleanup may observe
    // the owner through a cycle; it must never see mixed pages and boundaries.
    auto previous = std::move(_page);
    _page         = std::move(replacement).value();
    return {};
}
} // namespace jb::php
