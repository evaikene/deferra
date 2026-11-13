#include "resource_state.hpp"
#include "attribute_binding.hpp"
#include "client_binding.hpp"

#include <phpcxx/exception.hpp>

#include <limits>
#include <string_view>
#include <utility>

namespace jb::php {

auto check_configuration_edit(ConfigurationEditCheck const& check) -> ::php::Result<void>
{
    return check ? check() : ::php::Result<void>{};
}

ConfigurationRestore::ConfigurationRestore(std::function<void()> commit) noexcept
{
    if (commit) {
        _commits.push_back(std::move(commit));
    }
}

auto ConfigurationRestore::append(ConfigurationRestore replacement) -> void
{
    for (auto& commit : replacement._commits) {
        _commits.push_back(std::move(commit));
    }
}

auto ConfigurationRestore::commit() && noexcept -> void
{
    auto commits = std::move(_commits);
    for (auto& commit : commits) {
        commit();
    }
}

auto resource_deleted(jobu::Queue const& snapshot) noexcept -> bool
{
    return snapshot.state == jobu::QueueState::Deleted;
}

auto resource_deleted(jobu::JobDefinition const& snapshot) noexcept -> bool
{
    return snapshot.state == jobu::JobState::Deleted;
}

auto resource_guard(bool dirty, bool deleted) -> ::php::Result<void>
{
    if (!dirty && !deleted) {
        return {};
    }
    auto name =
        deleted ? std::string_view("jobu\\notfoundexception") : std::string_view("jobu\\unsavedchangesexception");
    auto* entry = static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
    ::php::raise({.kind    = ::php::ErrorKind::Runtime,
                  .message = deleted ? "The resource is locally deleted" : "Save or discard local changes first"},
                 entry);
    return ::php::pending_exception();
}

auto resource_identity(core::Uuid const& expected, core::Uuid const& received, std::uint64_t revision)
    -> ::php::Result<void>
{
    if (expected != received || received.is_nil() || revision == 0 ||
        revision > static_cast<std::uint64_t>(std::numeric_limits<zend_long>::max())) {
        return ::php::Error{.kind    = ::php::ErrorKind::Value,
                            .message = "Invalid resource snapshot identity or revision"};
    }
    return {};
}

auto resource_compatible(jobu::Queue const& expected, jobu::Queue const& received) -> ::php::Result<void>
{
    return resource_identity(expected.id, received.id, received.revision);
}

auto resource_compatible(jobu::JobDefinition const& expected, jobu::JobDefinition const& received)
    -> ::php::Result<void>
{
    if (expected.type != received.type) {
        return ::php::Error{.kind = ::php::ErrorKind::Value, .message = "A job resource cannot change runner type"};
    }
    return resource_identity(expected.id, received.id, received.revision);
}

auto update_job_attributes(ResourceState<jobu::JobDefinition>& state, jobu::AttributeSet values) -> ::php::Result<void>
{
    auto allowed = state.editable();
    if (!allowed) {
        return allowed.error();
    }
    auto changes = changed_attributes(state.baseline().attributes, values);
    if (!changes) {
        return changes.error();
    }

    // Recompute per-key differences from the accepted baseline. This removes
    // reverted edits while preserving all unrelated scalar/payload edit markers.
    auto old     = state.current().attributes;
    auto updated = state.set("attributes", &jobu::JobDefinition::attributes, std::move(values), equal_attributes);
    if (!updated) {
        return updated.error();
    }
    state.track("attributes", false);
    for (auto const& [name, value] : old) {
        state.track("attributes." + name, false);
    }
    for (auto const& [name, value] : changes.value()) {
        state.track("attributes." + name, true);
    }
    return {};
}

auto resource_call(::php::ValueView client, core::TimePoint deadline, Operation const& operation)
    -> ::php::Result<jobu::ControlReply>
{
    auto native = native_client(Z_OBJ_P(client.get()));
    if (!native) {
        return native.error();
    }
    auto reply = native.value()->call(deadline, operation);
    if (!reply) {
        raise_operation_failure(reply.error(), operation);
        return ::php::pending_exception();
    }
    return std::move(reply).value();
}

} // namespace jb::php
