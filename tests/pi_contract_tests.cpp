#include "logonomic_cli_addon/context_state.h"
#include "logonomic_cli_addon/execution_context.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace json  = logonomic::cli_addon::json;
namespace fs    = std::filesystem;

constexpr std::string_view k_nonce = "0123456789abcdef0123456789abcdef";
constexpr std::string_view k_other_nonce = "11111111111111111111111111111111";
constexpr std::string_view k_asset = "assets/logonomic-pi.mjs";
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
    fs::path path = fs::temp_directory_path() / ("logonomic-pi-contract-" + addon::random_nonce_hex());
    Fixture_root() { fs::create_directories(path); }
    ~Fixture_root()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

addon::Host_outcome invoke(
    addon::Host_simulator& host, const addon::Capability_pair& capability,
    const std::string& operation, addon::Json_value payload)
{
    const auto request = host.invoke(capability, operation, std::move(payload), 3000);
    const auto outcome = host.wait_outcome(request, 4000);
    check(outcome.has_value(), operation + " returns through the SDK host protocol");
    return *outcome;
}

addon::Json_value success(
    addon::Host_simulator& host, const addon::Capability_pair& capability,
    const std::string& operation, addon::Json_value payload)
{
    auto result = invoke(host, capability, operation, std::move(payload));
    check(result.status == addon::Host_status::SUCCESS,
        operation + " succeeds: " + result.reason_code + " " + result.message);
    return std::move(result.payload);
}

addon::Json_value native_body()
{
    return json::object({
        {"session_id",   addon::Json_value("native-session-a")},
        {"session_file", addon::Json_value("/pi/sessions/a.jsonl")},
        {"cwd",          addon::Json_value("/project")},
        {"event_id",     addon::Json_value("native-event-1")},
        {"turn_id",      addon::Json_value("root-turn-1")},
        {"attention_id", addon::Json_value("")},
    });
}

void lifecycle_contract(addon::Host_simulator& host)
{
    addon::Contract_failure failure;
    std::optional<addon::Lifecycle_declaration> declaration;
    for (const auto& sidecar : host.package()->sidecars) {
        if (sidecar.capability_id == "lifecycle-events" && sidecar.version == 2) {
            declaration = addon::decode_lifecycle_declaration(sidecar.payload, &failure);
        }
    }
    check(declaration.has_value(), "the packaged lifecycle declaration is valid");

    addon::Classify_event_request request;
    request.transition_nonce = k_nonce;
    request.event.body = native_body();
    const auto classify = [&](std::string name) {
        request.event.name = std::move(name);
        const auto payload = success(host, {"helper-ingress", 2}, "classify_event",
            addon::encode_classify_event_request(request));
        const auto result = addon::decode_classify_event_result(payload, &failure, 2);
        check(result.has_value(), "the SDK admits the complete classification reply: " + failure.reason_code);
        for (const auto& observation : result->observations) {
            check(addon::lifecycle_profile_admits(declaration->features, observation),
                "every emitted observation is admitted by the package's real lifecycle profile");
        }
        return *result;
    };

    request.event.body.set("reason", addon::Json_value("startup"));
    auto result = classify("session_start");
    check(result.classification == addon::Classification::TRANSITION &&
        result.transition == addon::Transition_kind::START && result.observations.size() == 1,
        "native startup opens one host-owned transition");
    const auto& started = result.observations.front();
    check(started.provider_session_id == "native-session-a" &&
        started.provider_event_id == "native-event-1" && started.provider_turn_id.empty() &&
        started.session_transition == addon::Session_transition::STARTED &&
        started.phase_transition == addon::Phase_transition::NONE,
        "startup preserves native identity without inventing work or completion");

    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "native-session-a", std::string(k_nonce)}};
    request.event.body = native_body();
    result = classify("agent_start");
    check(result.classification == addon::Classification::EVENT && result.observations.size() == 1 &&
        result.observations.front().phase_transition == addon::Phase_transition::WORKING &&
        result.observations.front().phase_evidence == addon::Evidence::CONFIRMED &&
        result.observations.front().provider_turn_id == "root-turn-1" &&
        result.observations.front().outcome_transition == addon::Outcome_transition::NONE,
        "native agent start confirms work for the same root turn");

    const std::pair<std::string_view, addon::Outcome_transition> outcomes[] = {
        {"succeeded", addon::Outcome_transition::SUCCEEDED},
        {"failed", addon::Outcome_transition::FAILED},
        {"interrupted", addon::Outcome_transition::INTERRUPTED},
        {"none", addon::Outcome_transition::NONE},
    };
    for (const auto& [name, expected] : outcomes) {
        request.event.body.set("aborted", addon::Json_value(false));
        request.event.body.set("outcome", addon::Json_value(name));
        result = classify("agent_settled");
        const auto& observation = result.observations.front();
        check(observation.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
            observation.phase_evidence == addon::Evidence::CONFIRMED &&
            observation.outcome_transition == expected &&
            observation.outcome_evidence == (expected == addon::Outcome_transition::NONE
                ? addon::Evidence::NONE : addon::Evidence::CONFIRMED) &&
            observation.provider_turn_id == "root-turn-1",
            "settled reports only the native terminal evidence for " + std::string(name));
    }
    request.event.body.set("aborted", addon::Json_value(true));
    request.event.body.set("outcome", addon::Json_value("none"));
    check(classify("agent_settled").observations.front().outcome_transition == addon::Outcome_transition::INTERRUPTED,
        "native aborted evidence confirms interruption even without an assistant message");

    request.event.body = native_body();
    check(classify("tool_execution_end").classification == addon::Classification::IGNORED,
        "tool completion cannot invent a root-turn completion");
    request.event.body.set("attention_id", addon::Json_value("prompt-1"));
    for (const auto& [name, expected] : std::vector<std::pair<std::string, addon::Attention_transition>>{
            {"ui_prompt_start", addon::Attention_transition::REQUESTED},
            {"ui_prompt_end", addon::Attention_transition::RESOLVED}})
    {
        result = classify(name);
        const auto& observation = result.observations.front();
        check(observation.attention_request_id == "prompt-1" &&
            observation.attention_transition == expected && observation.attention_evidence == addon::Evidence::CONFIRMED &&
            observation.phase_transition == addon::Phase_transition::NONE &&
            observation.outcome_transition == addon::Outcome_transition::NONE,
            "native prompts pair by identity without completing the root turn");
    }

    request.event.body = native_body();
    request.transition_nonce = k_other_nonce;
    check(classify("agent_start").classification == addon::Classification::IGNORED,
        "A -> B -> A rejects delayed work from the old A activation");
    request.transition_nonce = k_nonce;
    request.ledger.current_transition->provider_session_id = "native-session-b";
    check(classify("agent_start").classification == addon::Classification::IGNORED,
        "native events cannot be attached to another active conversation");
    request.ledger.current_transition->provider_session_id = "native-session-a";

    for (const auto reason : {"new", "resume", "fork", "reload"}) {
        request.event.body.set("reason", addon::Json_value(reason));
        check(classify("session_shutdown").classification == addon::Classification::IGNORED,
            "an internal Pi session switch does not end the Logonomic terminal");
        result = classify("session_start");
        check(result.classification == addon::Classification::TRANSITION &&
            result.transition == ((std::string_view(reason) == "resume" || std::string_view(reason) == "reload")
                ? addon::Transition_kind::RESUME : addon::Transition_kind::START),
            "native session selection establishes its actual transition kind");
    }
    request.event.body.set("reason", addon::Json_value("quit"));
    result = classify("session_shutdown");
    check(result.observations.front().session_transition == addon::Session_transition::ENDED &&
        result.observations.front().session_end_reason == "quit" && result.observations.front().provider_turn_id.empty(),
        "only native quit ends the selected session");

    request.event.name = "agent_start";
    request.event.body = native_body();
    request.event.body.set("turn_id", addon::Json_value(""));
    const auto malformed = invoke(host, {"helper-ingress", 2}, "classify_event",
        addon::encode_classify_event_request(request));
    check(malformed.status == addon::Host_status::FAILED && malformed.reason_code == "invalid_pi_event",
        "missing native turn identity is refused instead of synthesized");
    const auto unsupported = invoke(host, {"lifecycle-events", 2}, "observe", addon::Json_value::object());
    check(unsupported.status == addon::Host_status::UNAVAILABLE && unsupported.reason_code == "unknown_operation",
        "events-mode lifecycle does not claim a continuous observer");
}

void telemetry_contract(addon::Host_simulator& host, const fs::path& root)
{
    // Pi appends compactions durably and retains earlier branches. Repeated
    // summary text can make its callback identify an earlier compaction.
    const auto history = root / "telemetry-session.jsonl";
    std::ofstream(history) << R"({"type":"session","version":3,"id":"native-session-a","timestamp":"2026-11-16T00:00:00.000Z","cwd":"/project"})" << '\n'
        << R"({"type":"message","id":"user-1","parentId":null,"timestamp":"2026-11-16T00:00:00.000Z","message":{"role":"user","content":"first turn","timestamp":1}})" << '\n'
        << R"({"type":"compaction","id":"compact-1","parentId":"user-1","timestamp":"2026-11-16T00:00:00.000Z","summary":"same summary","firstKeptEntryId":"user-1","tokensBefore":24})" << '\n';
    addon::Classify_event_request request;
    request.transition_nonce = k_nonce;
    request.event.name = "session_start";
    request.event.body = native_body();
    request.event.body.set("reason", addon::Json_value("startup"));
    request.event.body.set("session_file", addon::Json_value(addon::path_to_utf8(history)));
    addon::Context_update usage;
    usage.provider_session_id = "native-session-a";
    usage.event_id = "native-event-1";
    usage.used_tokens = 24;
    usage.context_window = 100;
    addon::encode_context_updates(request.event.body, {usage}, std::nullopt);
    addon::Execution_context_update execution;
    execution.provider_session_id = "native-session-a";
    execution.transition_nonce = k_nonce;
    execution.status = addon::Execution_context_status::CONFIRMED;
    execution.environment = "local";
    execution.cwd = addon::path_to_utf8(root / "nested");
    execution.source = "pi.session_start";
    addon::encode_execution_updates(request.event.body, {execution});
    const auto classify = [&] {
        addon::Contract_failure failure;
        const auto payload = success(host, {"helper-ingress", 2}, "classify_event",
            addon::encode_classify_event_request(request));
        const auto result = addon::decode_classify_event_result(payload, &failure, 2);
        check(result.has_value(), "SDK admits isolated telemetry with its lifecycle result: " + failure.reason_code);
        return *result;
    };
    auto result = classify();
    check(result.observations.size() == 1 && result.context_updates.empty() && result.execution_updates.empty(),
        "unrequested optional telemetry is omitted without suppressing startup");
    request.context_state_version = 2;
    request.execution_context_version = 1;
    request.context_snapshot_revision = 1;
    result = classify();
    check(result.observations.size() == 1 && result.context_updates.size() == 1 && result.execution_updates.size() == 1,
        "joint startup carries one native context and execution observation");
    check(result.context_updates.front().used_tokens == 24 && result.context_updates.front().context_window == 100 &&
        result.context_updates.front().compaction_count == 1 && result.context_updates.front().compaction_count_revision == 1 &&
        result.context_updates.front().compaction_count_complete && !result.context_updates.front().compacted,
        "fresh host revision authorizes the full native history count without callback increments");
    check(result.execution_updates.front() == execution,
        "execution context preserves the exact native subdirectory, activation and sequence");

    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "native-session-a", std::string(k_nonce)}};
    request.event.name = "context_snapshot";
    std::ofstream(history, std::ios::app)
        << R"({"type":"message","id":"user-2","parentId":null,"timestamp":"2026-11-16T00:00:01.000Z","message":{"role":"user","content":"new branch","timestamp":2}})" << '\n'
        << R"({"type":"compaction","id":"compact-2","parentId":"user-2","timestamp":"2026-11-16T00:00:01.000Z","summary":"same summary","firstKeptEntryId":"user-2","tokensBefore":24})" << '\n';
    request.context_snapshot_revision = 2;
    result = classify();
    check(result.classification == addon::Classification::IGNORED && result.observations.empty() &&
        result.context_updates.front().compaction_count == 2 &&
        result.context_updates.front().compaction_count_revision == 2 && result.context_updates.front().compaction_count_complete,
        "a new revision rereads all persisted branches and counts identical-summary compactions separately");

    std::ofstream(history, std::ios::app) << "{\"type\":\"message\"";
    request.context_snapshot_revision = 3;
    result = classify();
    check(result.context_updates.front().compaction_count == 2 &&
        !result.context_updates.front().compaction_count_complete && result.context_updates.front().used_tokens == 24,
        "an incomplete append withdraws count completeness while preserving native occupancy");
    request.event.body.set("session_file", addon::Json_value(addon::path_to_utf8(root / "absent.jsonl")));
    request.context_snapshot_revision = 4;
    result = classify();
    check(result.context_updates.front().compaction_count == 0 &&
        !result.context_updates.front().compaction_count_complete && result.context_updates.front().used_tokens == 24,
        "unreadable history supplies only a fresh lower bound and cannot erase independent occupancy");
    usage.used_tokens.reset();
    usage.context_window.reset();
    usage.usage_unavailable = true;
    addon::encode_context_updates(request.event.body, {usage}, std::nullopt);
    request.context_snapshot_revision.reset();
    result = classify();
    check(result.context_updates.front().usage_unavailable && !result.context_updates.front().used_tokens &&
        !result.context_updates.front().compaction_count,
        "unknown post-compaction native usage stays unknown and no unsolicited history revision is invented");

    request.transition_nonce = k_other_nonce;
    result = classify();
    check(result.context_updates.empty() && result.execution_updates.empty() && result.observations.empty(),
        "a stale activation cannot refresh either optional capability");
    request.transition_nonce = k_nonce;
    request.event.name = "agent_settled";
    request.event.body.set("aborted", addon::Json_value(false));
    request.event.body.set("outcome", addon::Json_value("failed"));
    request.event.body.set("context_updates", addon::Json_value("malformed context"));
    request.event.body.set("execution_context", addon::Json_value("malformed execution context"));
    result = classify();
    check(result.observations.size() == 1 &&
        result.observations.front().outcome_transition == addon::Outcome_transition::FAILED &&
        result.context_failure && result.execution_failure && result.context_updates.empty() && result.execution_updates.empty(),
        "malformed optional evidence leaves the valid native failure lifecycle intact");
    addon::encode_context_updates(request.event.body, {usage}, std::nullopt);
    execution.transition_nonce = k_other_nonce;
    addon::encode_execution_updates(request.event.body, {execution});
    result = classify();
    check(result.observations.size() == 1 && result.context_updates.size() == 1 && result.execution_failure &&
        result.execution_updates.empty(), "execution attribution failure is isolated from context and lifecycle");
}

std::string direct_script(const std::vector<std::string>& words)
{
    std::string script;
    for (const auto& word : words) {
        if (!script.empty()) {
            script += ' ';
        }
        script += '"';
        for (char character : word) {
            script += character == '"' ? "\"\"\"" : std::string(1, character);
        }
        script += '"';
    }
    return script;
}

void manifest(const fs::path& directory, std::string_view version, std::string_view name = "@earendil-works/pi-coding-agent")
{
    std::ofstream output(directory / "package.json");
    output << addon::serialize_json(json::object({
        {"name", addon::Json_value(name)}, {"version", addon::Json_value(version)},
        {"bin", json::object({{"pi", addon::Json_value("dist/bundle/cli.js")}})},
    }));
    check(output.good(), "fixture metadata is written");
}

void launcher_contract(addon::Host_simulator& host, const fs::path& root,
    const fs::path& self, const std::string& node, const std::string& real_cli)
{
    // Upstream scripts/build-binaries.sh ships native pi plus adjacent package.json.
    // The fixture binary is never executed; recognition must derive identity from that layout.
    const auto bundled = root / "native bundle";
    const auto unrelated = root / "unrelated";
    fs::create_directories(bundled);
    fs::create_directories(unrelated);
#ifdef _WIN32
    constexpr auto executable = "pi.exe";
#else
    constexpr auto executable = "pi";
#endif
    fs::copy_file(self, bundled / executable);
    fs::copy_file(self, unrelated / executable);
    manifest(bundled, "1.5.2");
    const auto ambient = json::object({
        {"PATH", addon::Json_value(addon::path_to_utf8(bundled))},
        {"PATHEXT", addon::Json_value(".EXE;.CMD;.PS1")},
    });
    addon::Contract_failure failure;
    const auto recognized = [&](const addon::Launcher_request& launcher, const addon::Json_value& environment) {
        const auto payload = success(host, {"launcher-discovery", 1}, "recognize",
            addon::encode_recognize_request(launcher, environment, addon::host_platform_id()));
        const auto result = addon::decode_recognize_result(payload, &failure);
        check(result.has_value(), "SDK decodes launcher recognition");
        return result->recognized;
    };
    check(recognized({"pi", "direct", ""}, ambient), "the documented standalone bundle is recognized");
    check(!recognized({direct_script({addon::path_to_utf8(unrelated / executable)}), "direct", ""}, ambient),
        "an unrelated executable named pi is not branded as the provider");
    check(!recognized({"pi", "direct", "PATH=" + addon::path_to_utf8(unrelated)}, ambient),
        "ambient Pi identity cannot override an authored PATH selection");
    check(!recognized({"PATH='" + addon::path_to_utf8(unrelated) + "' pi", "bash", ""}, ambient),
        "inline shell PATH assignments do not borrow ambient Pi identity");

    const auto discovered = success(host, {"launcher-discovery", 1}, "discover",
        addon::encode_discover_request(ambient, addon::path_to_utf8(root), addon::host_platform_id()));
    const auto candidates = addon::decode_discover_result(discovered, addon::host_platform_id(), &failure);
    check(candidates && candidates->size() == 1 && candidates->front().is_default &&
        recognized({candidates->front().script_body, candidates->front().runner_key, candidates->front().environment_text}, ambient),
        "discovery emits one directly recognizable, quoted launcher");

    addon::Prepare_request request;
    request.working_directory = addon::path_to_utf8(root);
    request.declared_assets = {std::string(k_asset)};
    request.launch_environment = addon::Launch_environment{addon::host_platform_id(), ambient, std::nullopt};
    const auto prepare = [&](const std::vector<std::string>& words) {
        request.launcher = {direct_script(words), "direct", ""};
        request.launcher_argv = words;
        const auto payload = success(host, {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(request, 3));
        const auto patch = addon::decode_launch_patch(payload, &failure, 3);
        check(patch.has_value() && addon::validate_launch_patch(*patch, request.declared_assets, request.declared_assets, &failure),
            "SDK validates the full launch patch: " + failure.reason_code);
        return *patch;
    };
    const std::vector<std::string> words = {addon::path_to_utf8(bundled / executable), "--provider", "test-provider",
        "--model", "test-model", "--tools", "read,edit", "a prompt with spaces"};
    auto expected = words;
    expected.insert(expected.begin() + 1, {"--extension", "{{asset:assets/logonomic-pi.mjs}}"});
    auto patch = prepare(words);
    check(patch.argv == expected && patch.disposition->prepared && patch.disposition->lifecycle_events &&
        patch.disposition->coordination_delivery && patch.set.empty() && patch.unset.empty(),
        "interactive preparation only inserts the native extension and preserves user choices");

    for (const auto argument : {"--help", "--version", "--print", "--mode", "--export", "--no-extensions", "install"}) {
        patch = prepare({words.front(), argument});
        check(!patch.disposition->prepared && !patch.disposition->lifecycle_events &&
            !patch.disposition->coordination_delivery && !patch.argv && patch.set.empty() && patch.unset.empty(),
            "noninteractive or explicitly extension-free Pi remains an unchanged ordinary launch");
    }
    // Pi's main.ts dispatches these commands from args[0], before interactive
    // argument parsing. Prepending --extension would turn them into prompt text.
    for (auto command : std::vector<std::vector<std::string>>{
            {"auth"}, {"auth", "check", "--provider", "test-provider"}, {"mcp"}, {"uninstall"}})
    {
        command.insert(command.begin(), words.front());
        patch = prepare(command);
        check(!patch.argv && !patch.disposition->prepared && !patch.disposition->lifecycle_events &&
            !patch.disposition->coordination_delivery && patch.set.empty() && patch.unset.empty(),
            "native management command retains its original leading argument: " + command[1]);
    }
    check(prepare({words.front(), "--model", "auth", "explain this code"}).disposition->prepared,
        "a command spelling used as an option value remains an interactive invocation");
    for (const auto& [version, supported] : std::vector<std::pair<std::string, bool>>{
            {"0.84.4", false}, {"1.1.0-alpha", false}, {"1.1.0", true}, {"99999.0.0", true}})
    {
        manifest(bundled, version);
        check(prepare(words).disposition->prepared == supported,
            "native API minimum is semantic and imposes no future release ceiling: " + version);
    }
    manifest(bundled, "1.5.2");
    request.declared_assets.clear();
    check(!prepare(words).disposition->prepared, "a missing managed asset never yields a broken native extension launch");
    request.declared_assets = {std::string(k_asset)};

    if (!node.empty()) {
        // Upstream package.json selects its npm bin; the historic and current paths differ.
        const auto npm = root / "npm package";
        fs::create_directories(npm / "dist/bundle");
        manifest(npm, "1.5.2");
        std::ofstream(npm / "dist/bundle/cli.js") << "#!/usr/bin/env node\n";
        const std::vector<std::string> node_words = {node, addon::path_to_utf8(npm / "dist/bundle/cli.js"), "--session", "native-session.jsonl"};
        check(recognized({direct_script(node_words), "direct", ""}, ambient),
            "the npm manifest's actual bin entrypoint is recognized through native Node");
        expected = node_words;
        expected.insert(expected.begin() + 2, {"--extension", "{{asset:assets/logonomic-pi.mjs}}"});
        check(prepare(node_words).argv == expected, "Node prefix and explicit native session selection are preserved");
        manifest(npm, "1.5.2", "unrelated-package");
        check(!recognized({direct_script(node_words), "direct", ""}, ambient),
            "an unrelated Node package is rejected despite a pi-shaped bin path");
        if (!real_cli.empty()) {
            const std::vector<std::string> actual = {node, real_cli};
            check(recognized({direct_script(actual), "direct", ""}, ambient) && prepare(actual).disposition->prepared,
                "the actual installed Pi package is recognized and prepared through its native entrypoint");
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: pi_contract_tests <package> [node] [pi-cli] | <package> --telemetry\n";
        return 2;
    }
    try {
        Fixture_root fixtures;
        addon::Simulator_options options;
        options.package_directory = addon::path_from_utf8(argv[1]);
        options.scratch_directory = fixtures.path / "host";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure failure;
        check(host.start(&failure), "SDK host starts the real packaged adapter: " + failure.reason_code + " " + failure.detail);
        if (argc == 3 && std::string_view(argv[2]) == "--telemetry") {
            telemetry_contract(host, fixtures.path);
        }
        else {
            lifecycle_contract(host);
            launcher_contract(host, fixtures.path, fs::absolute(addon::path_from_utf8(argv[0])),
                argc >= 3 ? argv[2] : "", argc >= 4 ? argv[3] : "");
        }
        check(host.shutdown(2000, nullptr), "packaged adapter shuts down under the SDK host contract");
        std::cout << "Passed " << assertions << " SDK protocol and provider contract assertions.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Provider contract failed: " << error.what() << '\n';
        return 1;
    }
}
