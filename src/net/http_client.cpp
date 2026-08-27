#include "http_client.hpp"

namespace jb::net {

HttpClient::HttpClient(jb::core::Object* parent)
    : Object(parent)
{}

HttpClient::~HttpClient() = default;

} // namespace jb::net
