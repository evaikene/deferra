#pragma once

#include "operation.hpp"

#include <jb/core/event_loop_types.hpp>
#include <jb/jobu/client/control_client.hpp>

#include <jb/core/uuid.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jb::php {

/// Pre-commit guard for an attached configuration. It may signal a PHP exception,
/// but must not execute user code or mutate configuration. Recheck after coercion.
using ConfigurationEditCheck = std::function<::php::Result<void>()>;
auto check_configuration_edit(ConfigurationEditCheck const& check) -> ::php::Result<void>;

/// A prepared, notification-free configuration replacement. Preparation owns all
/// native replacement values; commit only moves them into existing backing storage.
/// The target must outlive this temporary. Neither preparation nor commit may run
/// PHP/GC. Do not store Zend references in the closure or retain it across PHP calls.
class ConfigurationRestore final {
public:
    explicit ConfigurationRestore(std::function<void()> commit = {}) noexcept;
    ConfigurationRestore(ConfigurationRestore&&) noexcept                    = default;
    auto operator=(ConfigurationRestore&&) noexcept -> ConfigurationRestore& = default;
    ConfigurationRestore(ConfigurationRestore const&)                        = delete;
    auto operator=(ConfigurationRestore const&) -> ConfigurationRestore&     = delete;

    /// Combine independently prepared replacements without running them.
    auto append(ConfigurationRestore replacement) -> void;

    /// Consume once after every component of the replacement has been validated.
    auto commit() && noexcept -> void;

private:
    std::vector<std::function<void()>> _commits;
};

/// Return a pending PHP exception for a dirty/deleted guard. Requires a PHP frame.
auto resource_guard(bool dirty, bool deleted) -> ::php::Result<void>;
/// Validate native reply identity/revision before any local cache replacement.
auto resource_identity(core::Uuid const& expected, core::Uuid const& received, std::uint64_t revision)
    -> ::php::Result<void>;
/// Call through the originating Client and preserve the existing operation error mapping.
auto resource_call(::php::ValueView client, core::TimePoint deadline, Operation const& operation)
    -> ::php::Result<jobu::ControlReply>;

auto resource_compatible(jobu::Queue const& expected, jobu::Queue const& received) -> ::php::Result<void>;
auto resource_compatible(jobu::JobDefinition const& expected, jobu::JobDefinition const& received)
    -> ::php::Result<void>;

auto resource_deleted(jobu::Queue const& snapshot) noexcept -> bool;
auto resource_deleted(jobu::JobDefinition const& snapshot) noexcept -> bool;

/// Request-local state shared by later concrete resources. Snapshot is an owning
/// native DTO with id/revision members (Queue or JobDefinition), never Zend values.
/// Concrete bindings own/trace their attached configuration objects and provide a
/// prepare(snapshot) adapter returning ConfigurationRestore. That adapter validates
/// every replacement before commit; it must never execute PHP or change live state.
///
/// Only accepted replies advance the baseline. Failed calls/conversions leave the
/// baseline, working copy and tracked edits untouched, including uncertain saves.
/// Bind with ObjectBinding's default non-cloneable/non-serializable policy.
template <typename Snapshot>
class ResourceState final {
public:
    using Edits = std::set<std::string, std::less<>>;

    /// client must be an initialized originating PHP Client; snapshot must already
    /// have a non-nil identity and valid revision. Attach children after construction.
    ResourceState(::php::Value client, Snapshot snapshot) noexcept
        : _client(std::move(client))
        , _baseline(snapshot)
        , _current(std::move(snapshot))
        , _deleted(resource_deleted(_current))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto client() const noexcept -> ::php::ValueView { return _client.view(); }

    auto baseline() const noexcept -> Snapshot const& { return _baseline; }

    auto current() const noexcept -> Snapshot const& { return _current; }

    auto edits() const noexcept -> Edits const& { return _edits; }

    auto dirty() const noexcept -> bool { return !_edits.empty(); }

    auto deleted() const noexcept -> bool { return _deleted; }

    /// Check before local mutation, including attached child edits. Concrete
    /// adapters must repeat this after any argument conversion that executes PHP.
    auto editable() const -> ::php::Result<void> { return resource_guard(false, _deleted); }

    auto lifecycle_allowed() const -> ::php::Result<void> { return resource_guard(dirty(), _deleted); }

    /// Update one validated field. Equal assignments are clean; editing back to
    /// baseline removes the edit. Equality may be specialized for native DTOs.
    template <typename T, typename Equal = std::equal_to<>>
    auto set(std::string field, T Snapshot::* member, T value, Equal equal = {}) -> ::php::Result<void>
    {
        auto allowed = editable();
        if (!allowed) {
            return allowed.error();
        }
        track(std::move(field), !equal(_baseline.*member, value));
        _current.*member = std::move(value);
        return {};
    }

    /// Nested adapters use stable field/key names and compare against baseline.
    /// Notification is bookkeeping only: no PHP, RPC, or configuration mutation.
    auto track(std::string field, bool changed) -> void
    {
        if (changed) {
            _edits.insert(std::move(field));
        }
        else {
            _edits.erase(field);
        }
    }

    /// Called only after a successful deletion. Preserve cached values/revision;
    /// a null deletion reply does not contain a new revision token.
    auto mark_deleted() noexcept -> void { _deleted = true; }

    template <typename Prepare>
    auto discard(Prepare prepare) -> ::php::Result<void>
    {
        auto replacement = prepare(_baseline);
        if (!replacement) {
            return replacement.error();
        }
        auto restored = _baseline;
        std::move(replacement).value().commit();
        _current = std::move(restored);
        _edits.clear();
        return {};
    }

    /// Adopt a fully decoded reply. Identity is immutable and revisions are tokens,
    /// not locally incremented counters. Job runner retyping is rejected.
    template <typename Prepare>
    auto accept(Snapshot snapshot, Prepare prepare) -> ::php::Result<void>
    {
        auto identity = resource_compatible(_baseline, snapshot);
        if (!identity) {
            return identity.error();
        }
        auto replacement = prepare(snapshot);
        if (!replacement) {
            return replacement.error();
        }
        auto current = snapshot;

        // All fallible conversion is complete. No PHP or notifications can observe
        // a partial refresh between the attached objects and the native snapshots.
        std::move(replacement).value().commit();
        _baseline = std::move(snapshot);
        _deleted  = _deleted || resource_deleted(_baseline);
        _current  = std::move(current);
        _edits.clear();
        return {};
    }

    template <typename Decode, typename Prepare>
    auto refresh(core::TimePoint deadline, Operation const& operation, Decode decode, Prepare prepare)
        -> ::php::Result<void>
    {
        auto allowed = resource_guard(dirty(), false);
        if (!allowed) {
            return allowed.error();
        }
        return receive(deadline, operation, decode, prepare);
    }

    /// make freezes only tracked changes and the baseline revision into a typed
    /// Operation. It must not execute PHP. The existing bridge alone owns retries.
    template <typename Make, typename Decode, typename Prepare>
    auto save(core::TimePoint deadline, Make make, Decode decode, Prepare prepare) -> ::php::Result<void>
    {
        auto allowed = editable();
        if (!allowed) {
            return allowed.error();
        }
        if (!dirty()) {
            return {};
        }
        auto operation = make(_current, _baseline, _edits);
        if (!operation) {
            return operation.error();
        }
        return receive(deadline, operation.value(), decode, prepare);
    }

private:
    template <typename Decode, typename Prepare>
    auto receive(core::TimePoint deadline, Operation const& operation, Decode decode, Prepare prepare)
        -> ::php::Result<void>
    {
        auto reply = resource_call(_client.view(), deadline, operation);
        if (!reply) {
            return reply.error();
        }
        auto snapshot = decode(reply.value());
        if (!snapshot) {
            return snapshot.error();
        }
        return accept(std::move(snapshot).value(), prepare);
    }

    ::php::Value _client;
    Snapshot     _baseline;
    Snapshot     _current;
    Edits        _edits;
    bool         _deleted = false;
};

/// Synchronize an attached job attribute layer and track its changed keys.
/// Call after a local edit using attribute_snapshot(), without invoking PHP.
/// Missing baseline keys are rejected because job.update cannot remove attributes.
auto update_job_attributes(ResourceState<jobu::JobDefinition>& state, jobu::AttributeSet values) -> ::php::Result<void>;

} // namespace jb::php
