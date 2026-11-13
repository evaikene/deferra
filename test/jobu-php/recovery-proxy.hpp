#pragma once

#include <chrono>
#include <jb/core/event_loop.hpp>
#include <jb/core/json.hpp>
#include <jb/core/object.hpp>
#include <jb/core/signal.hpp>
#include <jb/core/timer.hpp>
#include <jb/net/local_server.hpp>
#include <jb/net/local_socket.hpp>
#include <jb/rpc/framing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jobu_php_test {

/// Test-only frame proxy. Dropping a successful daemon reply proves the mutation
/// committed before transport loss; recorded parameters prove exact replay.
/// Links remain owned until fixture teardown, including from inside callbacks.
class RecoveryProxy final {
public:
    RecoveryProxy(std::string const& endpoint, std::string upstream)
        : _upstream(std::move(upstream))
    {
        REQUIRE(_listener.listen(endpoint));
        _listener.new_connection.connect(&_receiver, [this] {
            while (auto socket = _listener.take_next_connection()) {
                ++connections;
                _links.push_back(std::make_unique<Link>(*this, std::move(socket)));
            }
        });
    }

    /// Delay each history detail reply to verify explicit history-operation deadlines.
    std::chrono::milliseconds                                     history_reply_delay{0};
    /// Delay output and latest-attempt replies to verify a shared multi-read deadline.
    std::chrono::milliseconds                                     output_reply_delay{0};
    /// Separate delays prove reconnect and administration share one deadline.
    std::chrono::milliseconds                                     handshake_reply_delay{0};
    std::chrono::milliseconds                                     administration_reply_delay{0};
    /// Delay statistics reads independently from reconnect handshakes.
    std::chrono::milliseconds                                     statistics_reply_delay{0};
    std::string                                                   drop_method;
    int                                                           drops{0};
    int                                                           connections{0};
    bool                                                          incompatible{false};
    bool                                                          silent{false};
    bool                                                          close_after_response{false};
    std::string                                                   error_method;
    std::optional<jb::core::JsonValue>                            response_error;
    std::function<void()>                                         after_drop;
    /// Optional successful-reply fault injection for Page decode/boundary tests.
    std::function<void(std::string const&, jb::core::JsonValue&)> transform_result;
    std::map<std::string, std::vector<jb::core::JsonValue>>       requests;
    std::vector<jb::core::JsonValue>                              dropped_results;

    auto stop_accepting() -> void { _listener.close(); }

    auto disconnect() -> void
    {
        for (auto const& link : _links) {
            link->close();
        }
    }

private:
    class Link {
    public:
        Link(RecoveryProxy& proxy, std::unique_ptr<jb::net::LocalSocket> socket)
            : _proxy(proxy)
            , _downstream(std::move(socket))
        {
            _reply_timer.timeout.connect(&_receiver, [this] {
                if (_downstream->state() == jb::net::LocalSocketState::Connected) {
                    CHECK(_downstream->write(_delayed_frame) == _delayed_frame.size());
                }
                _delayed_frame.clear();
            });
            _downstream->ready_read.connect(&_receiver, [this] { receive_request(); });
            _downstream->disconnected.connect(&_receiver, [this] { _upstream.abort(); });
            _upstream.disconnected.connect(&_receiver, [this] { _downstream->abort(); });
            _upstream.ready_read.connect(&_receiver, [this] { receive_response(); });
            _upstream.connected.connect(&_receiver, [this] {
                if (!_buffer.empty()) {
                    CHECK(_upstream.write(_buffer) == _buffer.size());
                    _buffer.clear();
                }
            });
            _upstream.connect_to_server(proxy._upstream);
            receive_request();
        }

        auto close() -> void
        {
            _downstream->abort();
            _upstream.abort();
        }

    private:
        auto receive_request() -> void
        {
            auto frames = _requests.append(_downstream->read_all());
            REQUIRE(frames);
            for (auto const& body : frames.value()) {
                auto json = jb::core::parse_json(body);
                REQUIRE(json);
                auto const& fields = json->as_object();
                _method            = fields.at("method").as_string();
                _proxy.requests[_method].push_back(fields.contains("params") ? fields.at("params")
                                                                             : jb::core::JsonValue{});
                auto frame = jb::rpc::frame_message(body);
                REQUIRE(frame);
                if (_upstream.state() == jb::net::LocalSocketState::Connected) {
                    CHECK(_upstream.write(frame.value()) == frame->size());
                }
                else {
                    _buffer += frame.value();
                }
            }
        }

        auto receive_response() -> void
        {
            auto frames = _responses.append(_upstream.read_all());
            REQUIRE(frames);
            for (auto const& body : frames.value()) {
                auto json = jb::core::parse_json(body);
                REQUIRE(json);
                auto& fields = std::get<jb::core::JsonValue::Object>(json.value().data);
                if (_proxy.silent) {
                    continue;
                }
                if (_proxy.drops > 0 && _method == _proxy.drop_method) {
                    REQUIRE(fields.contains("result"));
                    _proxy.dropped_results.push_back(fields.at("result"));
                    --_proxy.drops;
                    close();
                    if (_proxy.after_drop) {
                        _proxy.after_drop();
                    }
                    return;
                }
                if (_proxy.incompatible && _method == "system.info") {
                    auto incompatible = jb::core::parse_json(
                        R"({"daemon_version":"incompatible","api_version":{"major":2,"minor":7},"capabilities":[]})");
                    REQUIRE(incompatible);
                    fields.at("result") = std::move(incompatible).value();
                }
                if (_proxy.response_error && _method == _proxy.error_method) {
                    fields.erase("result");
                    fields["error"] = *_proxy.response_error;
                }
                if (_proxy.transform_result && fields.contains("result")) {
                    _proxy.transform_result(_method, fields.at("result"));
                }
                auto encoded = jb::core::serialize_json(json.value());
                REQUIRE(encoded);
                auto frame = jb::rpc::frame_message(encoded.value());
                REQUIRE(frame);
                auto delay = std::chrono::milliseconds{0};
                if (_method == "attempt.get" || _method == "run.get") {
                    delay = _proxy.history_reply_delay;
                }
                else if (_method == "attempt.output" || _method == "attempt.list") {
                    delay = _proxy.output_reply_delay;
                }
                else if (_method == "system.stats" || _method == "queue.stats") {
                    delay = _proxy.statistics_reply_delay;
                }
                else if (_method == "system.info") {
                    delay = _proxy.handshake_reply_delay;
                }
                else if (_method == "secret.list" || _method == "schedule.validate" || _method == "schedule.next") {
                    delay = _proxy.administration_reply_delay;
                }
                if (delay.count() > 0) {
                    // The synchronous client has at most one outstanding request
                    // on this link. The timer owns its bytes without blocking the loop.
                    _delayed_frame = frame.value();
                    _reply_timer.start(delay);
                    continue;
                }
                CHECK(_downstream->write(frame.value()) == frame->size());
                if (_proxy.close_after_response) {
                    // Drain the complete frame before EOF, in this same event-loop callback.
                    _downstream->disconnect_from_server();
                    return;
                }
            }
        }

        RecoveryProxy&                        _proxy;
        std::unique_ptr<jb::net::LocalSocket> _downstream;
        jb::net::LocalSocket                  _upstream;
        jb::rpc::StreamFramer                 _requests;
        jb::rpc::StreamFramer                 _responses;
        std::string                           _buffer;
        std::string                           _method;
        jb::core::Object                      _receiver;
        jb::core::Timer                       _reply_timer;
        std::string                           _delayed_frame;
    };

    std::string                        _upstream;
    jb::net::LocalServer               _listener;
    std::vector<std::unique_ptr<Link>> _links;
    jb::core::Object                   _receiver;
};

} // namespace jobu_php_test
