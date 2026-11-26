#include "assist/ai_host.hpp"

#include <format>

#include "gui/scene/session.hpp"

namespace hermes::assist {

AiHost::AiHost(AssistantConfig cfg, std::shared_ptr<HttpClient> http) : cfg_(std::move(cfg)), http_(std::move(http)) {}

AiHost::~AiHost() { assistant_.reset(); }

void AiHost::set_on_update(std::function<void()> fn) {
  on_update_ = std::move(fn);
  if (assistant_) assistant_->set_on_update(on_update_);
}

bool AiHost::ensure(std::string* why) {
  if (assistant_) return true;
  std::unique_ptr<Provider> p;
  if (provider_override_) {
    p = provider_override_();
  } else {
    if (!assistant_ready(cfg_, why)) return false;
    p = make_provider(cfg_.provider, http_, why);
    if (!p) return false;
  }
  assistant_ = std::make_unique<Assistant>(cfg_, std::move(p), tools_);
  if (on_update_) assistant_->set_on_update(on_update_);
  return true;
}

bool AiHost::ask(gui::Session& s, const gui::ViewApi& v, const std::string& q, std::string* why) {
  if (!ensure(why)) return false;
  return assistant_->ask(q, build_context(cfg_, s, v), why);
}

void AiHost::pump(gui::Session& s, gui::ViewApi& v) {
  if (!assistant_ || !commands_) return;
  ToolContext ctx{s, v, *commands_, cfg_.runs_db};
  assistant_->pump(ctx);
}

std::string AiHost::status() const {
  std::string s = status_line(cfg_);
  if (!cfg_.source.empty()) s += "\nconfig: " + cfg_.source;
  if (assistant_) {
    s += std::format("\nendpoint: {}\n{}; tokens in {} out {}", assistant_->provider().endpoint(),
                     assistant_->busy() ? "answering" : "idle", assistant_->input_tokens(), assistant_->output_tokens());
  } else {
    std::string why;
    AssistantConfig c = cfg_;
    s += assistant_ready(c, &why) ? "\nready (not contacted yet)" : "\nnot ready: " + why;
  }
  for (const auto& w : cfg_.warnings) s += "\nconfig warning: " + w;
  return s;
}

void AiHost::register_commands(gui::CommandRegistry& reg) {
  commands_ = &reg;
  reg.add({"ai", "<question> | status|privacy|tools|yes|no|stop|reset|on|off|set <k> <v>",
           "Ask the AI assistant (optional; see 'ai privacy' for what is sent)", false,
           [this](gui::CommandContext& c, const std::vector<std::string>& a) -> gui::CommandResult {
             auto ok = [](std::string t) { return gui::CommandResult{true, std::move(t), {}}; };
             auto fail = [](std::string t) { return gui::CommandResult{false, std::move(t), {}}; };
             if (a.size() < 2) return fail("usage: ai <question>  (or ai status|privacy|tools|yes|no|stop|reset|on|off)");
             const std::string& sub = a[1];
             if (a.size() == 2) {
               if (sub == "status") return ok(status());
               if (sub == "privacy") {
                 std::string ep = assistant_ ? assistant_->provider().endpoint() : std::string();
                 if (ep.empty()) {
                   std::string why;
                   if (auto p = make_provider(cfg_.provider, http_, &why)) ep = p->endpoint();
                 }
                 return ok(sharing_text(cfg_, ep));
               }
               if (sub == "tools") {
                 std::string s;
                 for (const Tool* t : tools_.list(true))
                   s += std::format("{:20} {:6} {}\n", t->name, t->access_for ? "mixed" : access_name(t->access),
                                    t->title);
                 return ok(s + "(change tools: " +
                           (cfg_.edits == EditPolicy::ReadOnly ? "not offered, read-only)" : std::string(policy_name(cfg_.edits)) + ")"));
               }
               if (sub == "yes" || sub == "no") {
                 if (!assistant_ || !assistant_->pending_confirmation()) return fail("nothing to confirm");
                 assistant_->confirm(sub == "yes");
                 return ok(sub == "yes" ? "allowed" : "declined");
               }
               if (sub == "stop") {
                 if (assistant_) assistant_->cancel();
                 return ok("stopping");
               }
               if (sub == "reset") {
                 if (assistant_ && !assistant_->reset()) return fail("still answering; 'ai stop' first");
                 return ok("conversation cleared");
               }
               if (sub == "on" || sub == "off") {
                 if (assistant_ && assistant_->busy()) return fail("still answering; 'ai stop' first");
                 cfg_.enabled = sub == "on";
                 assistant_.reset();
                 std::string why;
                 if (cfg_.enabled && !assistant_ready(cfg_, &why)) return ok("enabled, but not ready: " + why);
                 return ok(cfg_.enabled ? "assistant enabled for this session\n" + status_line(cfg_) : "assistant off");
               }
             }
             if (sub == "set") {
               if (a.size() < 4) return fail("usage: ai set <key> <value>");
               if (assistant_ && assistant_->busy()) return fail("still answering; 'ai stop' first");
               std::string v;
               for (std::size_t i = 3; i < a.size(); ++i) v += (i > 3 ? " " : "") + a[i];
               std::string why;
               if (!set_config_value(cfg_, a[2], v, &why)) return fail(why);
               resolve_api_key(cfg_);
               assistant_.reset();  // next question uses the new settings (new conversation)
               return ok("set " + a[2] + " for this session; " + status_line(cfg_));
             }
             std::string q;
             for (std::size_t i = 1; i < a.size(); ++i) q += (i > 1 ? " " : "") + a[i];
             std::string why;
             if (!ask(c.session, c.view, q, &why)) return fail(why);
             return ok("asking " + assistant_->provider().name() + " " + assistant_->provider().model() + " ...");
           }});
}

}  // namespace hermes::assist
