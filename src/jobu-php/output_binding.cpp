#include "output_binding.hpp"

#include "history_binding.hpp"
#include "operation.hpp"
#include "resource_state.hpp"
#include "result_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/byte_buffer.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
constexpr zend_long maximum_bytes = 65'536;

auto byte_limit(::php::Arguments const& args, std::uint32_t position, zend_long fallback) -> ::php::Result<std::size_t>
{
    auto value = args.size() >= position ? args.integer(position) : ::php::Result<zend_long>(fallback);
    if (!value) {
        return value.error();
    }
    if (value.value() < 1 || value.value() > maximum_bytes) {
        return snapshot::invalid("Output byte limit must be between 1 and 65536");
    }
    return static_cast<std::size_t>(value.value());
}

auto channel_from_php(::php::ValueView value, jobu::JobType type) -> ::php::Result<jobu::OutputChannel>
{
    auto channel = snapshot::enum_text(value, "jobu\\outputchannel");
    if (!channel) {
        return channel.error();
    }
    if (type == jobu::JobType::Cli) {
        if (channel.value() == "stdout") {
            return jobu::OutputChannel::Stdout;
        }
        if (channel.value() == "stderr") {
            return jobu::OutputChannel::Stderr;
        }
    }
    else {
        if (channel.value() == "body") {
            return jobu::OutputChannel::Body;
        }
        if (channel.value() == "headers") {
            return jobu::OutputChannel::Headers;
        }
    }
    return snapshot::invalid("Output channel does not belong to this runner type");
}

auto read_chunk(::php::ValueView client, jobu::AttemptOutputRequest const& request, core::TimePoint deadline)
    -> ::php::Result<jobu::AttemptOutputChunk>
{
    auto operation = Operation::bind("attempt.output", &jobu::ControlClient::read_attempt_output, request);
    if (!operation) {
        return snapshot::invalid("Invalid output request");
    }
    auto reply = resource_call(client, deadline, operation.value());
    if (!reply) {
        return reply.error();
    }
    auto* chunk = std::get_if<jobu::AttemptOutputChunk>(&reply.value());
    if (!chunk || chunk->attempt.run_id != request.attempt.run_id ||
        chunk->attempt.attempt_number != request.attempt.attempt_number || chunk->channel != request.channel ||
        chunk->offset != request.offset || chunk->bytes_returned > request.limit) {
        return snapshot::invalid("Unexpected output reply identity or slice");
    }
    return std::move(*chunk);
}

auto latest_attempt(::php::ValueView client, core::Uuid const& run, core::TimePoint deadline)
    -> ::php::Result<std::optional<jobu::AttemptNumber>>
{
    // The native list is descending. Pin its first identity, without loading
    // details or chasing attempts that may be created after this observation.
    jobu::AttemptListRequest request = jobu::AttemptQuery{.run_id = run, .limit = 1};
    auto operation = Operation::bind("attempt.list", &jobu::ControlClient::list_attempts, std::move(request));
    if (!operation) {
        return snapshot::invalid("Invalid latest-attempt request");
    }
    auto reply = resource_call(client, deadline, operation.value());
    if (!reply) {
        return reply.error();
    }
    auto const* page = std::get_if<jobu::AttemptPage>(&reply.value());
    if (!page || page->items.size() > 1 || (!page->items.empty() && page->items.front().run_id != run)) {
        return snapshot::invalid("Unexpected latest-attempt reply");
    }
    if (page->items.empty()) {
        return std::optional<jobu::AttemptNumber>{};
    }
    return std::optional{page->items.front().attempt_number};
}

auto output_bytes(core::ByteBuffer const& bytes) -> ::php::Value
{
    zval result;
    // An empty vector can have a null data pointer; Zend's string constructor
    // requires a valid pointer even for zero bytes under sanitizer instrumentation.
    if (bytes.empty()) {
        ZVAL_EMPTY_STRING(&result);
    }
    else {
        auto view = core::as_string_view(bytes);
        ZVAL_STRINGL(&result, view.data(), view.size());
    }
    return ::php::Value::adopt(result);
}

auto channel_tail(::php::ValueView        client,
                  jobu::AttemptKey const& key,
                  jobu::OutputChannel     channel,
                  std::size_t             maximum,
                  core::TimePoint         deadline) -> ::php::Result<::php::Value>
{
    // A one-byte probe discovers availability and retained length without
    // transferring the unwanted prefix. Pending capture is never polled.
    jobu::AttemptOutputRequest request{.attempt = key, .channel = channel, .limit = 1};
    auto                       probe = read_chunk(client, request, deadline);
    if (!probe) {
        return probe.error();
    }
    auto const& observed = probe.value();
    if (observed.retained_bytes == 0) {
        return observed.status == jobu::OutputStatus::Available ? output_bytes({}) : ::php::Value::null();
    }

    auto length    = std::min<std::uint64_t>(observed.retained_bytes, maximum);
    request.offset = observed.retained_bytes - length;
    request.limit  = static_cast<std::size_t>(length);
    if (request.offset == 0 && observed.data.size() == length) {
        return output_bytes(observed.data);
    }

    // Completed retained output is immutable. Retention may remove its attempt
    // between reads; propagate that error instead of returning a misleading empty
    // result or starting over. Lost capture can still have useful retained bytes.
    auto tail = read_chunk(client, request, deadline);
    if (!tail) {
        return tail.error();
    }
    if (tail.value().retained_bytes != observed.retained_bytes || tail.value().data.size() != length) {
        return snapshot::invalid("Output changed between retained-tail reads");
    }
    return output_bytes(tail.value().data);
}
} // namespace

auto read_output(::php::ValueView        client,
                 jobu::AttemptKey const& key,
                 jobu::JobType           type,
                 ::php::Arguments const& args,
                 core::TimePoint         start) -> ::php::Result<::php::Value>
{
    auto count = args.count(1, 4);
    if (!count) {
        return count.error();
    }
    auto channel = channel_from_php(args.get(1), type);
    if (!channel) {
        return channel.error();
    }
    auto offset = args.size() >= 2 ? args.integer(2) : ::php::Result<zend_long>(0);
    if (!offset) {
        return offset.error();
    }
    // SQLite's retained slice uses a one-based signed position internally.
    if (offset.value() < 0 || offset.value() == std::numeric_limits<zend_long>::max()) {
        return snapshot::invalid("Output offset must be between 0 and PHP_INT_MAX - 1");
    }
    auto length = byte_limit(args, 3, 16'384);
    if (!length) {
        return length.error();
    }

    auto deadline = history_deadline(client, args, 4, start);
    if (!deadline) {
        return deadline.error();
    }

    jobu::AttemptOutputRequest request{.attempt = key,
                                       .channel = channel.value(),
                                       .offset  = static_cast<std::uint64_t>(offset.value()),
                                       .limit   = length.value()};
    auto                       chunk = read_chunk(client, request, deadline.value());
    if (!chunk) {
        return chunk.error();
    }
    return output_chunk_to_php(chunk.value());
}

auto get_output(::php::ValueView                   client,
                core::Uuid const&                  run,
                std::optional<jobu::AttemptNumber> number,
                jobu::JobType                      type,
                ::php::Arguments const&            args,
                core::TimePoint                    start) -> ::php::Result<::php::Value>
{
    auto count = args.count(0, 2);
    if (!count) {
        return count.error();
    }
    auto maximum = byte_limit(args, 1, 4096);
    if (!maximum) {
        return maximum.error();
    }

    auto deadline = history_deadline(client, args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    if (!number) {
        auto latest = latest_attempt(client, run, deadline.value());
        if (!latest) {
            return latest.error();
        }
        number = latest.value();
        if (!number) {
            return ::php::Value::null();
        }
    }

    jobu::AttemptKey key{.run_id = run, .attempt_number = *number};
    if (type == jobu::JobType::Http) {
        return channel_tail(client, key, jobu::OutputChannel::Body, maximum.value(), deadline.value());
    }
    auto stderr_tail = channel_tail(client, key, jobu::OutputChannel::Stderr, maximum.value(), deadline.value());
    if (!stderr_tail) {
        return stderr_tail.error();
    }
    if (!stderr_tail.value().is_null() && Z_STRLEN_P(stderr_tail.value().get()) != 0) {
        return std::move(stderr_tail).value();
    }

    auto stdout_tail = channel_tail(client, key, jobu::OutputChannel::Stdout, maximum.value(), deadline.value());
    if (!stdout_tail) {
        return stdout_tail.error();
    }
    // Preserve evidence of empty captured stderr when stdout is unavailable.
    return stdout_tail.value().is_null() ? std::move(stderr_tail).value() : std::move(stdout_tail).value();
}
} // namespace jb::php
