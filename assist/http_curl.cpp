// HttpClient on libcurl (HTTPS with the system's certificates; honours the
// usual proxy environment variables). Without libcurl the build gets a
// client that explains what is missing.

#include <format>
#include <mutex>

#include "assist/provider.hpp"

#if HERMES_HAVE_CURL
#include <curl/curl.h>
#endif

namespace hermes::assist {

#if HERMES_HAVE_CURL
namespace {

struct Xfer {
  std::string body;
  const std::atomic<bool>* cancel = nullptr;
};

std::size_t on_data(char* p, std::size_t sz, std::size_t n, void* user) {
  auto* x = static_cast<Xfer*>(user);
  x->body.append(p, sz * n);
  if (x->body.size() > (64u << 20)) return 0;  // answers are small; stop runaway bodies
  return sz * n;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* x = static_cast<Xfer*>(user);
  return x->cancel && x->cancel->load() ? 1 : 0;
}

class CurlClient final : public HttpClient {
 public:
  CurlClient() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  }
  HttpResponse post(const std::string& url, const std::vector<std::string>& headers, const std::string& body,
                    double timeout_s, const std::atomic<bool>* cancel) override {
    HttpResponse r;
    CURL* c = curl_easy_init();
    if (!c) {
      r.error = "curl_easy_init failed";
      return r;
    }
    curl_slist* hl = nullptr;
    for (const auto& h : headers) hl = curl_slist_append(hl, h.c_str());
    Xfer x;
    x.cancel = cancel;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_s * 1000));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "hermes-assistant/0.1");
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
      long code = 0;
      curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
      r.status = static_cast<int>(code);
      r.body = std::move(x.body);
    } else {
      r.error = rc == CURLE_ABORTED_BY_CALLBACK ? "cancelled" : std::format("{}: {}", url, curl_easy_strerror(rc));
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return r;
  }
};

}  // namespace

std::shared_ptr<HttpClient> default_http_client() {
  static auto c = std::make_shared<CurlClient>();
  return c;
}
bool have_http_client() { return true; }

#else

namespace {
class NoHttp final : public HttpClient {
 public:
  HttpResponse post(const std::string&, const std::vector<std::string>&, const std::string&, double,
                    const std::atomic<bool>*) override {
    HttpResponse r;
    r.error = "this hermes build has no HTTP client (rebuild with libcurl: macOS has it; Linux: apt install libcurl4-openssl-dev)";
    return r;
  }
};
}  // namespace

std::shared_ptr<HttpClient> default_http_client() {
  static auto c = std::make_shared<NoHttp>();
  return c;
}
bool have_http_client() { return false; }

#endif

}  // namespace hermes::assist
