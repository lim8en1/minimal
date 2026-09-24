#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../plugins/server/checklist/checklist_hub.hpp"
#include "../plugins/server/checklist/endpoint.hpp"
#include "checklist_store.hpp"
#include "checklist_tool.hpp"
#include "fakes.hpp"
#include "minimal/plugins/plugin_rpc.hpp"
#include "minimal/plugins/plugin_storage.hpp"
#include "minimal/tools/tool.hpp"
#include "../plugins/server/checklist/rpc_service.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

// NOTE: every CHECK/CHECK_FALSE below that wraps an expression containing a
// braced initializer gets an extra paren layer -- the preprocessor splits macro
// arguments on commas INSIDE braces, and Catch2 macros are macros.

namespace {

struct Fixture {
    TempDir dir;
    plugins::PluginStorage storage{dir.path() / "plugins.db"};
    std::shared_ptr<checklist::ChecklistStore> store =
        std::make_shared<checklist::ChecklistStore>(&storage);
    std::unique_ptr<tools::Tool> tool = checklist::MakeChecklistTool(store);

    tools::ToolContext Context(const std::string& session_id = "s1") {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = session_id;
        return ctx;
    }

    static nlohmann::json SetInput(std::vector<nlohmann::json> items) {
        return nlohmann::json{{"op", "set"}, {"items", std::move(items)}};
    }
    static nlohmann::json Update(std::int64_t id, const char* status) {
        return nlohmann::json{{"op", "update"}, {"id", id}, {"status", status}};
    }
    static nlohmann::json Get(const std::string& session_id) {
        return nlohmann::json{{"session_id", session_id}};
    }
};

plugins::PluginRpcHandler::Reply Call(checklist::ChecklistRpcService& service,
                                      const std::string& method, const nlohmann::json& payload) {
    return service.Handle(method, payload.dump());
}

}  // namespace

// --- the tool -----------------------------------------------------------------

TEST_CASE("set assigns ids, stores, and renders the tasklist", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();

    const auto result =
        f.tool->execute(Fixture::SetInput({{{"text", "read the code"}, {"status", "completed"}},
                                            {{"text", "write the plan"}, {"status", "in_progress"}},
                                            {{"text", "get it reviewed"}}}),
                        ctx);

    CHECK_FALSE(result.is_error);
    // Numbered by id: the model reads update's ids straight off the list.
    CHECK(result.content_text.find("1. [x] read the code") != std::string::npos);
    CHECK(result.content_text.find("2. [ ] *(in progress)* write the plan") != std::string::npos);
    CHECK(result.content_text.find("3. [ ] get it reviewed") != std::string::npos);
    CHECK(result.content_text.find("1/3 done") != std::string::npos);

    // Ids are 1-based and monotonic, so update can address them.
    const auto stored = f.store->Load("s1");
    REQUIRE(stored.size() == 3);
    CHECK(stored[0].id == 1);
    CHECK(stored[2].id == 3);
    CHECK(stored[2].status == checklist::ItemStatus::Pending);
}

TEST_CASE("update mutates without resending the list", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "one"}}, {{"text", "two"}}}), ctx);

    const auto result = f.tool->execute(Fixture::Update(2, "completed"), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("2. [x] two") != std::string::npos);

    const auto stored = f.store->Load("s1");
    CHECK(stored[1].status == checklist::ItemStatus::Completed);
    CHECK(stored[0].status == checklist::ItemStatus::Pending);
}

TEST_CASE("view reports the stored list, empty included", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();

    const auto empty = f.tool->execute(nlohmann::json{{"op", "view"}}, ctx);
    CHECK_FALSE(empty.is_error);
    CHECK(empty.content_text == "Checklist is empty.");

    (void)f.tool->execute(Fixture::SetInput({{{"text", "only"}}}), ctx);
    const auto viewed = f.tool->execute(nlohmann::json{{"op", "view"}}, ctx);
    CHECK_FALSE(viewed.is_error);
    CHECK(viewed.content_text.find("1. [ ] only") != std::string::npos);
}

TEST_CASE("set replaces the whole list, including back to empty", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "old"}}}), ctx);

    (void)f.tool->execute(Fixture::SetInput({{{"text", "new one"}}, {{"text", "new two"}}}), ctx);
    CHECK(f.store->Load("s1").size() == 2);

    (void)f.tool->execute(Fixture::SetInput({}), ctx);
    CHECK(f.store->Load("s1").empty());
}

TEST_CASE("a cleared list is removed from enumeration, not stored as an empty blob",
          "[checklist][tool][invariant]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "one"}}}), ctx);
    REQUIRE_FALSE(f.store->SessionIds().empty());

    (void)f.tool->execute(Fixture::SetInput({}), ctx);
    // The empty row used to linger under its key, so the `list` RPC kept
    // reporting a session whose checklist was gone.
    CHECK(f.store->SessionIds().empty());
}

TEST_CASE("item count and text length are capped", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();

    std::vector<nlohmann::json> too_many;
    for (int i = 0; i < 201; ++i) too_many.push_back({{"text", "item"}});
    const auto many = f.tool->execute(Fixture::SetInput(std::move(too_many)), ctx);
    CHECK(many.is_error);

    const auto huge = f.tool->execute(Fixture::SetInput({{{"text", std::string(5000, 'x')}}}), ctx);
    CHECK(huge.is_error);

    // Over-cap text on an update: real item, real text-length refusal.
    (void)f.tool->execute(Fixture::SetInput({{{"text", "real"}}}), ctx);
    const auto updated = f.tool->execute(
        nlohmann::json{{"op", "update"}, {"id", 1}, {"text", std::string(5000, 'x')}}, ctx);
    CHECK(updated.is_error);
    CHECK(f.store->Load("s1")[0].text == "real");
}

TEST_CASE("checklists are per session", "[checklist][tool]") {
    Fixture f;
    auto ctx1 = f.Context("s1");
    auto ctx2 = f.Context("s2");
    (void)f.tool->execute(Fixture::SetInput({{{"text", "session one's task"}}}), ctx1);
    (void)f.tool->execute(Fixture::SetInput({{{"text", "session two's task"}}}), ctx2);

    CHECK(f.store->Load("s1").size() == 1);
    CHECK(f.store->Load("s1")[0].text == "session one's task");
    CHECK(f.store->Load("s2")[0].text == "session two's task");
    CHECK(f.store->Load("s3").empty());
}

// --- input validation: every failure is an error RESULT, never an exception ----

TEST_CASE("bad ops and missing fields are error results", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();

    CHECK((f.tool->execute(nlohmann::json::object(), ctx).is_error));                // no op
    CHECK((f.tool->execute(nlohmann::json{{"op", 1}}, ctx).is_error));               // non-string op
    CHECK((f.tool->execute(nlohmann::json{{"op", "destroy"}}, ctx).is_error));       // unknown op
    CHECK((f.tool->execute(nlohmann::json{{"op", "set"}}, ctx).is_error));           // no items
    CHECK((f.tool->execute(nlohmann::json{{"op", "set"}, {"items", 5}}, ctx).is_error));
    CHECK((f.tool->execute(Fixture::SetInput({{{"status", "done"}}}), ctx).is_error));  // no text
    CHECK((f.tool->execute(Fixture::SetInput({{{"text", ""}}}), ctx).is_error));        // empty text
    CHECK((f.tool->execute(Fixture::SetInput({{{"text", "two\nlines"}}}), ctx).is_error));
    CHECK((f.tool->execute(Fixture::SetInput({{{"text", "x"}, {"status", 3}}}), ctx).is_error));
}

TEST_CASE("update validates id, fields, and existence", "[checklist][tool]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "one"}}}), ctx);

    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}}, ctx).is_error));  // no id
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 1}}, ctx).is_error));  // no change
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 1}, {"status", "bad"}}, ctx)
               .is_error));
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 1}, {"text", ""}}, ctx)
               .is_error));
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 1}, {"text", "a\rb"}}, ctx)
               .is_error));
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 99}, {"status", "completed"}},
                           ctx)
               .is_error));  // unknown id
    CHECK((f.tool->execute(nlohmann::json{{"op", "update"}, {"id", "1"}, {"status", "completed"}},
                           ctx)
               .is_error));  // non-integer id

    // The unknown-id error names the affordance: view is how the model recovers.
    const auto miss =
        f.tool->execute(nlohmann::json{{"op", "update"}, {"id", 99}, {"status", "completed"}},
                        ctx);
    CHECK(miss.is_error);
    CHECK(miss.content_text.find("op=view") != std::string::npos);
}

// --- persistence and degradation -----------------------------------------------

TEST_CASE("state survives a server restart (close and reopen the store)", "[checklist]") {
    // The store is single-writer by construction now (ExclusiveFileLock), so
    // the "restart" is simulated the way a real restart happens: the holder
    // closes first, then the reopened store reads what was persisted.
    TempDir dir;
    {
        plugins::PluginStorage storage(dir.path() / "plugins.db");
        auto store = std::make_shared<checklist::ChecklistStore>(&storage);
        auto tool = checklist::MakeChecklistTool(store);
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        (void)tool->execute(Fixture::SetInput({{{"text", "survives"}}, {{"text", "restart"}}}),
                            ctx);
    }

    plugins::PluginStorage reopened(dir.path() / "plugins.db");
    checklist::ChecklistStore fresh_store(&reopened);
    const auto items = fresh_store.Load("s1");
    REQUIRE(items.size() == 2);
    CHECK(items[0].text == "survives");
}

TEST_CASE("a host without the storage seam degrades with a clear error", "[checklist]") {
    auto null_store = std::make_shared<checklist::ChecklistStore>(nullptr);
    auto tool = checklist::MakeChecklistTool(null_store);
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";

    CHECK_FALSE(null_store->available());
    // Built explicitly: a single-element braced list like {{"text", "x"}} is
    // ambiguous to nlohmann and collapses to an array of an array.
    const auto item = nlohmann::json::array({nlohmann::json{{"text", "x"}}});
    const auto set = tool->execute(nlohmann::json{{"op", "set"}, {"items", item}}, ctx);
    CHECK(set.is_error);
    CHECK(set.content_text.find("storage is not available") != std::string::npos);

    const auto view = tool->execute(nlohmann::json{{"op", "view"}}, ctx);
    CHECK(view.is_error);
    CHECK(view.content_text.find("storage is not available") != std::string::npos);
}

TEST_CASE("a corrupted stored row is a loud error, never a silent empty list", "[checklist]") {
    Fixture f;
    f.storage.Set(checklist::kStorageNamespace, "s1", "{not json");

    auto ctx = f.Context();
    CHECK_THROWS_AS(f.store->Load("s1"), std::runtime_error);
    const auto viewed = f.tool->execute(nlohmann::json{{"op", "view"}}, ctx);
    CHECK(viewed.is_error);
    CHECK(viewed.content_text.find("not valid JSON") != std::string::npos);
    // The error names the way out, and the way out works: set never reads
    // the old row.
    CHECK(viewed.content_text.find("op=set") != std::string::npos);
    const auto reset = f.tool->execute(Fixture::SetInput({{{"text", "fresh"}}, {{"text", "start"}}}), ctx);
    CHECK_FALSE(reset.is_error);
    CHECK(f.store->Load("s1").size() == 2);
}

// --- the .rpc service -----------------------------------------------------------

TEST_CASE("get returns the structured list a UI fetches", "[checklist][rpc]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "visible"}, {"status", "completed"}}}), ctx);

    checklist::ChecklistRpcService service(*f.store);
    const auto reply = Call(service, "get", Fixture::Get("s1"));
    CHECK(reply.ok);
    const auto body = nlohmann::json::parse(reply.payload);
    CHECK(body.at("session_id") == "s1");
    const auto& items = body.at("items");
    REQUIRE(items.size() == 1);
    CHECK(items[0].at("id") == 1);
    CHECK(items[0].at("text") == "visible");
    CHECK(items[0].at("status") == "completed");
}

TEST_CASE("list enumerates every stored session", "[checklist][rpc]") {
    Fixture f;
    auto ctx1 = f.Context("s1");
    auto ctx2 = f.Context("s2");
    (void)f.tool->execute(Fixture::SetInput({{{"text", "a"}}}), ctx1);
    (void)f.tool->execute(Fixture::SetInput({{{"text", "b"}}}), ctx2);

    checklist::ChecklistRpcService service(*f.store);
    const auto reply = Call(service, "list", nlohmann::json::object());
    CHECK(reply.ok);
    const auto body = nlohmann::json::parse(reply.payload);
    REQUIRE(body.at("checklists").size() == 2);
}

TEST_CASE("the service validates method and payload", "[checklist][rpc]") {
    Fixture f;
    checklist::ChecklistRpcService service(*f.store);

    CHECK_FALSE((Call(service, "subscribe", nlohmann::json::object()).ok));  // streams go through the endpoint
    CHECK_FALSE((Call(service, "get", nlohmann::json::object()).ok));        // no session_id
    CHECK_FALSE((Call(service, "get", nlohmann::json{{"session_id", 5}}).ok));
    const auto bad_json = service.Handle("get", "{oops");
    CHECK_FALSE(bad_json.ok);
    CHECK(bad_json.error.find("not valid JSON") != std::string::npos);
}

TEST_CASE("a null-storage service degrades like the tool", "[checklist][rpc]") {
    auto null_store = std::make_shared<checklist::ChecklistStore>(nullptr);
    checklist::ChecklistRpcService service(*null_store);
    const auto reply = Call(service, "get", Fixture::Get("s1"));
    CHECK_FALSE(reply.ok);
    CHECK(reply.error.find("storage is not available") != std::string::npos);
}

TEST_CASE("the dispatch convention routes PluginCall to the service", "[checklist][rpc]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "routed"}}}), ctx);

    checklist::ChecklistRpcService service(*f.store);
    plugins::ServiceRegistry services;
    services.Provide(plugins::PluginRpcServiceName(checklist::kPluginName), &service);
    services.Seal();

    const auto result = plugins::DispatchPluginCall(&services, checklist::kPluginName, "get",
                                                    Fixture::Get("s1").dump());
    CHECK(result.outcome == plugins::RpcOutcome::kOk);
    const auto body = nlohmann::json::parse(result.payload);
    CHECK(body.at("items")[0].at("text") == "routed");

    // And an unknown plugin name stays NOT_FOUND through the same path.
    const auto ghost = plugins::DispatchPluginCall(&services, "ghost", "get", "{}");
    CHECK(ghost.outcome == plugins::RpcOutcome::kNotFound);
}

// --- ordering, list filter, push ------------------------------------------------

TEST_CASE("the tool is sequential so a round's set and update apply in order",
          "[checklist][tool]") {
    Fixture f;
    CHECK_FALSE(f.tool->parallel_safe());
}

TEST_CASE("update validates its input before touching the stored row", "[checklist][tool]") {
    Fixture f;
    f.storage.Set(checklist::kStorageNamespace, "s1", "{not json");
    auto ctx = f.Context();
    // A bad text is reported as such, not masked by the unreadable row.
    const auto result = f.tool->execute(
        nlohmann::json{{"op", "update"}, {"id", 1}, {"text", std::string(5000, 'x')}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("capped") != std::string::npos);
}

TEST_CASE("list with session_ids answers only the named sessions that have a list",
          "[checklist][rpc]") {
    Fixture f;
    auto ctx1 = f.Context("s1");
    auto ctx2 = f.Context("s2");
    (void)f.tool->execute(Fixture::SetInput({{{"text", "a"}}}), ctx1);
    (void)f.tool->execute(Fixture::SetInput({{{"text", "b"}}}), ctx2);

    checklist::ChecklistRpcService service(*f.store);
    const auto reply =
        Call(service, "list", nlohmann::json{{"session_ids", {"s2", "ghost"}}});
    REQUIRE(reply.ok);
    const auto body = nlohmann::json::parse(reply.payload);
    REQUIRE(body.at("checklists").size() == 1);
    CHECK(body.at("checklists")[0].at("session_id") == "s2");

    CHECK_FALSE((Call(service, "list", nlohmann::json{{"session_ids", "s1"}}).ok));
    CHECK_FALSE((Call(service, "list", nlohmann::json{{"session_ids", {1}}}).ok));
}

namespace {

class RecordingSink final : public plugins::PluginSubscriptionSink {
public:
    bool Push(std::string json_payload) override {
        std::lock_guard<std::mutex> lock(mtx_);
        ++pushes_;
        if (!alive_) return false;
        payloads_.push_back(nlohmann::json::parse(json_payload));
        return true;
    }
    void Kill() {
        std::lock_guard<std::mutex> lock(mtx_);
        alive_ = false;
    }
    int PushCount() {
        std::lock_guard<std::mutex> lock(mtx_);
        return pushes_;
    }
    // The latest "checklist" push for `session_id`, if any.
    std::optional<nlohmann::json> Latest(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = payloads_.rbegin(); it != payloads_.rend(); ++it) {
            if (it->value("type", "") == "checklist" && it->value("session_id", "") == session_id) {
                return *it;
            }
        }
        return std::nullopt;
    }
    bool SawHeartbeat() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& payload : payloads_) {
            if (payload.value("type", "") == "heartbeat") return true;
        }
        return false;
    }

private:
    std::mutex mtx_;
    bool alive_ = true;
    int pushes_ = 0;
    std::vector<nlohmann::json> payloads_;
};

template <typename Pred>
bool WaitUntil(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

}  // namespace

TEST_CASE("every successful write pushes the session's list; a clear pushes []",
          "[checklist][push]") {
    Fixture f;
    checklist::ChecklistHub hub(*f.store, std::chrono::milliseconds(10),
                                std::chrono::milliseconds(1));
    auto tool = checklist::MakeChecklistTool(f.store, &hub);
    auto sink = std::make_shared<RecordingSink>();
    hub.AddSink(sink);
    auto ctx = f.Context();

    (void)tool->execute(Fixture::SetInput({{{"text", "one"}}, {{"text", "two"}}}), ctx);
    REQUIRE(WaitUntil([&] { return sink->Latest("s1").has_value(); }));
    CHECK(sink->Latest("s1")->at("items").size() == 2);

    (void)tool->execute(Fixture::Update(2, "completed"), ctx);
    REQUIRE(WaitUntil([&] {
        const auto latest = sink->Latest("s1");
        return latest && latest->at("items")[1].at("status") == "completed";
    }));

    (void)tool->execute(Fixture::SetInput({}), ctx);
    REQUIRE(WaitUntil([&] {
        const auto latest = sink->Latest("s1");
        return latest && latest->at("items").empty();
    }));

    // Idle: heartbeats keep the stream stoppable.
    CHECK(WaitUntil([&] { return sink->SawHeartbeat(); }));

    // A subscriber that went away is dropped, not pushed to forever.
    sink->Kill();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int after_drop = sink->PushCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sink->PushCount() == after_drop);
    hub.Shutdown();
}

TEST_CASE("the endpoint answers calls like .rpc and serves only the changes stream",
          "[checklist][rpc]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "routed"}}}), ctx);
    checklist::ChecklistRpcService service(*f.store);
    checklist::ChecklistHub hub(*f.store);
    checklist::ChecklistEndpoint endpoint(service, hub);

    const std::string payload = R"({"session_id": "s1"})";
    CHECK(nlohmann::json::parse(endpoint.Call("get", payload)) ==
          nlohmann::json::parse(service.Handle("get", payload).payload));
    CHECK_THROWS(endpoint.Call("get", "{}"));

    CHECK(endpoint.Subscribe("changes", "{}", std::make_shared<RecordingSink>()));
    CHECK_FALSE(endpoint.Subscribe("totals", "{}", std::make_shared<RecordingSink>()));
    hub.Shutdown();
}
