#pragma once

// Minimal blocking HTTP/1.1 POST over a plain TCP socket (no TLS), used by
// the run tracker's writer thread to push batches to a hermes dashboard
// server. For remote access over the internet, put the server behind an SSH
// tunnel, a VPN (e.g. Tailscale) or a TLS reverse proxy.

#include <string>
#include <string_view>

namespace hermes::track {

/// POSTs `body` (JSON) to `base_url` + `path`. `base_url` is
/// http://host[:port][/prefix]. True on a 2xx answer; otherwise false with
/// a reason in *err.
bool http_post(std::string_view base_url, std::string_view path, std::string_view body, std::string_view token,
               double timeout_s, std::string* err);

}  // namespace hermes::track
