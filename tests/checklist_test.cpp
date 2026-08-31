#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "checklist_store.hpp"
#include "checklist_tool.hpp"
#include "fakes.hpp"
#include "minimal/plugins/plugin_rpc.hpp"
#include "minimal/plugins/plugin_storage.hpp"
#include "minimal/tools/tool.hpp"
#include "rpc_service.hpp"
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
    CHECK(result.content_text.find("- [x] read the code") != std::string::npos);
    CHECK(result.content_text.find("*(in progress)* write the plan") != std::string::npos);
    CHECK(result.content_text.find("- [ ] get it reviewed") != std::string::npos);
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
    CHECK(result.content_text.find("- [x] two") != std::string::npos);

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
    CHECK(viewed.content_text.find("- [ ] only") != std::string::npos);
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
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "survives"}}, {{"text", "restart"}}}), ctx);

    plugins::PluginStorage reopened(f.dir.path() / "plugins.db");
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
}

// --- the .rpc service -----------------------------------------------------------

TEST_CASE("get returns the structured list a UI fetches", "[checklist][rpc]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "visible"}, {"status", "completed"}}}), ctx);

    checklist::ChecklistRpcService service(f.store);
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

    checklist::ChecklistRpcService service(f.store);
    const auto reply = Call(service, "list", nlohmann::json::object());
    CHECK(reply.ok);
    const auto body = nlohmann::json::parse(reply.payload);
    REQUIRE(body.at("checklists").size() == 2);
}

TEST_CASE("the service validates method and payload", "[checklist][rpc]") {
    Fixture f;
    checklist::ChecklistRpcService service(f.store);

    CHECK_FALSE((Call(service, "subscribe", nlohmann::json::object()).ok));  // not yet a method
    CHECK_FALSE((Call(service, "get", nlohmann::json::object()).ok));        // no session_id
    CHECK_FALSE((Call(service, "get", nlohmann::json{{"session_id", 5}}).ok));
    const auto bad_json = service.Handle("get", "{oops");
    CHECK_FALSE(bad_json.ok);
    CHECK(bad_json.error.find("not valid JSON") != std::string::npos);
}

TEST_CASE("a null-storage service degrades like the tool", "[checklist][rpc]") {
    auto null_store = std::make_shared<checklist::ChecklistStore>(nullptr);
    checklist::ChecklistRpcService service(null_store);
    const auto reply = Call(service, "get", Fixture::Get("s1"));
    CHECK_FALSE(reply.ok);
    CHECK(reply.error.find("storage is not available") != std::string::npos);
}

TEST_CASE("the dispatch convention routes PluginCall to the service", "[checklist][rpc]") {
    Fixture f;
    auto ctx = f.Context();
    (void)f.tool->execute(Fixture::SetInput({{{"text", "routed"}}}), ctx);

    checklist::ChecklistRpcService service(f.store);
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
