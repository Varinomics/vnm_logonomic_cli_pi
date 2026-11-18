#include "pi_profile.h"

#include "logonomic_cli_addon/conversation_export.h"
#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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
#ifdef _WIN32
constexpr const char* k_home = "USERPROFILE";
#else
constexpr const char* k_home = "HOME";
#endif

void check(bool condition, const std::string& description)
{
    ++assertions;
    if (!condition) throw std::runtime_error(description);
}

struct Fixture_root
{
    fs::path path = fs::temp_directory_path() / ("logonomic-pi-profile-" + addon::random_nonce_hex());
    Fixture_root() { fs::create_directories(path); }
    ~Fixture_root()
    {
        std::error_code error;
        fs::remove_all(pi::native_io_path(path), error);
    }
};

addon::Launch_environment environment(Json values)
{
    return {addon::host_platform_id(), std::move(values), std::nullopt};
}

std::string quoted(const std::string& word)
{
    std::string result = "\"";
    for (const char value : word) result += value == '"' ? "\"\"\"" : std::string(1, value);
    return result + '"';
}

addon::Host_outcome invoke(addon::Host_simulator& host, const addon::Capability_pair& capability,
    std::string operation, Json payload)
{
    const auto request = host.invoke(capability, std::move(operation), std::move(payload), 5000);
    const auto outcome = host.wait_outcome(request, 6000);
    check(outcome.has_value(), "SDK host receives a bounded operation result");
    return *outcome;
}

Json success(addon::Host_simulator& host, const addon::Capability_pair& capability, std::string operation, Json payload)
{
    const auto outcome = invoke(host, capability, std::move(operation), std::move(payload));
    check(outcome.status == addon::Host_status::SUCCESS, "SDK operation succeeds: " + outcome.reason_code + " " + outcome.message);
    return outcome.payload;
}

addon::Account_record selected(addon::Host_simulator& host, const addon::Accounts_launcher_request& request)
{
    addon::Contract_failure failure;
    const auto payload = success(host, {"accounts", 2}, "discover", addon::encode_accounts_launcher_request(request));
    const auto records = addon::decode_accounts_result(payload, &failure);
    check(records && records->size() == 1, "SDK admits the exact launcher-selected profile inventory");
    const auto resolved = success(host, {"accounts", 2}, "resolve", addon::encode_accounts_launcher_request(request));
    const auto resolution = addon::decode_account_resolution_result(resolved, &failure);
    check(resolution && resolution->adapter_account_key == records->front().adapter_account_key,
        "resolution names the same exact admitted account key");
    return records->front();
}

void path_contract(const fs::path& root)
{
    const auto cwd = addon::path_to_utf8(root);
    const auto home = addon::path_to_utf8(root / "native home");
    const auto fallback = addon::path_to_utf8(root / "fallback");
    addon::Contract_failure failure;
    const auto resolve = [&](Json values, const fs::path& expected, std::string_view fallback_home = {}) {
        const auto profile = pi::resolve_native_profile(environment(std::move(values)), cwd, fallback_home, &failure);
        check(profile && profile->path == expected.lexically_normal(),
            "expected native profile " + addon::path_to_utf8(expected) + ", got " +
            (profile ? addon::path_to_utf8(profile->path) : failure.reason_code));
        check(addon::is_valid_adapter_account_key(profile->account_key), "profile identity is a bounded opaque account key");
        return *profile;
    };
    const auto primary = resolve(json::object({{k_home, Json(home)}}), root / "native home" / ".pi" / "agent", fallback);
    resolve(json::object({{k_home, Json(home)}, {"PI_CODING_AGENT_DIR", Json("")}}), primary.path, fallback);
    resolve(Json::object(), root / "fallback" / ".pi" / "agent", fallback);
    resolve(json::object({{"PI_CODING_AGENT_DIR", Json("relative/../ profile λ ")}}), root / u8" profile λ ");
    resolve(json::object({{k_home, Json(home)}, {"PI_CODING_AGENT_DIR", Json("~/team")}}), root / "native home" / "team");
    resolve(json::object({{k_home, Json(home)}, {"PI_CODING_AGENT_DIR", Json("~//team")}}), root / "native home" / "team");
    resolve(json::object({{k_home, Json(home)}, {"PI_CODING_AGENT_DIR", Json("~")}}), root / "native home");
#ifndef _WIN32
    resolve(json::object({{k_home, Json("")}}), root / ".pi" / "agent", fallback);
    resolve(json::object({{k_home, Json("relative-home")}}), root / "relative-home" / ".pi" / "agent", fallback);
#endif
    auto absolute = addon::path_to_utf8(root / "absolute-profile");
    const auto first = resolve(json::object({{"PI_CODING_AGENT_DIR", Json(absolute)}}), root / "absolute-profile");
    const auto alias = resolve(json::object({{"PI_CODING_AGENT_DIR", Json(absolute + "/../absolute-profile/.")}}), first.path);
    check(first.account_key == alias.account_key && first.account_key != primary.account_key,
        "lexical aliases share identity while different native profiles remain distinct");
    check(pi::resolve_native_profile(environment(json::object({{"PI_CODING_AGENT_DIR", Json(absolute)}})), {}, {}, &failure).has_value(),
        "an absolute native profile does not require an unrelated working-directory fallback");
    check(!pi::resolve_native_profile(environment(Json::object()), cwd, {}, &failure) &&
        failure.reason_code == "pi_profile_unavailable", "a missing frozen native home cannot borrow the adapter's ambient home");
    check(!pi::resolve_native_profile(environment(json::object({{"PI_CODING_AGENT_DIR", Json("~/team")}})), cwd, {}, &failure),
        "tilde expansion also requires frozen home evidence");
    check(!pi::resolve_native_profile(environment(json::object({{"PI_CODING_AGENT_DIR", Json("relative-profile")}})), "relative-cwd", {}, &failure),
        "native profile normalization never uses the adapter's current directory");
    for (const auto value : {"file://remote/profile", "file:///profile%2fchild", "file:///profile%00", "file:///profile%ff",
            "file:///profile%", "file:///profile?query"})
    {
        check(!pi::resolve_native_profile(environment(json::object({{k_home, Json(home)}, {"PI_CODING_AGENT_DIR", Json(value)}})),
            cwd, fallback, &failure), "unsupported file URL fails explicitly instead of selecting the default profile");
    }
}

void accounts_contract(addon::Host_simulator& host, const fs::path& root, const fs::path& executable)
{
    const auto profile_path = root / "outside home" / " team profile ";
#ifdef _WIN32
    // The native Node runtime preserves the trailing space when creating its profile.
    const fs::path native_path(L"\\\\?\\" + profile_path.native());
#else
    const auto& native_path = profile_path;
#endif
    fs::create_directories(native_path);
    // Malformed credentials must not affect filesystem account discovery.
    std::ofstream(native_path / "auth.json") << "not credential data";
    addon::Accounts_launcher_request request;
    request.launcher = {quoted(addon::path_to_utf8(executable)), "direct", ""};
    request.launch_environment = environment(json::object({
        {k_home, Json(addon::path_to_utf8(root / "unused home"))},
        {"PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(profile_path))},
    }));
    request.default_home_fallback = addon::path_to_utf8(root / "unused fallback");
    request.normalization_directory = addon::path_to_utf8(root);
    const auto profile = selected(host, request);
    check(profile.profile_path == addon::path_to_utf8(profile_path) && profile.display_name == "team profile" &&
        profile.status == addon::Account_status::AVAILABLE,
        "real folder metadata identifies the selected nondefault profile without interpreting credentials");

    const auto original = request;
    request.launch_environment.effective_environment.set("PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(root / "not-created")));
    check(selected(host, request).status == addon::Account_status::UNAVAILABLE, "an absent profile is unavailable and remains in inventory");
    const auto ordinary_file = root / "ordinary-file";
    std::ofstream(ordinary_file) << "fixture";
    request.launch_environment.effective_environment.set("PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(ordinary_file)));
    check(selected(host, request).status == addon::Account_status::UNAVAILABLE, "a regular file is not an available profile directory");
#ifndef _WIN32
    const auto loop = root / "inaccessible-loop";
    fs::create_directory_symlink(loop.filename(), loop);
    request.launch_environment.effective_environment.set("PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(loop)));
    check(selected(host, request).status == addon::Account_status::UNKNOWN, "a filesystem lookup failure is unknown rather than absent");
#endif
    check(selected(host, original).adapter_account_key == profile.adapter_account_key,
        "a later operation does not change the identity selected by an earlier frozen environment");
    request = original;
    request.launch_environment = environment(Json::object());
    const auto fallback = selected(host, request);
    check(fallback.profile_path == addon::path_to_utf8(root / "unused fallback" / ".pi" / "agent"),
        "accounts discovery uses its explicit host fallback when native home is absent");
    request.default_home_fallback.clear();
    const auto unavailable = invoke(host, {"accounts", 2}, "discover", addon::encode_accounts_launcher_request(request));
    check(unavailable.status != addon::Host_status::SUCCESS && unavailable.reason_code == "pi_profile_unavailable",
        "unknown native home is an explicit accounts failure, never an empty successful inventory");
    request = original;
    request.launcher = {quoted(addon::path_to_utf8(root / "unrecognized-pi")), "direct", ""};
    check(invoke(host, {"accounts", 2}, "discover", addon::encode_accounts_launcher_request(request)).status != addon::Host_status::SUCCESS,
        "an unidentified launcher cannot borrow another installation's profile");
#ifndef _WIN32
    request = original;
    request.launcher = {"PI_CODING_AGENT_DIR='ignored-authored-spelling' " + quoted(addon::path_to_utf8(executable)), "bash", ""};
    const auto leading = invoke(host, {"accounts", 2}, "discover", addon::encode_accounts_launcher_request(request));
    check(leading.status == addon::Host_status::SUCCESS,
        "frozen leading profile assignment must resolve the selected Pi account: " + leading.reason_code);
    addon::Contract_failure leading_failure;
    const auto leading_accounts = addon::decode_accounts_result(leading.payload, &leading_failure);
    check(leading_accounts && leading_accounts->size() == 1 && leading_accounts->front().adapter_account_key == profile.adapter_account_key,
        "a recognized leading assignment takes its exact value from the host's already-frozen evaluation");
    request.launch_environment = environment(json::object({{k_home, Json(addon::path_to_utf8(root))}}));
    check(invoke(host, {"accounts", 2}, "discover", addon::encode_accounts_launcher_request(request)).status != addon::Host_status::SUCCESS,
        "an unexposed leading assignment cannot fall back to a different default profile");
#endif
    request = original;
    request.launcher.environment_text = "PI_CODING_AGENT_DIR=already-evaluated-by-host";
    check(selected(host, request).adapter_account_key == profile.adapter_account_key,
        "explicit launcher environment also uses the host's frozen value without a second evaluation");
}

#ifdef _WIN32
bool case_sensitive_accounts(addon::Host_simulator& host, const fs::path& root, const fs::path& executable)
{
    const auto parent = root / "profiles";
    fs::create_directory(parent);
    const auto upper = parent / "Work";
    const auto lower = parent / "work";
    addon::Accounts_launcher_request request;
    request.launcher = {quoted(addon::path_to_utf8(executable)), "direct", ""};
    request.normalization_directory = addon::path_to_utf8(root);
    const auto discover = [&](const fs::path& path) {
        request.launch_environment = environment(json::object({{"PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(path))}}));
        return selected(host, request).adapter_account_key;
    };
    const auto prepare = [&](addon::Account_context grant) {
        addon::Prepare_request launch;
        launch.launcher           = request.launcher;
        launch.launcher_argv      = {addon::path_to_utf8(executable)};
        launch.working_directory  = request.normalization_directory;
        launch.declared_assets    = {"assets/logonomic-pi.mjs"};
        launch.launch_environment = request.launch_environment;
        launch.account_context    = std::move(grant);
        return invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch));
    };
    const auto legacy = discover(upper);
    const auto& path = upper.native();
    const int length = static_cast<int>(path.size());
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), length, nullptr, 0, nullptr, nullptr, 0);
    check(count > 0, "obtain the original Windows profile spelling");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    check(LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), length, folded.data(), count, nullptr, nullptr, 0) == count &&
        legacy == "profile-" + addon::to_hex(addon::Sha256::digest(addon::path_to_utf8(fs::path(folded)))) &&
        discover(lower) == legacy, "ordinary profiles retain their exact preexisting opaque keys");
    check(prepare({1, legacy, std::nullopt}).status == addon::Host_status::SUCCESS &&
        prepare({2, legacy, addon::path_to_utf8(upper)}).status == addon::Host_status::SUCCESS,
        "established grants continue to work on a case-insensitive filesystem");

    const HANDLE directory = CreateFileW(parent.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(directory != INVALID_HANDLE_VALUE, "open the temporary profile directory");
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &info, sizeof(info)) != 0;
    const auto error = GetLastError();
    CloseHandle(directory);
    if (!enabled && (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER ||
            error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)) {
        return false;
    }
    check(enabled && fs::create_directory(upper) && fs::create_directory(lower), "create distinct native Work and work profiles");
    const auto upper_key = discover(upper);
    const auto lower_key = discover(lower);
    check(upper_key != lower_key && upper_key != legacy && lower_key != legacy, "case-sensitive siblings have independent identities");
    check(prepare({1, legacy, std::nullopt}).reason_code == "account_profile_mismatch" &&
        prepare({1, upper_key, std::nullopt}).reason_code == "account_profile_mismatch" &&
        prepare({2, legacy, addon::path_to_utf8(upper)}).reason_code == "account_profile_mismatch" &&
        prepare({2, "unrelated-key", addon::path_to_utf8(lower)}).reason_code == "account_profile_mismatch" &&
        prepare({2, lower_key, addon::path_to_utf8(upper)}).reason_code == "account_profile_mismatch",
        "a collapsed historic key or a different native path cannot authorize the selected profile");
    check(prepare({1, lower_key, std::nullopt}).status == addon::Host_status::SUCCESS &&
        prepare({2, lower_key, addon::path_to_utf8(lower)}).status == addon::Host_status::SUCCESS &&
        prepare({2, legacy, addon::path_to_utf8(lower)}).status == addon::Host_status::SUCCESS,
        "current grants and a matching path-bearing historical grant prepare successfully");
    check(fs::remove(upper) && fs::remove(lower) &&
        prepare({1, lower_key, std::nullopt}).status == addon::Host_status::SUCCESS,
        "a missing leaf under a known sensitive parent retains its distinct grant");
    check(fs::remove(parent), "remove the former case-sensitive parent");
    check(prepare({1, legacy, std::nullopt}).reason_code == "account_profile_mismatch",
        "a disappeared intermediate directory cannot turn an ambiguous historical key into a valid grant");
    return true;
}
#endif

void scoped_operations(addon::Host_simulator& host, const fs::path& root, const fs::path& executable)
{
    const auto native = root / "native-profile";
    fs::create_directory(native);
    const auto native_environment = environment(json::object({{"PI_CODING_AGENT_DIR", Json(addon::path_to_utf8(native))}}));
    addon::Contract_failure failure;
    const auto profile = pi::resolve_native_profile(native_environment, addon::path_to_utf8(root), {}, &failure);
    check(profile.has_value(), "explicit native profile is independent of an absent home");
    const auto managed = root / "host-managed-scope";
    fs::create_directory(managed);
    const addon::Profile_storage_context storage{1, addon::path_to_utf8(managed)};
    const auto acknowledgment = success(host, {"profile-storage", 1}, "acknowledge",
        json::object({{"profile_storage", addon::profile_storage_member(storage)}}));
    check(!addon::profile_storage_acknowledgment_failure(storage, acknowledgment), "managed storage echoes its exact host-owned version and root");
    check(invoke(host, {"profile-storage", 1}, "unexpected", json::object({{"profile_storage", addon::profile_storage_member(storage)}})).status !=
        addon::Host_status::SUCCESS, "profile storage rejects unknown operations");

    addon::Prepare_request launch;
    launch.launcher = {quoted(addon::path_to_utf8(executable)), "direct", ""};
    launch.launcher_argv = {addon::path_to_utf8(executable)};
    launch.working_directory = addon::path_to_utf8(root);
    launch.declared_assets = {"assets/logonomic-pi.mjs"};
    launch.launch_environment = native_environment;
    launch.profile_storage = storage;
    for (const std::uint16_t version : {1, 2}) {
        launch.account_context = addon::Account_context{version, profile->account_key,
            version == 2 ? std::optional(addon::path_to_utf8(native)) : std::nullopt};
        const auto payload = success(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch));
        const auto patch = addon::decode_launch_patch(payload, &failure);
        check(patch && patch->disposition->prepared && patch->set.empty() && patch->unset.empty(),
            "account selection preserves the authored native profile and produces a supported launch");
        check(!addon::account_context_acknowledgment_failure(*launch.account_context, payload) &&
            !addon::profile_storage_acknowledgment_failure(storage, payload), "both context versions and storage acknowledge every exact member");
    }
    const auto account = *launch.account_context;
    const auto rejected_launch = [&](addon::Account_context selected_account, std::string_view reason) {
        launch.account_context = std::move(selected_account);
        const auto outcome = invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch));
        check(outcome.status != addon::Host_status::SUCCESS && outcome.reason_code == reason,
            "a mismatched selected account cannot receive a successful preparation echo");
    };
    rejected_launch({1, "unrelated-profile", std::nullopt}, "account_profile_mismatch");
    rejected_launch({2, account.adapter_account_key, std::nullopt}, "account_profile_mismatch");
    rejected_launch({2, account.adapter_account_key, addon::path_to_utf8(root / "other-profile")}, "account_profile_mismatch");
    launch.account_context = account;
    launch.launch_environment = environment(Json::object());
    rejected_launch(account, "pi_profile_unavailable");
    launch.launch_environment = native_environment;
    launch.account_context = account;
    const auto selected_launcher = launch.launcher;
    const auto selected_argv = launch.launcher_argv;
    launch.launcher = {quoted(addon::path_to_utf8(root / "unrecognized-pi")), "direct", ""};
    launch.launcher_argv = {addon::path_to_utf8(root / "unrecognized-pi")};
    check(invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch)).reason_code == "launcher_program_unavailable",
        "selected account preparation cannot acknowledge an unidentified replacement launcher");
    launch.launcher = selected_launcher;
    launch.launcher_argv = selected_argv;
#ifndef _WIN32
    launch.launcher = {"PI_CODING_AGENT_DIR='already-evaluated-by-host' " + selected_launcher.script_body, "bash", ""};
    launch.launcher_argv = {"/bin/bash", "-c", launch.launcher.script_body};
    const auto assigned_launch = success(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch));
    const auto assigned_patch = addon::decode_launch_patch(assigned_launch, &failure);
    check(assigned_patch && assigned_patch->disposition->prepared && assigned_patch->argv &&
        assigned_patch->argv->front() == addon::path_to_utf8(executable) &&
        assigned_patch->set == addon::Environment{{"PI_CODING_AGENT_DIR", addon::path_to_utf8(native)}} &&
        !addon::account_context_acknowledgment_failure(account, assigned_launch),
        "frozen leading assignment preparation preserves the profile overlay and verified Pi executable");
#endif

    const auto source = root / "selected-session.jsonl";
    std::ofstream(source) << R"({"type":"session","version":3,"id":"scoped-session","timestamp":"2026-11-16T00:00:00.000Z","cwd":"/fixture"})" << '\n'
        << R"({"type":"message","id":"user","parentId":null,"timestamp":"2026-11-16T00:00:01.000Z","message":{"role":"user","content":"Retained native conversation","timestamp":1794787201000}})" << '\n';
    addon::Resume_prepare_request resume;
    resume.context.launcher = launch.launcher;
    resume.context.launcher_argv = launch.launcher_argv;
    resume.context.working_directory = launch.working_directory;
    resume.context.declared_assets = launch.declared_assets;
    resume.context.launch_environment = launch.launch_environment;
    resume.context.profile_storage = storage;
    resume.context.account_context = account;
    resume.resume_record = json::object({{"provider_session_id", Json("scoped-session")}, {"session_file", Json(addon::path_to_utf8(source))}});
    const auto resumed = success(host, {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume));
    const auto proposal = addon::decode_resume_prepare_result(resumed, &failure);
    check(proposal && !addon::account_context_acknowledgment_failure(account, resumed) &&
        !addon::profile_storage_acknowledgment_failure(storage, resumed),
        "native resume preserves the remembered account and independent managed root");
    addon::Resume_inspect_request inspection;
    inspection.context = resume.context;
    inspection.context.declared_assets.clear();
    inspection.resume_record = resume.resume_record;
    auto inspected = success(host, {"interactive-resume", 4}, "inspect_resume", addon::encode_resume_inspect_request(inspection));
    auto evidence = addon::decode_resume_inspect_result(inspected, &failure, 4);
    check(evidence && evidence->availability == addon::Resume_availability::AVAILABLE &&
        addon::validate_resume_inspection(inspection, *evidence, &failure), "same-account resume inspection binds the complete request");
    resume.context.account_context = addon::Account_context{1, "different-profile", std::nullopt};
    check(invoke(host, {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume)).reason_code == "account_profile_mismatch",
        "a current launcher cannot replace the remembered account during resume");
    inspection.context.account_context = resume.context.account_context;
    inspected = success(host, {"interactive-resume", 4}, "inspect_resume", addon::encode_resume_inspect_request(inspection));
    evidence = addon::decode_resume_inspect_result(inspected, &failure, 4);
    check(evidence && evidence->availability == addon::Resume_availability::UNKNOWN,
        "a mismatched inspection is unknown, never evidence that the remembered conversation was deleted");

    addon::Conversation_export_request export_request;
    export_request.provider_session_id = "scoped-session";
    export_request.resume_record = resume.resume_record;
    export_request.working_directory = launch.working_directory;
    export_request.launch_environment = native_environment;
    export_request.account_context = account;
    export_request.profile_storage = storage;
    export_request.output_directory = addon::path_to_utf8(root / "export");
    fs::create_directory(addon::path_from_utf8(export_request.output_directory));
    const auto exported = success(host, {"conversation-export", 1}, "export", addon::encode_conversation_export_request(export_request));
    check(!addon::account_context_acknowledgment_failure(account, exported) && !addon::profile_storage_acknowledgment_failure(storage, exported),
        "export acknowledges the recorded source account and managed root");
    export_request.account_context = resume.context.account_context;
    check(invoke(host, {"conversation-export", 1}, "export", addon::encode_conversation_export_request(export_request)).reason_code == "account_profile_mismatch",
        "mismatched export source custody fails before publishing content");
}

void native_paths(const fs::path& fixture)
{
    std::ifstream input(fixture, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto cases = addon::parse_json(bytes, {}, nullptr);
    check(cases && cases->is_array() && !cases->as_array().empty(), "installed Pi supplies native profile path cases");
    for (const auto& item : cases->as_array()) {
        addon::Contract_failure failure;
        const auto actual = pi::resolve_native_profile(environment(json::member(item, "environment")),
            json::text(item, "cwd"), {}, &failure);
        check(actual && addon::path_to_utf8(actual->path) == json::text(item, "expected"),
            "profile path matches installed Pi getAgentDir/resolvePath: " + json::text(item, "name") + " " + failure.reason_code);
    }
}

void native_handoff(addon::Host_simulator& host, const fs::path& package, const fs::path& request_file, const fs::path& output_file)
{
    std::ifstream input(request_file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto fixture = addon::parse_json(bytes, {}, nullptr);
    check(fixture && fixture->is_object(), "native handoff fixture is valid JSON");
    addon::Contract_failure failure;
    auto launch = addon::decode_prepare_request(json::member(*fixture, "launch"), &failure);
    check(launch.has_value(), "native handoff uses the SDK preparation request: " + failure.reason_code);
    const auto account = selected(host, {launch->launcher, *launch->launch_environment, {}, launch->working_directory});
    launch->account_context = addon::Account_context{2, account.adapter_account_key, account.profile_path};
    const auto payload = success(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(*launch));
    auto patch = addon::decode_launch_patch(payload, &failure);
    check(patch && patch->disposition->prepared && patch->argv &&
        addon::validate_launch_patch(*patch, launch->declared_assets, launch->declared_assets, &failure) &&
        !addon::account_context_acknowledgment_failure(*launch->account_context, payload),
        "SDK admits the selected-account interactive proposal before native execution");
    const std::vector<std::pair<std::string, std::string>> assets = {
        {"assets/logonomic-pi.mjs", addon::path_to_utf8(package / "assets" / "logonomic-pi.mjs")},
    };
    const auto materialize = [&](std::string& value) {
        const auto tokens = addon::scan_asset_tokens(value, &failure);
        check(tokens.has_value(), "SDK admits the proposed asset tokens");
        const auto expanded = addon::substitute_asset_tokens(value, *tokens, assets);
        check(expanded.has_value(), "SDK materializes the package-owned extension");
        value = *expanded;
    };
    for (auto& argument : *patch->argv) materialize(argument);
    for (auto& item : patch->set) materialize(item.second);

    addon::Resume_prepare_request resume;
    resume.context.launcher = launch->launcher;
    resume.context.launcher_argv = launch->launcher_argv;
    resume.context.working_directory = launch->working_directory;
    resume.context.declared_assets = launch->declared_assets;
    resume.context.account_context = launch->account_context;
    resume.context.launch_environment = launch->launch_environment;
    resume.resume_record = json::member(*fixture, "resume_record");
    const auto resumed = success(host, {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume));
    const auto proposal = addon::decode_resume_prepare_result(resumed, &failure);
    check(proposal && addon::validate_resume_process_proposal(proposal->process, &failure) &&
        !addon::account_context_acknowledgment_failure(*launch->account_context, resumed),
        "SDK admits the selected-account resume bridge proposal before native execution");
    std::ofstream output(output_file, std::ios::binary);
    output << addon::serialize_json(json::object({
        {"interactive", addon::encode_launch_patch(*patch)}, {"resume", resumed},
        {"selected_profile", Json(*account.profile_path)},
    }));
    check(output.good(), "actual prepared proposals are written for native handoff qualification");
}

} // namespace

int main(int argc, char* argv[])
{
    const bool native = argc == 3 && std::string_view(argv[1]) == "--native-paths";
    const bool handoff = argc == 5 && std::string_view(argv[1]) == "--native-handoff";
    const bool case_profiles = argc == 3 && std::string_view(argv[1]) == "--case-profiles";
    if (argc != 2 && !native && !handoff && !case_profiles) {
        std::cerr << "Usage: pi_profile_tests <package> | --case-profiles <package> | --native-paths <native-case-file> | "
            "--native-handoff <package> <request-file> <output-file>\n";
        return 2;
    }
    try {
        if (native) {
            native_paths(addon::path_from_utf8(argv[2]));
        }
        else {
            Fixture_root fixture;
            if (!handoff && !case_profiles) path_contract(fixture.path);
            const auto bundle = fixture.path / "bundle";
            fs::create_directory(bundle);
#ifdef _WIN32
            const auto executable = bundle / "pi.exe";
#else
            const auto executable = bundle / "pi";
#endif
            fs::copy_file(fs::absolute(addon::path_from_utf8(argv[0])), executable);
            std::ofstream(bundle / "package.json") << R"({"name":"@earendil-works/pi-coding-agent","version":"1.5.2","bin":{"pi":"dist/bundle/cli.js"}})";
            addon::Simulator_options options;
            options.package_directory = addon::path_from_utf8(argv[handoff || case_profiles ? 2 : 1]);
            options.scratch_directory = fixture.path / "host";
            addon::Host_simulator host(std::move(options), nullptr);
            addon::Contract_failure failure;
            check(host.start(&failure), "SDK host starts the packaged profile provider: " + failure.reason_code + " " + failure.detail);
            bool skipped = false;
            if (case_profiles) {
#ifdef _WIN32
                skipped = !case_sensitive_accounts(host, fixture.path, executable);
#else
                skipped = true;
#endif
            }
            else if (handoff) {
                native_handoff(host, addon::path_from_utf8(argv[2]), addon::path_from_utf8(argv[3]), addon::path_from_utf8(argv[4]));
            }
            else {
                accounts_contract(host, fixture.path, executable);
                scoped_operations(host, fixture.path, executable);
            }
            int code = -1;
            check(host.shutdown(2000, &code) && code == 0, "packaged provider shuts down cleanly");
            if (skipped) return 77;
        }
        std::cout << "Passed " << assertions << " Pi native-profile and SDK account assertions.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "Pi profile contract failed: " << error.what() << '\n';
        return 1;
    }
}
