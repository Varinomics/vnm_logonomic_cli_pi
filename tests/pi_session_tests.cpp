#include "pi_sessions.h"

#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace pi = logonomic::pi;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

int assertions = 0;

void check(bool condition, const std::string& description)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error(description);
    }
}

struct Fixture_root
{
    fs::path path = fs::canonical(fs::temp_directory_path()) / ("logonomic-pi-sessions-" + addon::random_nonce_hex());
    Fixture_root() { fs::create_directories(path); }
    ~Fixture_root()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string read_text(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "exported file is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_records(const fs::path& path, const std::vector<Json>& records, std::string_view tail = {})
{
    std::ofstream output(path, std::ios::binary);
    for (const auto& record : records) {
        output << addon::serialize_json(record) << '\n';
    }
    output << tail;
    check(output.good(), "native fixture was written");
}

Json header(std::string_view id, const fs::path& cwd, int version = 3)
{
    return json::object({{"type", Json("session")}, {"version", Json(version)}, {"id", Json(id)},
        {"timestamp", Json("2026-11-16T10:00:00.000Z")}, {"cwd", Json(addon::path_to_utf8(cwd))}});
}

Json entry(std::string_view type, std::string_view id, std::string_view parent)
{
    return json::object({{"type", Json(type)}, {"id", Json(id)},
        {"parentId", parent.empty() ? Json() : Json(parent)}, {"timestamp", Json("2026-11-16T10:00:01.000Z")}});
}

Json message(std::string_view id, std::string_view parent, std::string_view role, Json content,
    std::string_view stop = {})
{
    auto value = entry("message", id, parent);
    if (role == "assistant" && content.is_string()) {
        content = Json(Json::Array{json::object({{"type", Json("text")}, {"text", std::move(content)}})});
    }
    auto native = json::object({{"role", Json(role)}, {"content", std::move(content)}, {"timestamp", Json(1)}});
    if (!stop.empty()) {
        native.set("stopReason", Json(stop));
    }
    value.set("message", std::move(native));
    return value;
}

Json record(const fs::path& path, std::string_view id = "native-a")
{
    return json::object({{"provider_session_id", Json(id)}, {"session_file", Json(addon::path_to_utf8(path))}});
}

addon::Session_launch_context context(const fs::path& root, const std::vector<std::string>& words)
{
    std::string script;
    for (const auto& word : words) {
        if (!script.empty()) {
            script += ' ';
        }
        script += '"';
        for (const char character : word) {
            script += character == '"' ? "\"\"\"" : std::string(1, character);
        }
        script += '"';
    }
    addon::Session_launch_context result;
    result.launcher = {script, "direct", ""};
    result.launcher_argv = words;
    result.working_directory = addon::path_to_utf8(root);
    result.launch_environment = addon::Launch_environment{addon::host_platform_id(), Json::object(), std::nullopt};
    return result;
}

addon::Host_outcome invoke(addon::Host_simulator& host, const addon::Capability_pair& capability,
    const std::string& operation, Json payload)
{
    const auto id = host.invoke(capability, operation, std::move(payload), 5000);
    const auto outcome = host.wait_outcome(id, 6000);
    check(outcome.has_value(), "SDK host received " + operation);
    return *outcome;
}

Json success(addon::Host_simulator& host, const addon::Capability_pair& capability,
    const std::string& operation, Json payload)
{
    auto result = invoke(host, capability, operation, std::move(payload));
    check(result.status == addon::Host_status::SUCCESS,
        operation + " succeeds: " + result.reason_code + " " + result.message);
    return std::move(result.payload);
}

void native_contract(addon::Host_simulator& host, const fs::path& root)
{
    // Native oracle: Pi SessionManager v2/v3, _buildIndex/getBranch and
    // append-only _persist. SDK oracle: interactive-resume@3/@4 and export@1.
    const auto source = root / "native.jsonl";
    auto compaction = entry("compaction", "compact-old", "old");
    compaction.set("summary", Json("Abandoned compaction summary"));
    compaction.set("firstKeptEntryId", Json("user"));
    compaction.set("tokensBefore", Json(100));
    const std::vector<Json> records = {
        header("native-a", root),
        message("user", "", "user", Json(Json::Array{
            json::object({{"type", Json("text")}, {"text", Json("User λ with `code`.")}}),
            json::object({{"type", Json("image")}, {"mimeType", Json("image/png")}, {"data", Json("AA==")}}),
        })),
        message("old", "user", "assistant", Json("Abandoned answer"), "stop"),
        compaction,
        message("tools", "user", "assistant", Json(Json::Array{
            json::object({{"type", Json("text")}, {"text", Json("Intermediate update")},
                {"textSignature", Json(R"({"v":1,"id":"intermediate","phase":"commentary"})")}}),
            json::object({{"type", Json("thinking")}, {"thinking", Json("Private reasoning")}}),
            json::object({{"type", Json("toolCall")}, {"id", Json("call-1")}, {"name", Json("read")},
                {"arguments", json::object({{"path", Json("a.cpp")}})}}),
        }), "toolUse"),
        message("tool-result", "tools", "toolResult", Json(Json::Array{
            json::object({{"type", Json("text")}, {"text", Json("Tool output\n`````\n::: user")}}),
        })),
        message("final", "tool-result", "assistant", Json(Json::Array{
            json::object({{"type", Json("text")}, {"text", Json("Selected final answer ✓")}}),
            json::object({{"type", Json("thinking")}, {"thinking", Json("")}, {"redacted", Json(true)},
                {"thinkingSignature", Json("opaque-secret")}}),
        }), "stop"),
    };
    write_records(source, records);
    addon::Contract_failure failure;
    const auto parsed = pi::read_native_session(source, "native-a", {}, &failure);
    check(parsed && parsed->entries.size() == 6 && parsed->branch == std::vector<std::size_t>({0, 3, 4, 5}),
        "all durable branches remain readable; restored branch is last persisted ancestry");
    check(!parsed->incomplete_tail_omitted && json::text(parsed->entries[2], "type") == "compaction",
        "the shared context reader can count retained compactions outside the selected branch");

    addon::Resume_inspect_request inspection;
    inspection.context = context(root, {"pi"});
    inspection.resume_record = record(source);
    const auto inspect = [&](int version) {
        auto payload = success(host, {"interactive-resume", static_cast<std::uint16_t>(version)}, "inspect_resume",
            addon::encode_resume_inspect_request(inspection));
        auto result = addon::decode_resume_inspect_result(payload, &failure, version);
        check(result && addon::validate_resume_inspection(inspection, *result, &failure), "inspection binds the exact complete request");
        if (version == 3) {
            check(!payload.find("content"), "version 3 remains wire-compatible without content evidence");
        }
        return *result;
    };
    check(inspect(3).availability == addon::Resume_availability::AVAILABLE, "exact native file is available");
    const auto known = inspect(4);
    check(known.content == addon::Resume_content::NONEMPTY, "retained selected-branch messages establish content");
    auto foreign = inspection;
    foreign.context.working_directory += "/other";
    check(!addon::validate_resume_inspection(foreign, known, &failure), "changed launch context invalidates cached evidence");
    auto previous_format = records;
    previous_format.front() = header("native-a", root, 2);
    write_records(source, previous_format);
    check(inspect(4).content == addon::Resume_content::NONEMPTY, "native version 2 tree format is read without migration writes");
    write_records(source, records);

    addon::Conversation_export_request request;
    request.provider_session_id = "native-a";
    request.resume_record = inspection.resume_record;
    request.working_directory = addon::path_to_utf8(root);
    request.launch_environment = *inspection.context.launch_environment;
    const auto export_file = [&](addon::Conversation_export_format format, bool compact, std::string_view name) {
        request.output_directory = addon::path_to_utf8(root / name);
        fs::create_directory(root / name);
        request.format = format;
        request.compact = compact;
        const auto payload = success(host, {"conversation-export", 1}, "export",
            addon::encode_conversation_export_request(request));
        const auto result = addon::decode_conversation_export_result(payload, &failure);
        check(result && result->compact == compact && result->attachment_count == 1, "writer preserves projection and attachment descriptor");
        const auto text = read_text(root / name / result->file_name);
        check(text.find("User λ") != std::string::npos && text.find("Selected final answer ✓") != std::string::npos &&
            text.find("Abandoned") == std::string::npos && text.find("opaque-secret") == std::string::npos,
            "export preserves selected Unicode conversation and excludes abandoned branches and opaque payloads");
        check(text.find("[Redacted thinking content;") != std::string::npos, "encrypted-content marker remains in both projections");
        check((text.find("Private reasoning") != std::string::npos) == !compact &&
            (text.find("Tool output") != std::string::npos) == !compact &&
            (text.find("Intermediate update") != std::string::npos) == !compact,
            "compact projection removes known reasoning, tools and intermediate updates");
        return *result;
    };
    export_file(addon::Conversation_export_format::TEXT, false, "text");
    export_file(addon::Conversation_export_format::MARKDOWN, false, "markdown");
    export_file(addon::Conversation_export_format::CONVERSATION, true, "compact");
    check(read_text(root / "compact/conversation.vnm_chat").starts_with("#!vnm-chat 1\n"), "highlighted format uses SDK grammar");

    write_records(source, records, "{\"type\":\"message\"");
    check(inspect(4).availability == addon::Resume_availability::UNKNOWN &&
        inspect(4).content == addon::Resume_content::UNKNOWN, "partial native evidence cannot establish resumability or emptiness");
    check(export_file(addon::Conversation_export_format::TEXT, false, "tail").incomplete_tail_omitted,
        "export can publish the frozen complete prefix while declaring an omitted tail");

    write_records(source, {header("native-a", root)});
    const auto empty = inspect(4);
    check(empty.availability == addon::Resume_availability::AVAILABLE && empty.content == addon::Resume_content::EMPTY,
        "a complete native metadata-only file is resumable but empty");
    request.output_directory = addon::path_to_utf8(root / "empty");
    fs::create_directory(root / "empty");
    const auto no_content = invoke(host, {"conversation-export", 1}, "export", addon::encode_conversation_export_request(request));
    check(no_content.status == addon::Host_status::UNAVAILABLE && no_content.reason_code == "conversation_empty" &&
        fs::is_empty(root / "empty"), "empty history never publishes an empty success file");

    auto edit = entry("context_edit", "edit", "empty-user");
    edit.set("targetId", Json("empty-user"));
    edit.set("replacement", json::object({{"content", Json("Retained model-context replacement")}}));
    write_records(source, {header("native-a", root), message("empty-user", "", "user", Json("")), edit});
    const auto edited = inspect(4);
    check(edited.availability == addon::Resume_availability::AVAILABLE && edited.content == addon::Resume_content::UNKNOWN,
        "context-edit replacement evidence cannot be discarded to prove an otherwise-empty original transcript empty");

    for (const auto& invalid : std::vector<std::vector<Json>>{
            {header("different-id", root)}, {header("native-a", root, 1)}, {header("native-a", root, 4)},
            {header("native-a", root), message("orphan", "absent-parent", "user", Json("lost"))},
            {header("native-a", root), entry("future_record", "unknown", "")},
            {header("native-a", root), message("same", "", "user", Json("one")), message("same", "", "user", Json("two"))}})
    {
        write_records(source, invalid);
        const auto result = inspect(4);
        check(result.availability == addon::Resume_availability::UNKNOWN && result.content == addon::Resume_content::UNKNOWN,
            "foreign, unsupported, orphaned and duplicate native records remain unknown");
    }
    write_records(source, records, "broken complete record\n");
    check(inspect(4).availability == addon::Resume_availability::UNKNOWN, "malformed complete JSONL is never silently skipped");
    fs::remove(source);
    check(inspect(4).availability == addon::Resume_availability::ABSENT, "confirmed missing exact file is absent");
    fs::create_directory(source);
    check(inspect(4).availability == addon::Resume_availability::UNKNOWN, "an inaccessible file shape is unknown, not absent");
    fs::remove(source);
    inspection.resume_record = json::object({{"provider_session_id", Json("native-a")}});
    check(inspect(4).availability == addon::Resume_availability::UNKNOWN, "memory-only and old locator-free records never guess a native file");
    write_records(source, records);
    check(!pi::read_native_session(source, "native-a", [] { return true; }, &failure) && failure.reason_code == "cancelled",
        "native reads honor cancellation");
    request.output_directory = addon::path_to_utf8(root / "cancelled");
    fs::create_directory(root / "cancelled");
    check(!pi::export_conversation(request, [] { return true; }, &failure) && failure.reason_code == "cancelled" &&
        fs::is_empty(root / "cancelled"), "cancelled export publishes no file");
    check(read_text(source).find("Selected final answer") != std::string::npos, "inspection/export preserve native history");
}

void retained_content_contract(addon::Host_simulator& host, const fs::path& root)
{
    // Pi navigateTree can resetLeaf before the first user entry, then append
    // a label/model record. That selected branch is empty, but the file and
    // native session listing retain the earlier conversation on another branch.
    const auto source = root / "retained-content.jsonl";
    auto label = entry("label", "root-label", "");
    label.set("targetId", Json("retained-user"));
    label.set("label", Json("Before the first prompt"));
    auto model = entry("model_change", "root-model", "root-label");
    model.set("provider", Json("fixture-provider"));
    model.set("modelId", Json("fixture-model"));
    write_records(source, {header("retained-session", root),
        message("retained-user", "", "user", Json("Retained native conversation")),
        message("retained-answer", "retained-user", "assistant", Json("Retained native answer"), "stop"),
        label, model});

    addon::Resume_inspect_request inspection;
    inspection.context = context(root, {"pi"});
    inspection.resume_record = record(source, "retained-session");
    addon::Contract_failure failure;
    const auto parsed = pi::read_native_session(source, "retained-session", {}, &failure);
    check(parsed && parsed->branch == std::vector<std::size_t>({2, 3}), "native resetLeaf fixture restores only the metadata branch");
    const auto inspect = [&] {
        const auto payload = success(host, {"interactive-resume", 4}, "inspect_resume",
            addon::encode_resume_inspect_request(inspection));
        const auto result = addon::decode_resume_inspect_result(payload, &failure, 4);
        check(result && addon::validate_resume_inspection(inspection, *result, &failure), "retained-content inspection acknowledges its exact source");
        return *result;
    };
    const auto retained = inspect();
    check(retained.availability == addon::Resume_availability::AVAILABLE && retained.content == addon::Resume_content::NONEMPTY,
        "B1: a metadata-only selected branch cannot establish empty while another native branch retains conversation content");

    addon::Conversation_export_request request;
    request.provider_session_id = "retained-session";
    request.resume_record = inspection.resume_record;
    request.working_directory = addon::path_to_utf8(root);
    request.launch_environment = *inspection.context.launch_environment;
    request.output_directory = addon::path_to_utf8(root / "retained-export");
    fs::create_directory(root / "retained-export");
    const auto exported = invoke(host, {"conversation-export", 1}, "export", addon::encode_conversation_export_request(request));
    check(exported.status == addon::Host_status::UNAVAILABLE && exported.reason_code == "conversation_empty",
        "export still follows the persisted selected branch rather than exporting other retained branches");

    model.set("parentId", Json());
    write_records(source, {header("retained-session", root), model});
    check(inspect().content == addon::Resume_content::EMPTY, "a complete file containing only initialization metadata remains empty");
}

void block_phase_contract(addon::Host_simulator& host, const fs::path& root)
{
    // Pi's OpenAI Responses writer can retain commentary and final_answer
    // TextSignatureV1 blocks in the same assistant message with stopReason=stop.
    const auto source = root / "block-phases.jsonl";
    const auto text = [](std::string_view value, std::string_view signature) {
        return json::object({{"type", Json("text")}, {"text", Json(value)}, {"textSignature", Json(signature)}});
    };
    write_records(source, {header("phase-session", root),
        message("phase-user", "", "user", Json("Phase-aware source")),
        message("phase-answer", "phase-user", "assistant", Json(Json::Array{
            text("Signed commentary", R"({"v":1,"id":"commentary-id","phase":"commentary"})"),
            text("Signed final answer", R"({"v":1,"id":"final-id","phase":"final_answer"})"),
            text("Unspecified native phase", R"({"v":1,"id":"unspecified-id"})"),
            text("Future native phase", R"({"v":1,"id":"future-phase-id","phase":"future"})"),
            text("Future signature version", R"({"v":2,"id":"future-version-id","phase":"commentary"})"),
            text("Legacy signature", "legacy-response-id"),
            text("Malformed signature", "{incomplete"),
            json::object({{"type", Json("text")}, {"text", Json("Absent signature")}}),
        }), "stop")});

    addon::Conversation_export_request request;
    request.provider_session_id = "phase-session";
    request.resume_record = record(source, "phase-session");
    request.working_directory = addon::path_to_utf8(root);
    request.launch_environment = *context(root, {"pi"}).launch_environment;
    request.format = addon::Conversation_export_format::CONVERSATION;
    const auto export_text = [&](bool compact, std::string_view directory) {
        request.compact = compact;
        request.output_directory = addon::path_to_utf8(root / directory);
        fs::create_directory(root / directory);
        const auto payload = success(host, {"conversation-export", 1}, "export",
            addon::encode_conversation_export_request(request));
        addon::Contract_failure failure;
        const auto result = addon::decode_conversation_export_result(payload, &failure);
        check(result && result->compact == compact, "phase-aware export acknowledges the requested projection");
        return read_text(root / directory / result->file_name);
    };
    const auto compact = export_text(true, "phase-compact");
    check(compact.find("Signed commentary") == std::string::npos,
        "B2: native commentary text is excluded from compact export even when its enclosing message ends with stopReason stop");
    const auto full = export_text(false, "phase-full");
    check(full.find("::: assistant commentary\nSigned commentary") != std::string::npos &&
        full.find("::: assistant final\nSigned final answer") != std::string::npos,
        "highlighted export preserves per-block native phases and original block order");
    check(full.find("Signed commentary") < full.find("Signed final answer"), "phase grouping preserves native block order");
    check(compact.find("::: assistant final\nSigned final answer") != std::string::npos, "compact export retains authoritative final-answer text");
    for (const auto unknown : {"Unspecified native phase", "Future native phase", "Future signature version",
            "Legacy signature", "Malformed signature", "Absent signature"})
    {
        check(compact.find(std::string("::: assistant\n") + unknown) != std::string::npos &&
            full.find(std::string("::: assistant\n") + unknown) != std::string::npos,
            "unknown native text phase remains visible and unlabeled: " + std::string(unknown));
    }
}

Json resume_contract(addon::Host_simulator& host, const fs::path& root, const fs::path& source,
    std::string_view id, const std::vector<std::string>& prefix)
{
    auto words = prefix;
    words.insert(words.end(), {"--provider", "logonomic-fixture", "--model", "fixture-model", "--thinking", "off",
        "--continue", "--resume", "--session", "wrong.jsonl", "--fork", "other.jsonl", "--no-session",
        "--verbose", "--offline", "original prompt", "@original-file"});
    addon::Resume_prepare_request request;
    request.context = context(root, words);
    request.context.declared_assets = {"assets/logonomic-pi.mjs"};
    request.resume_record = record(source, id);
    const auto payload = success(host, {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(request));
    addon::Contract_failure failure;
    const auto result = addon::decode_resume_prepare_result(payload, &failure);
    check(result && addon::validate_resume_process_proposal(result->process, &failure) &&
        addon::validate_resume_acknowledgment(request.resume_record, result->acknowledgment, &failure),
        "SDK validates attached resume with exact record acknowledgment");
    const auto& arguments = result->process.argv;
    check(arguments.size() >= prefix.size() + 2 && std::equal(prefix.begin(), prefix.end(), arguments.begin()),
        "resume retains the verified executable/Node prefix");
    check(std::find(arguments.begin(), arguments.end(), "fixture-model") != arguments.end() &&
        std::find(arguments.begin(), arguments.end(), "--verbose") != arguments.end(), "authored model and options remain");
    for (const auto removed : {"--continue", "--resume", "--fork", "--no-session", "wrong.jsonl", "other.jsonl",
            "original prompt", "@original-file"})
    {
        check(std::find(arguments.begin(), arguments.end(), removed) == arguments.end(), "resume removes conflicting selector or previous prompt");
    }
    check(arguments[arguments.size() - 2] == "--session" && arguments.back() == addon::path_to_utf8(source),
        "native exact path is the sole resume selector");
    check(fs::is_regular_file(addon::path_from_utf8(arguments[prefix.size() + 1])) &&
        result->process.assets.empty() && result->process.environment.empty() && result->patch.set.empty(),
        "literal managed extension comes from SDK custody without unsupported process asset expansion");
    return payload;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool native = argc == 9 && std::string_view(argv[1]) == "--native-resume";
    const bool regression = argc == 4 && std::string_view(argv[1]) == "--regression" &&
        (std::string_view(argv[2]) == "retained-content" || std::string_view(argv[2]) == "block-phase");
    if (argc != 2 && !native && !regression) {
        std::cerr << "Usage: pi_session_tests <package> | --regression <retained-content|block-phase> <package> | "
            "--native-resume <package> <node> <cli> <session-file> <id> <cwd> <output>\n";
        return 2;
    }
    try {
        Fixture_root fixture;
        addon::Simulator_options options;
        options.package_directory = addon::path_from_utf8(argv[regression ? 3 : native ? 2 : 1]);
        options.scratch_directory = fixture.path / "host";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure failure;
        check(host.start(&failure), "SDK host starts the packaged adapter: " + failure.reason_code + " " + failure.detail);
        if (regression) {
            if (std::string_view(argv[2]) == "retained-content") {
                retained_content_contract(host, fixture.path);
            }
            else {
                block_phase_contract(host, fixture.path);
            }
        }
        else
        if (native) {
            auto payload = resume_contract(host, addon::path_from_utf8(argv[7]), addon::path_from_utf8(argv[5]), argv[6], {argv[3], argv[4]});
            std::ofstream output(addon::path_from_utf8(argv[8]));
            output << addon::serialize_json(payload);
            check(output.good(), "native preparation proposal is written for the bridge fixture");
        }
        else {
            native_contract(host, fixture.path);
            retained_content_contract(host, fixture.path);
            block_phase_contract(host, fixture.path);
            const auto bundle = fixture.path / "bundle";
            fs::create_directory(bundle);
#ifdef _WIN32
            const auto executable = bundle / "pi.exe";
#else
            const auto executable = bundle / "pi";
#endif
            fs::copy_file(fs::absolute(addon::path_from_utf8(argv[0])), executable);
            std::ofstream(bundle / "package.json") << R"({"name":"@earendil-works/pi-coding-agent","version":"1.5.2","bin":{"pi":"dist/bundle/cli.js"}})";
            resume_contract(host, fixture.path, fixture.path / "native.jsonl", "native-a", {addon::path_to_utf8(executable)});
        }
        check(host.shutdown(2000, nullptr), "SDK host shuts down cleanly");
        std::cout << "Passed " << assertions << " native history and SDK session contract assertions.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Session contract failed: " << error.what() << '\n';
        return 1;
    }
}
