#include "curl_error_priv.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace jb::net::http::detail {

namespace {

auto http_error(HttpErrorKind kind, jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> HttpError
{
    return {
        .kind = kind,
        .error =
            {
                    .category = category,
                    .code     = std::string{code},
                    .message  = std::string{message},
                    },
    };
}

auto is_one_of(CURLcode result, std::initializer_list<CURLcode> values) noexcept -> bool
{
    for (auto value : values) {
        if (result == value) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

auto map_curl_error(CURLcode result) -> HttpError
{
    if (is_one_of(result, {CURLE_COULDNT_RESOLVE_PROXY, CURLE_COULDNT_RESOLVE_HOST})) {
        return http_error(HttpErrorKind::Resolve,
                          jb::core::ErrorCategory::Unavailable,
                          "net.http.resolve_failed",
                          "The HTTP host could not be resolved");
    }
    if (is_one_of(result,
                  {CURLE_COULDNT_CONNECT, CURLE_NO_CONNECTION_AVAILABLE, CURLE_QUIC_CONNECT_ERROR, CURLE_PROXY})) {
        return http_error(HttpErrorKind::Connect,
                          jb::core::ErrorCategory::Unavailable,
                          "net.http.connect_failed",
                          "The HTTP connection could not be established");
    }
    if (is_one_of(result,
                  {CURLE_PEER_FAILED_VERIFICATION,
                   CURLE_SSL_ISSUER_ERROR,
                   CURLE_SSL_PINNEDPUBKEYNOTMATCH,
                   CURLE_SSL_INVALIDCERTSTATUS})) {
        return http_error(HttpErrorKind::TlsVerification,
                          jb::core::ErrorCategory::PermissionDenied,
                          "net.http.tls_verification_failed",
                          "HTTP TLS peer verification failed");
    }
    if (is_one_of(result,
                  {CURLE_SSL_CONNECT_ERROR,
                   CURLE_SSL_CERTPROBLEM,
                   CURLE_SSL_CIPHER,
                   CURLE_USE_SSL_FAILED,
                   CURLE_SSL_ENGINE_NOTFOUND,
                   CURLE_SSL_ENGINE_SETFAILED,
                   CURLE_SSL_ENGINE_INITFAILED,
                   CURLE_SSL_CACERT_BADFILE,
                   CURLE_SSL_CRL_BADFILE,
                   CURLE_SSL_CLIENTCERT})) {
        return http_error(HttpErrorKind::TlsHandshake,
                          jb::core::ErrorCategory::Io,
                          "net.http.tls_handshake_failed",
                          "The HTTP TLS handshake failed");
    }
    if (result == CURLE_OPERATION_TIMEDOUT) {
        return http_error(HttpErrorKind::Timeout,
                          jb::core::ErrorCategory::Timeout,
                          "net.http.timeout",
                          "The HTTP request deadline expired");
    }
    if (is_one_of(result, {CURLE_SEND_ERROR, CURLE_UPLOAD_FAILED, CURLE_READ_ERROR, CURLE_SEND_FAIL_REWIND})) {
        return http_error(HttpErrorKind::Send,
                          jb::core::ErrorCategory::Io,
                          "net.http.send_failed",
                          "The HTTP request could not be sent");
    }
    if (is_one_of(result,
                  {CURLE_RECV_ERROR,
                   CURLE_PARTIAL_FILE,
                   CURLE_GOT_NOTHING,
                   CURLE_BAD_CONTENT_ENCODING,
                   CURLE_SSL_SHUTDOWN_FAILED})) {
        return http_error(HttpErrorKind::Receive,
                          jb::core::ErrorCategory::Io,
                          "net.http.receive_failed",
                          "The HTTP response could not be received");
    }
    if (result == CURLE_TOO_MANY_REDIRECTS) {
        return http_error(HttpErrorKind::Redirect,
                          jb::core::ErrorCategory::Io,
                          "net.http.redirect_failed",
                          "The HTTP redirect could not be followed safely");
    }
    if (is_one_of(result,
                  {CURLE_UNSUPPORTED_PROTOCOL,
                   CURLE_WEIRD_SERVER_REPLY,
                   CURLE_HTTP2,
                   CURLE_HTTP2_STREAM,
                   CURLE_HTTP3,
                   CURLE_CHUNK_FAILED})) {
        return http_error(HttpErrorKind::Protocol,
                          jb::core::ErrorCategory::Io,
                          "net.http.protocol_error",
                          "The HTTP protocol exchange failed");
    }

    // Configuration, callback-contract, allocation, and future curl results are not safe transport classifications.
    return http_error(HttpErrorKind::Internal,
                      jb::core::ErrorCategory::Internal,
                      "net.http.internal",
                      "The HTTP request failed internally");
}

} // namespace jb::net::http::detail
