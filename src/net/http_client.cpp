#include "http_client.hpp"

namespace jb::net {

HttpClient::HttpClient(jb::core::Object* parent)
    : Object(parent)
{}

HttpClient::HttpClient(jb::core::priv::ObjectPrivate& dd, jb::core::Object* parent)
    : Object(dd, parent)
{}

HttpClient::~HttpClient() = default;

} // namespace jb::net
