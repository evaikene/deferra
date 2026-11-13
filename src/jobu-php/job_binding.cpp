#include "job_binding.hpp"
#include "history_binding.hpp"

#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "client_binding.hpp"
#include "http_payload_binding.hpp"
#include "queue_binding.hpp"
#include "resource_state.hpp"
#include "run_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/text_validation.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/management_json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
using State = ResourceState<jobu::JobDefinition>;

auto payload_to_php(jobu::JobType type, core::JsonValue const& value) -> ::php::Result<::php::Value>
{
    return type == jobu::JobType::Cli ? cli_payload_to_php(value) : http_payload_to_php(value);
}

auto payload_from_php(jobu::JobType type, ::php::ValueView value) -> ::php::Result<core::JsonValue>
{
    return type == jobu::JobType::Cli ? cli_payload_from_php(value) : http_payload_from_php(value);
}

auto decode_job(jobu::ControlReply const& reply) -> ::php::Result<jobu::JobDefinition>
{
    if (auto const* value = std::get_if<jobu::JobDefinition>(&reply)) {
        return *value;
    }
    return snapshot::invalid("Expected a job reply");
}

template <typename Request>
auto job_operation(std::string name,
                   Operation::Accepted (jobu::ControlClient::*method)(Request const&, jobu::ControlCallOptions),
                   Request request) -> ::php::Result<Operation>
{
    auto result = Operation::bind(std::move(name), method, std::move(request));
    if (result) {
        return std::move(result).value();
    }
    if (result.error().category == core::ErrorCategory::InvalidArgument) {
        return snapshot::invalid(result.error().message);
    }
    ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Unable to prepare the JobU operation"},
                 snapshot::entry("jobu\\exception"));
    auto* entry = snapshot::entry("jobu\\exception");
    zend_update_property_string(entry, EG(exception), ZEND_STRL("_errorCode"), result.error().code.c_str());
    zend_update_property_string(entry, EG(exception), ZEND_STRL("_category"), category_name(result.error().category));
    return ::php::pending_exception();
}

auto nullable_string(::php::Arguments const& args, std::uint32_t position) -> ::php::Result<std::optional<std::string>>
{
    if (!snapshot::present(args.get(position))) {
        return std::optional<std::string>{};
    }
    auto text = args.string(position);
    if (!text) {
        return text.error();
    }
    return std::optional{std::string(text.value().bytes())};
}

auto job_name(::php::Arguments const& args, std::uint32_t position) -> ::php::Result<std::optional<std::string>>
{
    auto name = nullable_string(args, position);
    if (!name) {
        return name.error();
    }
    if (name.value() && (name.value()->size() > 256 || !core::is_valid_utf8(*name.value()) ||
                         name.value()->find('\0') != std::string::npos)) {
        return snapshot::invalid("Job name must be UTF-8 without NUL, at most 256 bytes");
    }
    return name;
}

auto priority_argument(::php::Arguments const& args, std::uint32_t position) -> ::php::Result<std::int32_t>
{
    auto value = args.integer(position);
    if (!value) {
        return value.error();
    }
    if (value.value() < std::numeric_limits<std::int32_t>::min() ||
        value.value() > std::numeric_limits<std::int32_t>::max()) {
        return snapshot::invalid("Priority must fit a signed 32-bit integer");
    }
    return static_cast<std::int32_t>(value.value());
}

class Job final {
public:
    Job(::php::Value        client,
        jobu::JobDefinition value,
        ::php::Value        attributes,
        ::php::Value        payload,
        ::php::Value        schedule) noexcept
        : _state(std::move(client), std::move(value))
        , _attributes(std::move(attributes))
        , _payload(std::move(payload))
        , _schedule(std::move(schedule))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void
    {
        _state.trace(visitor);
        visitor.add(_attributes.view());
        visitor.add(_payload.view());
        visitor.add(_baseline_payload.view());
        visitor.add(_schedule.view());
    }

    auto state() noexcept -> State& { return _state; }

    auto attributes() const noexcept -> ::php::ValueView { return _attributes.view(); }

    auto payload() const noexcept -> ::php::ValueView { return _payload.view(); }

    auto schedule() const noexcept -> ::php::ValueView { return _schedule.view(); }

    auto attach(::php::ValueView owner) -> ::php::Result<void>
    {
        // Children retain this owner, allowing bookkeeping callbacks to borrow
        // native state. The callbacks never run PHP or replace child objects.
        auto attributes = attach_attributes(
            Z_OBJ_P(_attributes.get()),
            owner,
            [this] {
                auto values  = attribute_snapshot(_attributes.view());
                auto changed = update_job_attributes(_state, std::move(values).value());
                (void)changed;
            },
            [this] { return _state.editable(); });
        if (!attributes) {
            return attributes.error();
        }
        auto baseline = payload_to_php(_state.baseline().type, _state.baseline().payload);
        if (!baseline) {
            return baseline.error();
        }
        _baseline_payload = std::move(baseline).value();
        auto notify       = [this] {
            bool equal = _state.current().type == jobu::JobType::Cli
                           ? equal_cli_payload(_payload.view(), _baseline_payload.view())
                           : equal_http_payload(_payload.view(), _baseline_payload.view());
            _state.track("payload", !equal);
        };
        auto guard = [this] { return _state.editable(); };
        return _state.current().type == jobu::JobType::Cli
                 ? attach_cli_payload(Z_OBJ_P(_payload.get()), owner, notify, guard)
                 : attach_http_payload(Z_OBJ_P(_payload.get()), owner, notify, guard);
    }

    auto restore(jobu::JobDefinition const& value, ::php::ValueView source) -> ::php::Result<ConfigurationRestore>
    {
        auto attributes = prepare_attributes_restore(Z_OBJ_P(_attributes.get()), value.attributes);
        if (!attributes) {
            return attributes.error();
        }
        auto payload = value.type == jobu::JobType::Cli ? prepare_cli_payload_restore(Z_OBJ_P(_payload.get()), source)
                                                        : prepare_http_payload_restore(Z_OBJ_P(_payload.get()), source);
        if (!payload) {
            return payload.error();
        }
        attributes.value().append(std::move(payload).value());
        return std::move(attributes).value();
    }

    auto accept(jobu::JobDefinition value) -> ::php::Result<void>
    {
        // Schedule construction may allocate Zend values. Keep it outside the
        // prepared restoration closure, which must not retain or execute PHP.
        auto schedule = schedule_to_php(value.schedule);
        if (!schedule) {
            return schedule.error();
        }
        auto baseline_payload = payload_to_php(value.type, value.payload);
        if (!baseline_payload) {
            return baseline_payload.error();
        }
        auto accepted = _state.accept(std::move(value), [this, &baseline_payload](auto const& snapshot) {
            return restore(snapshot, baseline_payload.value().view());
        });
        if (!accepted) {
            return accepted.error();
        }
        _schedule         = std::move(schedule).value();
        _baseline_payload = std::move(baseline_payload).value();
        return {};
    }

    auto discard() -> ::php::Result<void>
    {
        auto schedule = schedule_to_php(_state.baseline().schedule);
        if (!schedule) {
            return schedule.error();
        }
        auto source = payload_to_php(_state.baseline().type, _state.baseline().payload);
        if (!source) {
            return source.error();
        }
        auto restored =
            _state.discard([this, &source](auto const& snapshot) { return restore(snapshot, source.value().view()); });
        if (!restored) {
            return restored.error();
        }
        _schedule = std::move(schedule).value();
        return {};
    }

    auto set_schedule(::php::ValueView source) -> ::php::Result<void>
    {
        auto value = update_schedule_from_php(source);
        if (!value) {
            return value.error();
        }
        auto allowed = _state.editable();
        if (!allowed) {
            return allowed.error();
        }
        auto baseline = schedule_to_php(_state.baseline().schedule);
        if (!baseline) {
            return baseline.error();
        }
        auto equal = zend_compare(const_cast<zval*>(source.get()), const_cast<zval*>(baseline.value().get())) == 0;
        _schedule  = ::php::Value::retain(source);
        _state.track("schedule", !equal);
        return {};
    }

    auto save(core::TimePoint deadline) -> ::php::Result<void>
    {
        auto allowed = _state.editable();
        if (!allowed) {
            return allowed.error();
        }
        if (!_state.dirty()) {
            return {};
        }
        auto const&            baseline = _state.baseline();
        auto const&            current  = _state.current();
        auto const&            edits    = _state.edits();
        jobu::UpdateJobRequest request{.job_id = baseline.id, .expected_revision = baseline.revision};
        if (edits.contains("name")) {
            request.name.emplace(current.name);
        }
        if (edits.contains("priority")) {
            request.priority = current.priority;
        }
        if (edits.contains("schedule")) {
            auto value = update_schedule_from_php(_schedule.view());
            if (!value) {
                return value.error();
            }
            request.schedule = std::move(value).value();
        }
        auto attributes = attributes_from_php(_attributes.view());
        if (!attributes) {
            return attributes.error();
        }
        auto patch = changed_attributes(baseline.attributes, attributes.value());
        if (!patch) {
            return patch.error();
        }
        request.attribute_changes = std::move(patch).value();
        if (edits.contains("payload")) {
            auto value = payload_from_php(current.type, _payload.view());
            if (!value) {
                return value.error();
            }
            request.payload = std::move(value).value();
        }
        auto call = job_operation("job.update", &jobu::ControlClient::update_job, std::move(request));
        if (!call) {
            return call.error();
        }
        auto reply = resource_call(_state.client(), deadline, call.value());
        if (!reply) {
            return reply.error();
        }
        auto value = decode_job(reply.value());
        if (!value) {
            return value.error();
        }
        return accept(std::move(value).value());
    }

private:
    State        _state;
    ::php::Value _attributes;
    ::php::Value _payload;
    ::php::Value _baseline_payload;
    ::php::Value _schedule;
};

using Binding = ::php::ObjectBinding<Job>;

auto make_job(::php::ValueView client, jobu::JobDefinition value) -> ::php::Result<::php::Value>
{
    auto identity = resource_identity(value.id, value.id, value.revision);
    if (!identity) {
        return identity.error();
    }
    auto attributes = job_attributes_to_php(value.type, value.attributes);
    if (!attributes) {
        return attributes.error();
    }
    auto payload = payload_to_php(value.type, value.payload);
    if (!payload) {
        return payload.error();
    }
    auto schedule = schedule_to_php(value.schedule);
    if (!schedule) {
        return schedule.error();
    }
    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\job"));
    auto owner  = ::php::Value::adopt(slot);
    auto native = Binding::construct(Z_OBJ_P(owner.get()),
                                     ::php::Value::retain(client),
                                     std::move(value),
                                     std::move(attributes).value(),
                                     std::move(payload).value(),
                                     std::move(schedule).value());
    if (!native) {
        return native.error();
    }
    auto attached = native.value()->attach(owner.view());
    if (!attached) {
        return attached.error();
    }
    return owner;
}

// Zend entry boundaries translate recoverable failures through Result. Allocation
// failure is fatal here and must not unwind through Zend frames.
auto get_job(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        return count.error();
    }
    auto text = args.string(1);
    if (!text) {
        return text.error();
    }
    auto id = uuid_from_php(args.get(1));
    if (!id) {
        return id.error();
    }
    if (id.value().is_nil()) {
        return snapshot::invalid("Job ID must not be nil");
    }

    auto queue = queue_state(Z_OBJ(frame->This));
    if (!queue) {
        return queue.error();
    }
    auto owner  = queue.value()->client();
    auto client = native_client(Z_OBJ_P(owner.get()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    auto call = job_operation("job.get", &jobu::ControlClient::get_job, id.value());
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(owner, deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    auto value = decode_job(reply.value());
    if (!value) {
        return value.error();
    }
    auto identity = resource_identity(id.value(), value.value().id, value.value().revision);
    if (!identity) {
        return identity.error();
    }

    // Lookup preserves the Queue's local state. The returned definition must
    // belong to the receiver's stable identity, even if a Job has moved.
    if (value.value().queue_id != queue.value()->current().id) {
        ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Job not found in this queue"},
                     snapshot::entry("jobu\\notfoundexception"));
        return ::php::pending_exception();
    }
    return make_job(owner, std::move(value).value());
}

auto create_job(zend_execute_data* frame, jobu::JobType type) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(2, 7);
    if (!count) {
        return count.error();
    }

    auto queue = queue_state(Z_OBJ(frame->This));
    if (!queue) {
        return queue.error();
    }
    auto allowed = queue.value()->lifecycle_allowed();
    if (!allowed) {
        return allowed.error();
    }
    jobu::CreateJobRequest request{.queue = queue.value()->baseline().id, .type = type};
    auto                   payload = payload_from_php(type, args.get(1));
    if (!payload) {
        return payload.error();
    }
    request.payload = std::move(payload).value();
    auto schedule   = creation_schedule_from_php(args.get(2));
    if (!schedule) {
        return schedule.error();
    }
    request.schedule = std::move(schedule).value();
    auto name        = job_name(args, 3);
    if (!name) {
        return name.error();
    }
    request.name = std::move(name).value();
    if (args.size() >= 4) {
        auto priority = priority_argument(args, 4);
        if (!priority) {
            return priority.error();
        }
        request.priority = priority.value();
    }
    if (snapshot::present(args.get(5))) {
        auto attributes = attributes_from_php(args.get(5));
        if (!attributes) {
            return attributes.error();
        }
        auto valid = jobu::validate_job_attributes(jobu::StandardAttributeRegistry{}, attributes.value(), type);
        if (!valid) {
            return snapshot::invalid(valid.error().message);
        }
        request.attributes = std::move(attributes).value();
    }
    auto key = nullable_string(args, 6);
    if (!key) {
        return key.error();
    }
    request.idempotency_key = std::move(key).value();

    // Argument coercion can call user code. Recheck admission before freezing the
    // operation, then use the same UUID, key and inputs throughout reconnection.
    allowed = queue.value()->lifecycle_allowed();
    if (!allowed) {
        return allowed.error();
    }
    auto client = native_client(Z_OBJ_P(queue.value()->client().get()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 7, start);
    if (!deadline) {
        return deadline.error();
    }

    auto call = job_operation("job.create", &jobu::ControlClient::create_job, std::move(request));
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(queue.value()->client(), deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    auto value = decode_job(reply.value());
    if (!value) {
        return value.error();
    }
    if (value.value().type != type || value.value().queue_id != queue.value()->baseline().id) {
        return snapshot::invalid("Unexpected job creation identity");
    }
    return make_job(queue.value()->client(), std::move(value).value());
}

auto clone_configuration(::php::ValueView value) -> ::php::Result<::php::Value>
{
    auto* object = Z_OBJ_P(value.get());
    zval  slot{};
    ZVAL_OBJ(&slot, object->handlers->clone_obj(object));
    auto owner = ::php::Value::adopt(slot);
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    return owner;
}

auto read_job(zend_execute_data* frame,
              std::string_view   field,
              snapshot::Kind     kind,
              std::string_view   enumeration) noexcept -> ::php::Result<::php::Value>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& job = *native.value();
    if (field == "attributes") {
        return clone_configuration(job.attributes());
    }
    if (field == "payload") {
        return clone_configuration(job.payload());
    }
    if (field == "attached_attributes") {
        return ::php::Value::retain(job.attributes());
    }
    if (field == "attached_payload") {
        return ::php::Value::retain(job.payload());
    }
    if (field == "schedule") {
        return ::php::Value::retain(job.schedule());
    }
    if (field == "dirty" || field == "deleted") {
        zval slot{};
        ZVAL_BOOL(&slot, field == "dirty" ? job.state().dirty() : job.state().deleted());
        return ::php::Value::adopt(slot);
    }
    // Scalar getters must work even when local attributes are temporarily
    // inconsistent. Encode only the accepted snapshot, replacing edited scalars.
    auto value     = job.state().baseline();
    value.name     = job.state().current().name;
    value.priority = job.state().current().priority;
    auto encoded   = jobu::job_to_json(value, jobu::StandardAttributeRegistry{});
    if (!encoded) {
        return snapshot::invalid("Invalid cached job snapshot");
    }
    return snapshot::project(snapshot::field(encoded.value(), field), kind, enumeration);
}

auto edit_job(zend_execute_data* frame, std::string_view field) noexcept -> ::php::Result<void>
{
    ::php::Arguments args(frame);
    auto             count = args.count(1, 1);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& job     = *native.value();
    auto  allowed = job.state().editable();
    if (!allowed) {
        return allowed.error();
    }
    if (field == "name") {
        auto name = job_name(args, 1);
        if (!name) {
            return name.error();
        }
        return job.state().set("name", &jobu::JobDefinition::name, std::move(name).value());
    }
    if (field == "priority") {
        auto priority = priority_argument(args, 1);
        if (!priority) {
            return priority.error();
        }
        return job.state().set("priority", &jobu::JobDefinition::priority, priority.value());
    }
    if (field == "schedule") {
        return job.set_schedule(args.get(1));
    }
    if (field == "attributes") {
        return merge_attributes(Z_OBJ_P(job.attributes().get()), args.get(1));
    }
    return job.state().current().type == jobu::JobType::Cli
             ? replace_cli_payload(Z_OBJ_P(job.payload().get()), args.get(1))
             : replace_http_payload(Z_OBJ_P(job.payload().get()), args.get(1));
}

auto get_job_run(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        return count.error();
    }
    auto text = args.string(1);
    if (!text) {
        return text.error();
    }
    auto id = uuid_from_php(args.get(1));
    if (!id) {
        return id.error();
    }
    if (id.value().is_nil()) {
        return snapshot::invalid("Run ID must not be nil");
    }

    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto const& state    = native.value()->state();
    auto        deadline = history_deadline(state.client(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    auto run = read_run_details(state.client(), id.value(), deadline.value());
    if (!run) {
        return run.error();
    }

    // Parent membership comes from the returned history snapshot. Do not look
    // up the live Job: its retained runs remain readable after owner deletion.
    if (run.value().job_id != state.current().id) {
        ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Run not found in this job"},
                     snapshot::entry("jobu\\notfoundexception"));
        return ::php::pending_exception();
    }
    return run_to_php(state.client(), std::move(run).value());
}

auto job_runs(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto start  = core::Clock::now();
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto const& state = native.value()->state();
    // History remains readable for dirty/deleted owners; it never applies local edits.
    return list_run_history(state.client(), ::php::Arguments(frame), start, state.current().id);
}

auto job_rpc(zend_execute_data* frame, std::string_view action) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    bool             move    = action == "move";
    bool             run     = action == "runNow";
    auto             maximum = move || run ? 2U : 1U;
    if (action == "discard") {
        maximum = 0;
    }
    auto count = args.count(move ? 1 : 0, maximum);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& job   = *native.value();
    auto& state = job.state();
    if (action == "discard") {
        auto result = job.discard();
        if (!result) {
            return result.error();
        }
        return ::php::Value::null();
    }
    auto allowed = action == "save" ? state.editable() : state.lifecycle_allowed();
    if (!allowed) {
        return allowed.error();
    }
    auto client = native_client(Z_OBJ_P(state.client().get()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, move || run ? 2 : 1, start);
    if (!deadline) {
        return deadline.error();
    }
    if (action == "save") {
        auto saved = job.save(deadline.value());
        if (!saved) {
            return saved.error();
        }
        return ::php::Value::null();
    }

    auto make = [&]() -> ::php::Result<Operation> {
        auto const id = state.baseline().id;
        if (move) {
            auto object = args.object(1, snapshot::entry("jobu\\queue"));
            if (!object) {
                return object.error();
            }
            auto target = queue_state(const_cast<zend_object*>(object.value()));
            if (!target) {
                return target.error();
            }
            auto guard = target.value()->lifecycle_allowed();
            if (!guard) {
                return guard.error();
            }
            if (Z_OBJ_P(target.value()->client().get()) != Z_OBJ_P(state.client().get())) {
                return snapshot::invalid("Move destination must belong to the same Client");
            }
            return job_operation("job.move",
                                 &jobu::ControlClient::move_job,
                                 jobu::MoveJobRequest{.job_id            = id,
                                                      .expected_revision = state.baseline().revision,
                                                      .target_queue      = target.value()->baseline().id});
        }
        if (run) {
            auto key = nullable_string(args, 1);
            if (!key) {
                return key.error();
            }
            auto guard = state.lifecycle_allowed();
            if (!guard) {
                return guard.error();
            }
            return job_operation("job.run_now",
                                 &jobu::ControlClient::run_now,
                                 jobu::RunNowRequest{.job_id = id, .idempotency_key = std::move(key).value()});
        }
        if (action == "delete") {
            return job_operation("job.delete",
                                 &jobu::ControlClient::delete_job,
                                 jobu::DeleteJobRequest{.job_id = id, .expected_revision = state.baseline().revision});
        }
        if (action == "suspend") {
            return job_operation("job.suspend", &jobu::ControlClient::suspend_job, id);
        }
        if (action == "resume") {
            return job_operation("job.resume", &jobu::ControlClient::resume_job, id);
        }
        return job_operation("job.get", &jobu::ControlClient::get_job, id);
    };
    auto call = make();
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(state.client(), deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    if (run) {
        auto const* value = std::get_if<jobu::RunDetails>(&reply.value());
        if (!value || value->job_id != state.baseline().id || value->type != state.baseline().type) {
            return snapshot::invalid("Unexpected Run Now reply");
        }
        return run_to_php(state.client(), *value);
    }
    if (action == "delete") {
        if (!std::holds_alternative<jobu::EmptyReply>(reply.value())) {
            return snapshot::invalid("Expected a deletion reply");
        }
        state.mark_deleted();
        return ::php::Value::null();
    }
    auto value = decode_job(reply.value());
    if (!value) {
        return value.error();
    }
    auto accepted = job.accept(std::move(value).value());
    if (!accepted) {
        return accepted.error();
    }
    return ::php::Value::null();
}
} // namespace

auto job_to_php(::php::ValueView client, jobu::JobDefinition value) -> ::php::Result<::php::Value>
{
    return make_job(client, std::move(value));
}

auto bind_job_class(zend_class_entry* entry) -> bool
{
    entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(entry));
}
} // namespace jb::php

ZEND_METHOD(JobU_Queue, getJob)
{
    jb::php::snapshot::return_value(jb::php::get_job(execute_data), return_value);
}

ZEND_METHOD(JobU_Queue, createCliJob)
{
    jb::php::snapshot::return_value(jb::php::create_job(execute_data, jb::jobu::JobType::Cli), return_value);
}

ZEND_METHOD(JobU_Queue, createHttpJob)
{
    jb::php::snapshot::return_value(jb::php::create_job(execute_data, jb::jobu::JobType::Http), return_value);
}

ZEND_METHOD(JobU_Job, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Job resources are created by JobU"});
}

ZEND_METHOD(JobU_Job, getId)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getQueueId)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "queue_id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getRevision)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "revision", jb::php::snapshot::Kind::Integer, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getName)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "name", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getPriority)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "priority", jb::php::snapshot::Kind::Integer, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getState)
{
    jb::php::snapshot::return_value(
        jb::php::read_job(execute_data, "state", jb::php::snapshot::Kind::Enum, "jobu\\jobstate"),
        return_value);
}

ZEND_METHOD(JobU_Job, getType)
{
    jb::php::snapshot::return_value(
        jb::php::read_job(execute_data, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype"),
        return_value);
}

ZEND_METHOD(JobU_Job, getCreatedAt)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "created_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getUpdatedAt)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "updated_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getDeletedAt)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "deleted_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getSchedule)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "schedule", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, getAttributes)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "attributes", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, attributes)
{
    jb::php::snapshot::return_value(
        jb::php::read_job(execute_data, "attached_attributes", jb::php::snapshot::Kind::Generic, ""),
        return_value);
}

ZEND_METHOD(JobU_Job, getPayload)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "payload", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, payload)
{
    jb::php::snapshot::return_value(
        jb::php::read_job(execute_data, "attached_payload", jb::php::snapshot::Kind::Generic, ""),
        return_value);
}

ZEND_METHOD(JobU_Job, isDirty)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "dirty", jb::php::snapshot::Kind::Boolean, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, isDeleted)
{
    jb::php::snapshot::return_value(jb::php::read_job(execute_data, "deleted", jb::php::snapshot::Kind::Boolean, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Job, setName)
{
    (void)return_value;
    auto result = jb::php::edit_job(execute_data, "name");
    if (!result) {
        php::raise(result.error());
    }
}

ZEND_METHOD(JobU_Job, setPriority)
{
    (void)return_value;
    auto result = jb::php::edit_job(execute_data, "priority");
    if (!result) {
        php::raise(result.error());
    }
}

ZEND_METHOD(JobU_Job, setSchedule)
{
    (void)return_value;
    auto result = jb::php::edit_job(execute_data, "schedule");
    if (!result) {
        php::raise(result.error());
    }
}

ZEND_METHOD(JobU_Job, setAttributes)
{
    (void)return_value;
    auto result = jb::php::edit_job(execute_data, "attributes");
    if (!result) {
        php::raise(result.error());
    }
}

ZEND_METHOD(JobU_Job, setPayload)
{
    (void)return_value;
    auto result = jb::php::edit_job(execute_data, "payload");
    if (!result) {
        php::raise(result.error());
    }
}

ZEND_METHOD(JobU_Job, save)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "save"), return_value);
}

ZEND_METHOD(JobU_Job, refresh)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "refresh"), return_value);
}

ZEND_METHOD(JobU_Job, discardChanges)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "discard"), return_value);
}

ZEND_METHOD(JobU_Job, suspend)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "suspend"), return_value);
}

ZEND_METHOD(JobU_Job, resume)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "resume"), return_value);
}

ZEND_METHOD(JobU_Job, move)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "move"), return_value);
}

ZEND_METHOD(JobU_Job, delete)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "delete"), return_value);
}

ZEND_METHOD(JobU_Job, runNow)
{
    jb::php::snapshot::return_value(jb::php::job_rpc(execute_data, "runNow"), return_value);
}

ZEND_METHOD(JobU_Job, listRuns)
{
    jb::php::snapshot::return_value(jb::php::job_runs(execute_data), return_value);
}

ZEND_METHOD(JobU_Job, getRun)
{
    jb::php::snapshot::return_value(jb::php::get_job_run(execute_data), return_value);
}
