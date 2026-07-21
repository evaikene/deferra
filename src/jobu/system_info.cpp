#include "system_info.hpp"

#include "system_info_priv.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobu {

namespace {

auto make_json(auto value) -> jb::rpc::JsonValue
{
    jb::rpc::JsonValue result;
    result.data = std::move(value);
    return result;
}

auto canonical_capabilities(std::vector<std::string> capabilities) -> std::vector<std::string>
{
    std::ranges::sort(capabilities);
    auto const duplicates = std::ranges::unique(capabilities);
    capabilities.erase(duplicates.begin(), duplicates.end());
    return capabilities;
}

auto invalid_response() -> jb::core::Result<SystemInfo, jb::core::Error>
{
    return jb::core::Result<SystemInfo, jb::core::Error>::failure({
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.system_info.invalid_response",
        .message  = "The system.info response is invalid",
    });
}

auto find_member(jb::rpc::JsonValue::Object const& object, std::string const& name) -> jb::rpc::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto decode_version_component(jb::rpc::JsonValue const* value, std::uint32_t& result) -> bool
{
    if (!value || !value->is_uint() || value->as_uint() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    result = static_cast<std::uint32_t>(value->as_uint());
    return true;
}

} // anonymous namespace

auto system_info_to_json(SystemInfo const& info) -> jb::rpc::JsonValue
{
    auto capabilities = jb::rpc::JsonValue::Array{};
    for (auto& capability : canonical_capabilities(info.capabilities)) {
        capabilities.push_back(make_json(std::move(capability)));
    }

    auto api_version = jb::rpc::JsonValue::Object{
        {"major", make_json(static_cast<std::uint64_t>(info.api_version.major))},
        {"minor", make_json(static_cast<std::uint64_t>(info.api_version.minor))},
    };

    return make_json(jb::rpc::JsonValue::Object{
        {"api_version",    make_json(std::move(api_version))          },
        {"capabilities",   make_json(std::move(capabilities))         },
        {"daemon_version", make_json(std::string{info.daemon_version})},
    });
}

auto system_info_from_json(jb::rpc::JsonValue const& value) -> jb::core::Result<SystemInfo, jb::core::Error>
{
    using Result = jb::core::Result<SystemInfo, jb::core::Error>;

    if (!value.is_object()) {
        return invalid_response();
    }

    auto const& object         = value.as_object();
    auto const* daemon_version = find_member(object, "daemon_version");
    auto const* api_version    = find_member(object, "api_version");
    auto const* capabilities   = find_member(object, "capabilities");
    if (!daemon_version || !daemon_version->is_string() || !api_version || !api_version->is_object() || !capabilities ||
        !capabilities->is_array()) {
        return invalid_response();
    }

    auto        result     = SystemInfo{.daemon_version = daemon_version->as_string()};
    auto const& api_object = api_version->as_object();
    if (!decode_version_component(find_member(api_object, "major"), result.api_version.major) ||
        !decode_version_component(find_member(api_object, "minor"), result.api_version.minor)) {
        return invalid_response();
    }

    result.capabilities.reserve(capabilities->as_array().size());
    for (auto const& capability : capabilities->as_array()) {
        if (!capability.is_string()) {
            return invalid_response();
        }
        result.capabilities.push_back(capability.as_string());
    }
    result.capabilities = canonical_capabilities(std::move(result.capabilities));
    return Result::success(std::move(result));
}

namespace detail {

auto handle_system_info(SystemInfo const& info, std::optional<jb::rpc::JsonValue> const& params)
    -> jb::rpc::MethodResult
{
    if (params && (!params->is_object() || !params->as_object().empty())) {
        return jb::rpc::MethodResult::failure({
            .code    = static_cast<std::int64_t>(jb::rpc::ErrorCode::InvalidParams),
            .message = "Invalid params",
        });
    }
    return jb::rpc::MethodResult::success(system_info_to_json(info));
}

} // namespace detail

} // namespace jb::jobu
