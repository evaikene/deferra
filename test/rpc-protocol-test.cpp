#include "protocol.hpp"
#include "protocol_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace jb::core;
using namespace jb::rpc;
using namespace jb::rpc::detail;

namespace {

template <typename T>
auto make_json(T value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto make_request(std::optional<JsonValue> id     = {},
                  std::string              method = "example",
                  std::optional<JsonValue> params = {}) -> JsonValue
{
    auto object = JsonValue::Object{
        {"jsonrpc", make_json(std::string{"2.0"})},
        {"method",  make_json(std::move(method)) },
    };
    if (id) {
        object.emplace("id", std::move(*id));
    }
    if (params) {
        object.emplace("params", std::move(*params));
    }
    return make_json(std::move(object));
}

auto mutable_object(JsonValue& value) -> JsonValue::Object&
{
    return std::get<JsonValue::Object>(value.data);
}

auto require_request(JsonValue const& value) -> RequestEnvelope
{
    auto const document = decode_request_document(value);
    REQUIRE(document.kind == RequestDocumentKind::Single);
    REQUIRE(document.entries.size() == 1U);
    REQUIRE(std::holds_alternative<RequestEnvelope>(document.entries.front()));
    return std::get<RequestEnvelope>(document.entries.front());
}

auto check_invalid_request(JsonValue const& value) -> void
{
    auto const document = decode_request_document(value);
    REQUIRE(document.entries.size() == 1U);
    CHECK(std::holds_alternative<InvalidRequest>(document.entries.front()));
}

auto require_single_response(JsonValue const& value) -> ResponseEnvelope
{
    auto const document = decode_response_document(value);
    REQUIRE(document);
    REQUIRE(document->kind == ResponseDocumentKind::Single);
    REQUIRE(document->entries.size() == 1U);
    return document->entries.front();
}

auto check_protocol_error(JsonValue const& value, std::size_t max_batch_entries = default_max_batch_entries) -> void
{
    auto const result = decode_response_document(value, max_batch_entries);
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "rpc.protocol_error");
    CHECK(result.error().message == "The peer sent an invalid JSON-RPC response");
    CHECK(result.error().detail.empty());
}

auto responses_for(RequestDocument const& document) -> std::vector<JsonValue>
{
    auto responses = std::vector<JsonValue>{};
    for (auto const& entry : document.entries) {
        if (std::holds_alternative<InvalidRequest>(entry)) {
            responses.push_back(encode_error_response(NullRequestId{}, make_standard_error(ErrorCode::InvalidRequest)));
            continue;
        }

        auto const& request = std::get<RequestEnvelope>(entry);
        if (request.id) {
            responses.push_back(encode_success_response(*request.id, JsonValue{}));
        }
    }
    return responses;
}

} // anonymous namespace

TEST_CASE("Protocol public values preserve their documented alternatives", "[rpc][protocol]")
{
    static_assert(static_cast<std::int64_t>(ErrorCode::ParseError) == -32700);
    static_assert(static_cast<std::int64_t>(ErrorCode::InvalidRequest) == -32600);
    static_assert(static_cast<std::int64_t>(ErrorCode::MethodNotFound) == -32601);
    static_assert(static_cast<std::int64_t>(ErrorCode::InvalidParams) == -32602);
    static_assert(static_cast<std::int64_t>(ErrorCode::InternalError) == -32603);
    static_assert(static_cast<std::int64_t>(ErrorCode::ApplicationError) == -32000);
    static_assert(std::is_same_v<ConnectionId, std::uint64_t>);

    auto const null_id     = RequestId{NullRequestId{}};
    auto const signed_id   = RequestId{std::int64_t{-7}};
    auto const unsigned_id = RequestId{std::uint64_t{7}};
    auto const string_id   = RequestId{std::string{"7"}};
    CHECK(null_id.index() == 0U);
    CHECK(signed_id.index() == 1U);
    CHECK(unsigned_id.index() == 2U);
    CHECK(string_id.index() == 3U);

    RpcError const default_error;
    CHECK(default_error.code == -32603);
    CHECK(default_error.message.empty());
    CHECK_FALSE(default_error.data);

    PeerIdentity const   default_peer;
    RequestContext const default_context;
    CHECK_FALSE(default_peer.process_id);
    CHECK_FALSE(default_peer.user_id);
    CHECK_FALSE(default_peer.group_id);
    CHECK(default_context.connection_id == 0U);
    CHECK_FALSE(default_context.operation.authenticated_principal);

    RequestContext const context{
        .connection_id = 9U,
        .operation =
            {
                        .peer                    = {.process_id = 1U, .user_id = 2U, .group_id = 3U},
                        .authenticated_principal = std::string{"principal"},
                        },
    };
    CHECK(context.connection_id == 9U);
    CHECK(context.operation.peer.process_id == 1U);
    CHECK(context.operation.authenticated_principal == "principal");

    MethodHandler handler = [](RequestContext const& request, std::optional<JsonValue> const& params) {
        if (request.connection_id == 0U) {
            return MethodResult::failure({.message = "missing connection"});
        }
        return MethodResult::success(params.value_or(JsonValue{}));
    };
    auto success = handler(context, make_json(std::string{"owned"}));
    REQUIRE(success);
    CHECK(success->as_string() == "owned");

    auto failure = handler(RequestContext{}, std::nullopt);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().message == "missing connection");
}

TEST_CASE("Application errors map every category without transmitting detail", "[rpc][protocol]")
{
    auto const cases = std::array{
        std::pair{ErrorCategory::InvalidArgument,   std::string_view{"invalid_argument"}  },
        std::pair{ErrorCategory::NotFound,          std::string_view{"not_found"}         },
        std::pair{ErrorCategory::Conflict,          std::string_view{"conflict"}          },
        std::pair{ErrorCategory::PermissionDenied,  std::string_view{"permission_denied"} },
        std::pair{ErrorCategory::Unavailable,       std::string_view{"unavailable"}       },
        std::pair{ErrorCategory::ResourceExhausted, std::string_view{"resource_exhausted"}},
        std::pair{ErrorCategory::Cancelled,         std::string_view{"cancelled"}         },
        std::pair{ErrorCategory::Timeout,           std::string_view{"timeout"}           },
        std::pair{ErrorCategory::Io,                std::string_view{"io"}                },
        std::pair{ErrorCategory::Unsupported,       std::string_view{"unsupported"}       },
        std::pair{ErrorCategory::Internal,          std::string_view{"internal"}          },
    };

    for (auto const& [category, expected_name] : cases) {
        CAPTURE(expected_name);
        auto const error = application_error({
            .category = category,
            .code     = "jobu.stable_code",
            .message  = "Safe message",
            .detail   = "secret-detail-marker",
        });

        CHECK(error.code == -32000);
        CHECK(error.message == "Safe message");
        REQUIRE(error.data);
        REQUIRE(error.data->is_object());
        auto const& data = error.data->as_object();
        REQUIRE(data.size() == 2U);
        CHECK(data.at("category").as_string() == expected_name);
        CHECK(data.at("code").as_string() == "jobu.stable_code");

        auto encoded = serialize_json(encode_error_response(NullRequestId{}, error));
        REQUIRE(encoded);
        CHECK(encoded->find("secret-detail-marker") == std::string::npos);
    }
}

TEST_CASE("Standard errors and reserved method detection use stable values", "[rpc][protocol]")
{
    auto const cases = std::array{
        std::pair{ErrorCode::ParseError,     std::string_view{"Parse error"}     },
        std::pair{ErrorCode::InvalidRequest, std::string_view{"Invalid Request"} },
        std::pair{ErrorCode::MethodNotFound, std::string_view{"Method not found"}},
        std::pair{ErrorCode::InvalidParams,  std::string_view{"Invalid params"}  },
        std::pair{ErrorCode::InternalError,  std::string_view{"Internal error"}  },
    };
    for (auto const& [code, message] : cases) {
        auto const error = make_standard_error(code);
        CHECK(error.code == static_cast<std::int64_t>(code));
        CHECK(error.message == message);
        CHECK_FALSE(error.data);
    }

    CHECK(is_reserved_method("rpc.internal"));
    CHECK_FALSE(is_reserved_method("RPC.internal"));
    CHECK_FALSE(is_reserved_method("rpc"));
    CHECK_FALSE(is_reserved_method(""));
}

TEST_CASE("Request codecs preserve IDs, notification absence, and parameters", "[rpc][protocol]")
{
    auto const ids = std::array<RequestId, 5>{
        RequestId{std::int64_t{-1}},
        RequestId{std::uint64_t{1}},
        RequestId{std::string{"id"}},
        RequestId{std::string{}},
        RequestId{NullRequestId{}},
    };
    for (auto const& id : ids) {
        auto const  encoded = encode_request(id, "example");
        auto const& decoded = require_request(encoded);
        REQUIRE(decoded.id);
        CHECK(*decoded.id == id);
        CHECK(decoded.method == "example");
        CHECK_FALSE(decoded.params);
    }

    auto const  notification         = encode_notification("notify");
    auto const& decoded_notification = require_request(notification);
    CHECK_FALSE(decoded_notification.id);

    auto const object_params = make_json(JsonValue::Object{
        {"value", make_json(true)}
    });
    auto const array_params  = make_json(JsonValue::Array{make_json(std::uint64_t{1})});
    CHECK(require_request(encode_request(std::uint64_t{1}, "object", object_params)).params == object_params);
    CHECK(require_request(encode_notification("array", array_params)).params == array_params);

    CHECK(require_request(encode_notification("", std::nullopt)).method.empty());
    CHECK(require_request(encode_notification("rpc.structurally_valid")).method == "rpc.structurally_valid");

    auto encoded = serialize_json(encode_request(std::uint64_t{1}, "example", object_params));
    REQUIRE(encoded);
    CHECK(*encoded == R"({"id":1,"jsonrpc":"2.0","method":"example","params":{"value":true}})");
}

TEST_CASE("Request decoding rejects malformed envelopes without throwing", "[rpc][protocol]")
{
    SECTION("invalid roots")
    {
        check_invalid_request(make_json(JsonNull{}));
        check_invalid_request(make_json(true));
        check_invalid_request(make_json(std::string{"request"}));
    }

    SECTION("invalid protocol versions")
    {
        auto missing = make_request();
        mutable_object(missing).erase("jsonrpc");
        check_invalid_request(missing);

        auto wrong_type                       = make_request();
        mutable_object(wrong_type)["jsonrpc"] = make_json(std::uint64_t{2});
        check_invalid_request(wrong_type);

        auto wrong_value                       = make_request();
        mutable_object(wrong_value)["jsonrpc"] = make_json(std::string{"2.1"});
        check_invalid_request(wrong_value);

        auto wrong_case = make_request();
        mutable_object(wrong_case).erase("jsonrpc");
        mutable_object(wrong_case).emplace("Jsonrpc", make_json(std::string{"2.0"}));
        check_invalid_request(wrong_case);
    }

    SECTION("invalid methods")
    {
        auto missing = make_request();
        mutable_object(missing).erase("method");
        check_invalid_request(missing);

        auto wrong_type                      = make_request();
        mutable_object(wrong_type)["method"] = make_json(true);
        check_invalid_request(wrong_type);

        auto wrong_case = make_request();
        mutable_object(wrong_case).erase("method");
        mutable_object(wrong_case).emplace("Method", make_json(std::string{"example"}));
        check_invalid_request(wrong_case);
    }

    SECTION("primitive parameters")
    {
        auto const values = std::array{
            make_json(JsonNull{}),
            make_json(false),
            make_json(std::int64_t{-1}),
            make_json(std::uint64_t{1}),
            make_json(1.5),
            make_json(std::string{"params"}),
        };
        for (auto const& params : values) {
            check_invalid_request(make_request({}, "example", params));
        }
    }

    SECTION("invalid identifiers")
    {
        auto const values = std::array{
            make_json(false),
            make_json(1.5),
            make_json(JsonValue::Array{}),
            make_json(JsonValue::Object{}),
        };
        for (auto const& id : values) {
            check_invalid_request(make_request(id));
        }
    }

    SECTION("unknown members")
    {
        auto request = make_request(make_json(std::uint64_t{1}));
        mutable_object(request).emplace("future", make_json(std::string{"ignored"}));
        auto const& decoded = require_request(request);
        REQUIRE(decoded.id);
        CHECK(*decoded.id == RequestId{std::uint64_t{1}});
    }
}

TEST_CASE("Success response codecs preserve every JSON value and supported ID", "[rpc][protocol]")
{
    auto const values = std::array{
        make_json(JsonNull{                         }
        ),
        make_json(false),
        make_json(std::int64_t{-1                        }
        ),
        make_json(std::uint64_t{1                         }
        ),
        make_json(1.5),
        make_json(std::string{"value"                   }
        ),
        make_json(JsonValue::Array{make_json(true)           }
        ),
        make_json(JsonValue::Object{{"value", make_json(true)}}
        ),
    };
    for (auto const& value : values) {
        auto const& response = require_single_response(encode_success_response(std::uint64_t{1}, value));
        REQUIRE(std::holds_alternative<JsonValue>(response.payload));
        CHECK(std::get<JsonValue>(response.payload) == value);
    }

    auto const ids = std::array<RequestId, 4>{
        RequestId{NullRequestId{}},
        RequestId{std::int64_t{-1}},
        RequestId{std::uint64_t{1}},
        RequestId{std::string{"id"}},
    };
    for (auto const& id : ids) {
        CHECK(require_single_response(encode_success_response(id, JsonValue{})).id == id);
    }

    auto encoded = serialize_json(encode_success_response(std::string{"id"}, make_json(JsonNull{})));
    REQUIRE(encoded);
    CHECK(*encoded == R"({"id":"id","jsonrpc":"2.0","result":null})");
}

TEST_CASE("Error response codecs preserve code, message, and optional data", "[rpc][protocol]")
{
    auto const  absent_data = RpcError{.code = -32001, .message = "Absent"};
    auto const& absent      = require_single_response(encode_error_response(std::uint64_t{1}, absent_data));
    REQUIRE(std::holds_alternative<RpcError>(absent.payload));
    CHECK_FALSE(std::get<RpcError>(absent.payload).data);

    auto const null_data     = RpcError{.code = -32002, .message = "Null", .data = JsonValue{}};
    auto       encoded       = encode_error_response(std::uint64_t{2}, null_data);
    auto&      encoded_error = mutable_object(mutable_object(encoded).at("error"));
    encoded_error.emplace("future", make_json(std::string{"ignored"}));
    mutable_object(encoded).emplace("future", make_json(true));

    auto const& decoded = require_single_response(encoded);
    REQUIRE(std::holds_alternative<RpcError>(decoded.payload));
    auto const& error = std::get<RpcError>(decoded.payload);
    REQUIRE(error.data);
    CHECK(error.data->is_null());

    auto serialized = serialize_json(encoded);
    REQUIRE(serialized);
    CHECK(
        *serialized ==
        R"({"error":{"code":-32002,"data":null,"future":"ignored","message":"Null"},"future":true,"id":2,"jsonrpc":"2.0"})");
}

TEST_CASE("Response decoding accepts integral code boundaries", "[rpc][protocol]")
{
    auto make_error_response = [](JsonValue code) {
        return make_json(JsonValue::Object{
            {"error",
             make_json(JsonValue::Object{
                 {"code", std::move(code)},
                 {"message", make_json(std::string{"message"})},
             })                                      },
            {"id",      make_json(std::uint64_t{1})  },
            {"jsonrpc", make_json(std::string{"2.0"})},
        });
    };

    auto const& signed_response = require_single_response(make_error_response(make_json(std::int64_t{-32000})));
    CHECK(std::get<RpcError>(signed_response.payload).code == -32000);

    auto const  maximum  = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    auto const& boundary = require_single_response(make_error_response(make_json(maximum)));
    CHECK(std::get<RpcError>(boundary.payload).code == std::numeric_limits<std::int64_t>::max());

    check_protocol_error(make_error_response(make_json(maximum + 1U)));
}

TEST_CASE("Response decoding reports one stable local error for malformed envelopes", "[rpc][protocol]")
{
    auto valid = encode_success_response(std::uint64_t{1}, make_json(std::string{"secret-input-marker"}));

    auto missing_version = valid;
    mutable_object(missing_version).erase("jsonrpc");
    check_protocol_error(missing_version);

    auto wrong_version                       = valid;
    mutable_object(wrong_version)["jsonrpc"] = make_json(std::string{"1.0"});
    check_protocol_error(wrong_version);

    auto missing_id = valid;
    mutable_object(missing_id).erase("id");
    check_protocol_error(missing_id);

    auto invalid_id                  = valid;
    mutable_object(invalid_id)["id"] = make_json(1.5);
    check_protocol_error(invalid_id);

    auto missing_payload = valid;
    mutable_object(missing_payload).erase("result");
    check_protocol_error(missing_payload);

    auto both_payloads = valid;
    mutable_object(both_payloads).emplace("error", make_json(JsonValue::Object{}));
    check_protocol_error(both_payloads);

    auto const invalid_errors = std::array{
        make_json(JsonNull{                                             }
        ),
        make_json(JsonValue::Object{         }
        ),
        make_json(JsonValue::Object{{"code", make_json(std::int64_t{-1})}}
        ),
        make_json(JsonValue::Object{{"message", make_json(std::string{"message"})}                      }
        ),
        make_json(JsonValue::Object{
                           {"code", make_json(1.5)},
                           {"message", make_json(std::string{"message"})},
                           }
        ),
        make_json(JsonValue::Object{
                           {"code", make_json(std::int64_t{-1})},
                           {"message", make_json(true)},
                           }
        ),
    };
    for (auto const& error : invalid_errors) {
        auto response = valid;
        mutable_object(response).erase("result");
        mutable_object(response).emplace("error", error);
        check_protocol_error(response);
    }

    auto invalid_with_marker = valid;
    mutable_object(invalid_with_marker).erase("id");
    auto marker_result = decode_response_document(invalid_with_marker);
    REQUIRE_FALSE(marker_result);
    CHECK(marker_result.error().code.find("secret-input-marker") == std::string::npos);
    CHECK(marker_result.error().message.find("secret-input-marker") == std::string::npos);
    CHECK(marker_result.error().detail.find("secret-input-marker") == std::string::npos);
}

TEST_CASE("Request batches preserve order and isolate invalid entries", "[rpc][protocol]")
{
    auto const batch    = make_json(JsonValue::Array{
        encode_request(std::uint64_t{1}, "request"),
        encode_notification("notification"),
        make_json(true),
        make_json(JsonValue::Array{make_request()}),
    });
    auto const document = decode_request_document(batch);
    REQUIRE(document.kind == RequestDocumentKind::Batch);
    REQUIRE(document.entries.size() == 4U);
    CHECK(std::get<RequestEnvelope>(document.entries[0]).method == "request");
    CHECK(std::get<RequestEnvelope>(document.entries[1]).method == "notification");
    CHECK(std::holds_alternative<InvalidRequest>(document.entries[2]));
    CHECK(std::holds_alternative<InvalidRequest>(document.entries[3]));

    auto responses = responses_for(document);
    REQUIRE(responses.size() == 3U);
    auto encoded = encode_batch(std::move(responses));
    REQUIRE(encoded);
    REQUIRE(encoded->is_array());
    CHECK(encoded->as_array().size() == 3U);

    auto malformed_with_id = make_request(make_json(std::string{"must-not-be-reused"}));
    mutable_object(malformed_with_id).erase("method");
    auto invalid_document  = decode_request_document(malformed_with_id);
    auto invalid_responses = responses_for(invalid_document);
    REQUIRE(invalid_responses.size() == 1U);
    auto const& invalid_response = require_single_response(invalid_responses.front());
    CHECK(invalid_response.id == RequestId{NullRequestId{}});
}

TEST_CASE("Request batches enforce inclusive configurable and default limits", "[rpc][protocol]")
{
    auto const empty = decode_request_document(make_json(JsonValue::Array{}));
    CHECK(empty.kind == RequestDocumentKind::RejectedBatch);
    REQUIRE(empty.entries.size() == 1U);
    CHECK(std::holds_alternative<InvalidRequest>(empty.entries.front()));

    auto one_entry = make_json(JsonValue::Array{make_request()});
    CHECK(decode_request_document(one_entry, 1U).kind == RequestDocumentKind::Batch);
    CHECK(decode_request_document(one_entry, 0U).kind == RequestDocumentKind::RejectedBatch);

    auto two_entries = make_json(JsonValue::Array{make_request(), make_request()});
    CHECK(decode_request_document(two_entries, 2U).kind == RequestDocumentKind::Batch);
    auto over_limit = decode_request_document(two_entries, 1U);
    CHECK(over_limit.kind == RequestDocumentKind::RejectedBatch);
    CHECK(over_limit.entries.size() == 1U);

    auto default_boundary = JsonValue::Array(64U, make_request());
    CHECK(decode_request_document(make_json(default_boundary)).kind == RequestDocumentKind::Batch);
    default_boundary.push_back(make_request());
    auto const rejected = decode_request_document(make_json(std::move(default_boundary)));
    CHECK(rejected.kind == RequestDocumentKind::RejectedBatch);
    CHECK(rejected.entries.size() == 1U);
}

TEST_CASE("Batch response shapes distinguish standalone errors, arrays, and no output", "[rpc][protocol]")
{
    auto const empty_request_batch = decode_request_document(make_json(JsonValue::Array{}));
    auto       empty_responses     = responses_for(empty_request_batch);
    REQUIRE(empty_responses.size() == 1U);
    CHECK(empty_responses.front().is_object());

    auto const mixed           = decode_request_document(make_json(JsonValue::Array{
        encode_request(std::uint64_t{1}, "request"),
        encode_notification("notification"),
    }));
    auto       mixed_responses = responses_for(mixed);
    REQUIRE(mixed_responses.size() == 1U);
    auto encoded_mixed = encode_batch(std::move(mixed_responses));
    REQUIRE(encoded_mixed);
    CHECK(encoded_mixed->is_array());
    CHECK(encoded_mixed->as_array().size() == 1U);

    auto const notifications = decode_request_document(make_json(JsonValue::Array{
        encode_notification("first"),
        encode_notification("second"),
    }));
    CHECK(responses_for(notifications).empty());
    CHECK_FALSE(encode_batch(responses_for(notifications)));
}

TEST_CASE("Response batches validate every entry and preserve order", "[rpc][protocol]")
{
    auto responses = std::vector<JsonValue>{
        encode_success_response(std::uint64_t{1}, make_json(std::string{"first"})),
        encode_error_response(std::string{"second"}, make_standard_error(ErrorCode::InternalError)),
    };
    auto encoded = encode_batch(responses);
    REQUIRE(encoded);

    auto decoded = decode_response_document(*encoded, 2U);
    REQUIRE(decoded);
    REQUIRE(decoded->kind == ResponseDocumentKind::Batch);
    REQUIRE(decoded->entries.size() == 2U);
    CHECK(decoded->entries[0].id == RequestId{std::uint64_t{1}});
    CHECK(decoded->entries[1].id == RequestId{std::string{"second"}});

    check_protocol_error(make_json(JsonValue::Array{}));
    check_protocol_error(*encoded, 1U);

    auto malformed_batch = encoded->as_array();
    malformed_batch[1]   = make_json(false);
    check_protocol_error(make_json(std::move(malformed_batch)));
}
