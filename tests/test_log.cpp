// Tests for hermes::log. Same minimal runner style as test_core.cpp.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"

namespace lg = hermes::log;
using lg::Level;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

HERMES_LOG_CHANNEL(log_route, "t.route");
HERMES_LOG_CHANNEL(log_tree, "t.route.tree");
HERMES_LOG_CHANNEL(log_tree2, "t.route.tree");  // second handle, same channel
HERMES_LOG_CHANNEL(log_place, "t.place.gp");

/// Installs a memory sink for one test and restores defaults afterwards.
struct Capture {
  std::shared_ptr<lg::MemorySink> sink = std::make_shared<lg::MemorySink>();
  lg::SinkId id;
  Capture() : id(lg::add_sink(sink)) {}
  ~Capture() {
    lg::remove_sink(id);
    for (const auto& c : lg::channels()) {
      if (c.name != "*") lg::clear_level(c.name);
    }
    lg::set_level("*", Level::Info);
    lg::set_default_message_cap(lg::kUnlimited);
    lg::reset_message_counts();
  }
  std::size_t count() const { return sink->size(); }
  std::vector<lg::StoredRecord> recs() const { return sink->records(); }
};

void test_levels_and_names() {
  CHECK(lg::parse_level("DEBUG") == Level::Debug);
  CHECK(lg::parse_level("warning") == Level::Warn);
  CHECK(lg::parse_level("4") == Level::Verbose);
  CHECK(!lg::parse_level("loud"));
  CHECK(lg::to_string(Level::Trace) == "trace");
  CHECK(log_tree.name() == "t.route.tree");
  CHECK(&lg::channel("t.route") == &lg::channel("t.route"));
}

void test_hierarchy() {
  Capture cap;
  CHECK(log_tree.level() == Level::Info);  // root default
  lg::set_level("t.route", Level::Debug);
  CHECK(log_route.enabled(Level::Debug));
  CHECK(log_tree.enabled(Level::Debug));   // child inherits
  CHECK(log_tree2.enabled(Level::Debug));  // every handle updated
  CHECK(!log_place.enabled(Level::Debug));

  lg::set_level("t.route.tree", Level::Off);  // child override
  CHECK(!log_tree.enabled(Level::Error));
  CHECK(log_route.enabled(Level::Debug));

  lg::set_level("t.route", Level::Warn);  // parent change keeps child override
  CHECK(!log_tree.enabled(Level::Error));
  CHECK(lg::explicit_level("t.route.tree") == Level::Off);

  lg::clear_level("t.route.tree");  // back to inheriting
  CHECK(log_tree.level() == Level::Warn);
  CHECK(!lg::explicit_level("t.route.tree"));

  lg::set_level("*", Level::Error);  // root affects only non-explicit subtrees
  CHECK(log_place.level() == Level::Error);
  CHECK(log_tree.level() == Level::Warn);

  // A channel created after its parent was configured inherits at creation.
  lg::set_level("t.late", Level::Trace);
  lg::Channel late("t.late.child.grandchild");
  CHECK(late.level() == Level::Trace);
  CHECK(lg::level("t.late.child") == Level::Trace);
}

void test_disabled_does_not_evaluate() {
  Capture cap;
  int calls = 0;
  auto side_effect = [&] { return ++calls; };
  HLOG_DEBUG(log_place, "x {}", side_effect());  // Info: Debug is off
  CHECK(calls == 0);
  CHECK(cap.count() == 0);
  HLOG_INFO(log_place, "x {}", side_effect());
  CHECK(calls == 1);
  CHECK(cap.count() == 1);
  CHECK(cap.recs()[0].text == "x 1");
  CHECK(cap.recs()[0].channel == "t.place.gp");
  CHECK(cap.recs()[0].level == Level::Info);
  CHECK(!HLOG_ENABLED(log_place, Debug));
  CHECK(HLOG_ENABLED(log_place, Info));
}

void test_runtime_level_macro() {
  Capture cap;
  for (int i = 0; i < 4; ++i) {
    HLOG_AT(log_place, i % 2 ? Level::Info : Level::Debug, "i={}", i);
  }
  CHECK(cap.count() == 2);
}

void test_compile_time_removal() {
  Capture cap;
  lg::set_level("t.place", Level::Trace);
  int calls = 0;
  HLOG_TRACE(log_place, "{}", ++calls);
  if constexpr (lg::kCompiledMaxLevel >= Level::Trace) {
    CHECK(calls == 1 && cap.count() == 1);
  } else {
    CHECK(calls == 0 && cap.count() == 0);
  }
}

void test_message_ids_and_caps() {
  Capture cap;
  lg::set_message_cap("T-0001", 3);
  for (int i = 0; i < 10; ++i) HLOG_WARN_ID(log_route, "T-0001", "w{}", i);
  auto r = cap.recs();
  CHECK(r.size() == 3);
  CHECK(r[0].id == "T-0001");
  CHECK(!r[1].last_of_id);
  CHECK(r[2].last_of_id);
  CHECK(r[2].text == "w2");
  CHECK(lg::message("T-0001").count() == 3);

  lg::reset_message_counts();
  HLOG_WARN_ID(log_route, "T-0001", "again");
  CHECK(cap.count() == 4);

  lg::set_message_enabled("T-0001", false);
  HLOG_WARN_ID(log_route, "T-0001", "off");
  CHECK(cap.count() == 4);
  lg::set_message_enabled("T-0001", true);

  // Two call sites share one id's state.
  lg::set_message_cap("T-0002", 2);
  HLOG_INFO_ID(log_route, "T-0002", "a");
  HLOG_INFO_ID(log_tree, "T-0002", "b");
  HLOG_INFO_ID(log_route, "T-0002", "c");
  CHECK(cap.count() == 6);

  // Default cap applies to ids without an explicit cap.
  lg::set_default_message_cap(1);
  for (int i = 0; i < 5; ++i) HLOG_INFO_ID(log_route, "T-0003", "d");
  CHECK(cap.count() == 7);
  CHECK(lg::message("T-0002").cap() == 2);  // explicit cap kept

  // Cap 0 = never.
  lg::set_message_cap("T-0004", 0);
  HLOG_ERROR_ID(log_route, "T-0004", "never");
  CHECK(cap.count() == 7);
  lg::set_message_cap("T-0004", lg::kUnlimited);  // back to default cap (1)
  CHECK(lg::message("T-0004").cap() == 1);
  lg::set_message_cap("T-0001", lg::kUnlimited);
  lg::set_message_cap("T-0002", lg::kUnlimited);

  bool listed = false;
  for (const auto& m : lg::messages()) listed |= m.id == "T-0003";
  CHECK(listed);
}

void test_disabled_channel_skips_message_counting() {
  Capture cap;
  lg::set_message_cap("T-0010", 1);
  lg::set_level("t.route", Level::Error);
  HLOG_WARN_ID(log_route, "T-0010", "hidden");  // channel off: does not use up the cap
  lg::clear_level("t.route");
  HLOG_WARN_ID(log_route, "T-0010", "shown");
  CHECK(cap.count() == 1 && cap.recs()[0].text == "shown");
  lg::set_message_cap("T-0010", lg::kUnlimited);
}

void test_configure_spec() {
  Capture cap;
  lg::configure("warn, t.route=debug t.route.tree=off;msg:T-0020=off,msg:T-0021=2");
  CHECK(lg::level("*") == Level::Warn);
  CHECK(log_route.level() == Level::Debug);
  CHECK(log_tree.level() == Level::Off);
  CHECK(!lg::message("T-0020").enabled());
  CHECK(lg::message("T-0021").cap() == 2);
  lg::configure("t.route.tree=inherit,msg:T-0020=on,msg:T-0021=unlimited,msg:*=7");
  CHECK(log_tree.level() == Level::Debug);
  CHECK(lg::message("T-0020").enabled());
  CHECK(lg::message("T-0021").cap() == 7);
  lg::configure("msg:*=unlimited");
  CHECK(lg::message("T-0021").cap() == lg::kUnlimited);

  bool threw = false;
  try {
    lg::configure("t.route=loud");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    lg::configure("msg:X=maybe");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

void test_text_format() {
  lg::Record r{Level::Warn, "io.def", "DEF-0003", "bad pin", true, {}, 1.5, 2, "/a/b/x.cpp", 7};
  CHECK(lg::format_text(r, {}) ==
        "[WARN io.def DEF-0003] bad pin [DEF-0003 reached its limit; further ones are suppressed]\n");
  r.last_of_id = false;
  r.id = {};
  CHECK(lg::format_text(r, {true, true, true}) == "[   1.500 t2] [WARN io.def] bad pin (x.cpp:7)\n");
}

void test_file_and_json_sinks() {
  Capture cap;
  const auto dir = std::filesystem::temp_directory_path();
  const std::string txt = (dir / "hermes_log_test.txt").string();
  const std::string js = (dir / "hermes_log_test.jsonl").string();
  {
    auto t = std::make_shared<lg::TextSink>(txt);
    auto j = std::make_shared<lg::JsonLinesSink>(js);
    j->set_level(Level::Warn);
    const auto ti = lg::add_sink(t), ji = lg::add_sink(j);
    HLOG_INFO(log_route, "hello {}", 1);
    HLOG_WARN_ID(log_route, "T-0030", "quote \" tab\t nl\n end");
    lg::remove_sink(ti);
    lg::remove_sink(ji);
  }  // sinks destroyed -> files closed
  std::ifstream ft(txt), fj(js);
  std::string l1, l2, l3;
  std::getline(ft, l1);
  CHECK(std::regex_match(l1, std::regex(R"(\[ *[0-9.]+ t[0-9]+\] \[INFO t\.route\] hello 1)")));
  std::getline(fj, l3);
  CHECK(l3.find("\"level\":\"warn\"") != std::string::npos);
  CHECK(l3.find("\"id\":\"T-0030\"") != std::string::npos);
  CHECK(l3.find(R"("msg":"quote \" tab\t nl\n end")") != std::string::npos);
  CHECK(!std::getline(fj, l2));  // Info filtered by the sink's level
  std::filesystem::remove(txt);
  std::filesystem::remove(js);
}

void test_threads_enabled_no_interleave() {
  Capture cap;
  const std::string txt =
      (std::filesystem::temp_directory_path() / "hermes_log_mt.txt").string();
  constexpr int kThreads = 8, kPer = 5000;
  {
    auto t = std::make_shared<lg::TextSink>(txt);
    const auto ti = lg::add_sink(t);
    std::vector<std::thread> ts;
    for (int k = 0; k < kThreads; ++k) {
      ts.emplace_back([k] {
        for (int i = 0; i < kPer; ++i) {
          HLOG_INFO(log_place, "thread {} line {} payload {}", k, i, std::string(40, 'x'));
        }
      });
    }
    for (auto& th : ts) th.join();
    lg::remove_sink(ti);
  }
  CHECK(cap.count() == static_cast<std::size_t>(kThreads * kPer));
  std::ifstream f(txt);
  std::string line;
  std::size_t n = 0, good = 0;
  const std::regex re(R"(\[ *[0-9.]+ t[0-9]+\] \[INFO t\.place\.gp\] thread [0-9] line [0-9]+ payload x{40})");
  while (std::getline(f, line)) {
    ++n;
    good += std::regex_match(line, re);
  }
  CHECK(n == static_cast<std::size_t>(kThreads * kPer));
  CHECK(good == n);
  std::filesystem::remove(txt);
}

void test_threads_cap_exact() {
  Capture cap;
  lg::set_message_cap("T-0040", 100);
  std::vector<std::thread> ts;
  for (int k = 0; k < 8; ++k) {
    ts.emplace_back([] {
      for (int i = 0; i < 20000; ++i) HLOG_WARN_ID(log_route, "T-0040", "{}", i);
    });
  }
  for (auto& th : ts) th.join();
  CHECK(cap.count() == 100);
  int last = 0;
  for (const auto& r : cap.recs()) last += r.last_of_id;
  CHECK(last == 1);
  lg::set_message_cap("T-0040", lg::kUnlimited);
}

void test_level_changes_while_logging() {
  Capture cap;
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int k = 0; k < 4; ++k) {
    ts.emplace_back([&] {
      while (!stop.load()) HLOG_DEBUG(log_tree, "x");
    });
  }
  for (int i = 0; i < 2000; ++i) {
    lg::set_level("t.route", i % 2 ? Level::Debug : Level::Info);
  }
  stop = true;
  for (auto& th : ts) th.join();
  CHECK(true);  // no crash / deadlock; TSAN-clean by construction (atomics + mutexes)
}

void test_recursive_sink_is_dropped() {
  Capture cap;
  auto cb = std::make_shared<lg::CallbackSink>([](const lg::Record&) {
    HLOG_INFO(log_route, "from inside a sink");  // must not deadlock or recurse
  });
  const auto id = lg::add_sink(cb);
  HLOG_INFO(log_route, "outer");
  lg::remove_sink(id);
  CHECK(cap.count() == 1);
}

void test_write_preformatted() {
  Capture cap;
  lg::write(lg::channel("t.py"), Level::Info, "{not a format}");
  lg::write(lg::channel("t.py"), Level::Debug, "hidden");
  lg::set_message_cap("T-0050", 1);
  lg::write(lg::channel("t.py"), Level::Warn, "w", "T-0050");
  lg::write(lg::channel("t.py"), Level::Warn, "w", "T-0050");
  auto r = cap.recs();
  CHECK(r.size() == 2);
  CHECK(r[0].text == "{not a format}");
  CHECK(r[1].id == "T-0050" && r[1].last_of_id);
  lg::set_message_cap("T-0050", lg::kUnlimited);
}

void test_sink_removal_and_console_level() {
  Capture cap;
  auto extra = std::make_shared<lg::MemorySink>();
  const auto id = lg::add_sink(extra);
  CHECK(lg::remove_sink(id));
  CHECK(!lg::remove_sink(id));
  HLOG_INFO(log_route, "x");
  CHECK(extra->size() == 0);
  CHECK(lg::console_sink() != nullptr);
}

}  // namespace

int main() {
  lg::console_sink()->set_level(Level::Off);  // keep the test output clean
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"levels_and_names", test_levels_and_names},
      {"hierarchy", test_hierarchy},
      {"disabled_does_not_evaluate", test_disabled_does_not_evaluate},
      {"runtime_level_macro", test_runtime_level_macro},
      {"compile_time_removal", test_compile_time_removal},
      {"message_ids_and_caps", test_message_ids_and_caps},
      {"disabled_channel_skips_message_counting", test_disabled_channel_skips_message_counting},
      {"configure_spec", test_configure_spec},
      {"text_format", test_text_format},
      {"file_and_json_sinks", test_file_and_json_sinks},
      {"threads_enabled_no_interleave", test_threads_enabled_no_interleave},
      {"threads_cap_exact", test_threads_cap_exact},
      {"level_changes_while_logging", test_level_changes_while_logging},
      {"recursive_sink_is_dropped", test_recursive_sink_is_dropped},
      {"write_preformatted", test_write_preformatted},
      {"sink_removal_and_console_level", test_sink_removal_and_console_level},
  };
  for (const auto& [name, fn] : tests) {
    const int before = g_failures;
    fn();
    std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name.c_str());
  }
  std::printf("%zu tests, %d failures\n", tests.size(), g_failures);
  return g_failures == 0 ? 0 : 1;
}
