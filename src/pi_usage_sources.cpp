#include "pi_usage.h"
#include "pi_launcher.h"
#include "pi_profile.h"

#include "logonomic_cli_addon/conversation_export.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/usage_temporal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace logonomic::pi {
namespace {
namespace addon = cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::uint64_t k_max_exact = 9007199254740991ULL;

void check(const Usage_progress& cancelled)
{
    if (cancelled()) throw std::runtime_error("cancelled");
}

fs::file_status native_status(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::status(native_io_path(path), error);
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
        throw std::runtime_error("pi_usage_source_unavailable");
    }
    return status;
}

std::optional<std::string> setting_directory(const fs::path& path)
{
    if (!fs::exists(native_status(path))) return std::nullopt;
    const auto file = native_io_path(path);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 4 * 1024 * 1024) {
        throw std::runtime_error("pi_usage_settings_unavailable");
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("pi_usage_settings_unavailable");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    addon::Json_parse_limits limits;
    limits.max_bytes = 4 * 1024 * 1024;
    limits.max_string_bytes = limits.max_bytes;
    auto settings = addon::parse_json(bytes, limits, nullptr);
    if (!settings || !settings->is_object()) throw std::runtime_error("pi_usage_settings_invalid");
    const auto* value = settings->find("sessionDir");
    if (!value) return std::nullopt;
    if (!value->is_string()) throw std::runtime_error("pi_usage_settings_invalid");
    return value->as_string();
}

std::set<fs::path> sources(const addon::Usage_scan_request& request, const Native_profile& profile,
    const Usage_progress& cancelled)
{
    const auto& context = request.context;
    addon::Contract_failure failure;
    const auto& environment = *context.launch_environment;
    auto launcher = resolve_frozen_launcher(context.launcher, environment, context.working_directory,
        std::span<const std::string>(context.launcher_argv));
    if (!launcher) throw std::runtime_error("pi_usage_launcher_unavailable");
    const auto resolve = [&](const std::string& path) {
        auto native = resolve_native_path(environment, context.working_directory, path, {}, &failure);
        if (!native) throw std::runtime_error(failure.reason_code);
        return *native;
    };
    std::optional<std::string> configured;
    std::optional<std::string> session, fork;
    const auto arguments = std::span(launcher->argv).subspan(launcher->argument_prefix_size);
    constexpr std::string_view valued_options[] = {
        "--provider", "--model", "--api-key", "--system-prompt", "--append-system-prompt",
        "--session-id", "--name", "-n", "--models", "--tools", "-t", "--exclude-tools", "-xt",
        "--thinking", "--extension", "-e", "--skill", "--prompt-template", "--theme",
        "--export",
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg == "--") break;
        if (arg == "--session-dir" || arg == "--session" || arg == "--fork") {
            if (++i == arguments.size()) throw std::runtime_error("pi_usage_launcher_unavailable");
            if (arg == "--session-dir") configured = arguments[i];
            else if (arg == "--session") session = arguments[i];
            else fork = arguments[i];
        }
        else if (std::find(std::begin(valued_options), std::end(valued_options), arg) != std::end(valued_options) &&
            i + 1 < arguments.size()) ++i;
    }
    if (!configured || configured->empty()) {
        const auto values = addon::environment_pairs(environment.effective_environment);
        configured = addon::environment_value(values, "PI_CODING_AGENT_SESSION_DIR");
        if (!configured || configured->empty()) {
            configured.reset();
            if (!context.working_directory.empty()) configured = setting_directory(
                addon::path_from_utf8(context.working_directory) / ".pi" / "settings.json");
            if (!configured) configured = setting_directory(profile.path / "settings.json");
        }
    }
    std::set<fs::path> result;
    const auto list = [&](const fs::path& directory) {
        if (!fs::exists(native_status(directory))) return;
        for (const auto& entry : fs::directory_iterator(native_io_path(directory))) {
            check(cancelled);
            if (entry.path().extension() == ".jsonl" && entry.is_regular_file()) {
                result.insert(directory / entry.path().filename());
            }
        }
    };
    const auto root = profile.path / "sessions";
    if (fs::exists(native_status(root))) {
        for (const auto& entry : fs::directory_iterator(native_io_path(root))) {
            check(cancelled);
            if (entry.is_directory()) list(root / entry.path().filename());
        }
    }
    if (configured && !configured->empty()) list(resolve(*configured));
    const auto& selected = fork ? fork : session;
    if (selected && (selected->ends_with(".jsonl") || selected->find_first_of("/\\") != std::string::npos)) {
        const auto path = resolve(*selected);
        if (fs::exists(native_status(path))) result.insert(path);
    }
    return result;
}

std::optional<std::uint64_t> count(const Json& usage, std::string_view key)
{
    const auto* value = usage.find(key);
    if (!value) return std::nullopt;
    if (!value->is_integer() || value->as_integer() < 0 ||
        static_cast<std::uint64_t>(value->as_integer()) > k_max_exact) throw std::runtime_error("pi_usage_counter_invalid");
    return static_cast<std::uint64_t>(value->as_integer());
}

addon::Usage_amount amount(const Json& usage, std::string_view key)
{
    const auto value = count(usage, key);
    return {true, value, addon::Usage_field_coverage{value ? 1ULL : 0ULL, 1, false,
        value ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN}};
}

struct Session
{
    std::string id;
    fs::path parent;
    std::set<std::string> events;
    bool complete = false;
};

void add_usage(const Json& entry, const Json& source, const Json& usage, const std::string& kind,
    Session& session, Usage_receipts& events)
{
    if (!usage.is_object()) throw std::runtime_error("pi_usage_counter_invalid");
    const auto id = json::text(entry, "id");
    const auto instant = json::text(entry, "timestamp");
    if (id.empty() || id.size() > 256 || instant.size() != 24 || !instant.ends_with("Z") ||
        !addon::parse_native_usage_timestamp(instant)) throw std::runtime_error("pi_usage_event_identity_invalid");
    const auto key = "pi-event:" + addon::to_hex(addon::Sha256::digest(addon::serialize_json(Json(Json::Array{Json(id), Json(instant)}))));

    Usage_receipt receipt;
    receipt.instant = instant;
    receipt.atomic = kind == "assistant" || (kind == "usage" && json::text(entry, "kind") == "cache_warm");
    auto& record = receipt.record;
    record.record_id = key;
    record.completed_date = instant.substr(0, 10);
    record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
    record.input = amount(usage, "input");
    record.output = amount(usage, "output");
    record.cache_read = amount(usage, "cacheRead");
    record.cache_creation = amount(usage, "cacheWrite");
    const auto total = count(usage, "totalTokens");
    if (total && *total) {
        record.total_tokens = *total;
        record.counter_field = "usage.totalTokens";
    }
    else {
        for (const auto* part : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
            if (!part->value) return;
            if (*part->value > k_max_exact - record.total_tokens) throw std::runtime_error("pi_usage_counter_invalid");
            record.total_tokens += *part->value;
        }
        record.counter_field = "usage.input+output+cacheRead+cacheWrite";
    }
    // Pi initializes usage to zero even when the provider supplied no measurements.
    // An empty provider usage object can produce the same journal as reported zero.
    if (!record.total_tokens) return;
    if (!session.events.insert(key).second) throw std::runtime_error("pi_usage_duplicate_entry");
    record.source_counter_semantics = "per_event_sum";
    record.aggregate_evidence = "complete";
    if (kind == "assistant" || kind == "usage") {
        const auto provider = json::text(source, "provider");
        const auto served = json::text(source, "responseModel");
        const auto model = served.empty() ? json::text(source, "model") : served;
        if (!provider.empty() && !model.empty() && provider.size() + model.size() + 1 <= 256) record.model = provider + "/" + model;
    }
    // Pi can rechain parentId and replace the firstKeptEntryId during a native fork.
    // Hash only the preserved receipt payload. No message text is saved in the cache.
    const auto signature = addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(
        Json(Json::Array{Json(kind), kind == "assistant" || kind == "toolResult" ? source : usage,
            Json(record.model.value_or(""))}))));
    receipt.evidence_digest = signature;
    const auto [existing, inserted] = events.try_emplace(key, std::move(receipt));
    if (!inserted && existing->second.evidence_digest != signature) throw std::runtime_error("pi_usage_event_conflict");
}

std::optional<Session> read_session(const fs::path& path, Usage_receipts& events, const Usage_progress& cancelled)
{
    Session session;
    bool unsupported = false;
    addon::Contract_failure failure;
    const auto snapshot = addon::read_conversation_jsonl(native_io_path(path), [&](const Json& entry, addon::Contract_failure*) {
        const auto type = json::text(entry, "type");
        if (session.id.empty()) {
            const auto& version = json::member(entry, "version");
            if (type != "session" || !version.is_integer() || (version.as_integer() != 2 && version.as_integer() != 3)) {
                // Custom storage can contain other JSONL files. Pi v1 also
                // predates stable receipt IDs and assigns random IDs on migration.
                unsupported = true;
                return false;
            }
            if (!json::bounded_string(json::member(entry, "id"), 256)) throw std::runtime_error("pi_usage_session_format_unavailable");
            session.id = json::text(entry, "id");
            if (const auto* parent = entry.find("parentSession"); parent && !parent->is_null()) {
                if (!parent->is_string() || parent->as_string().empty()) throw std::runtime_error("pi_usage_parent_invalid");
                session.parent = addon::path_from_utf8(parent->as_string()).lexically_normal();
            }
        }
        else if (type == "message") {
            const auto& msg = json::member(entry, "message");
            const auto role = json::text(msg, "role");
            const auto* usage = msg.find("usage");
            if (usage && (role == "assistant" || role == "toolResult")) add_usage(entry, msg, *usage, role, session, events);
        }
        else if (type == "usage" || type == "branch_summary" || type == "compaction") {
            const auto* usage = entry.find("usage");
            if (usage) add_usage(entry, entry, *usage, type, session, events);
        }
        else if (type == "session" || type.empty()) throw std::runtime_error("pi_usage_session_format_unavailable");
        return true;
    }, cancelled, &failure);
    if (unsupported) return std::nullopt;
    if (!snapshot) throw std::runtime_error(failure.reason_code);
    if (session.id.empty()) return std::nullopt;
    session.complete = !snapshot->incomplete_tail_omitted;
    return session;
}
} // namespace

Usage_receipts collect_native_usage(const cli_addon::Usage_scan_request& request, const Native_profile& profile,
    const Usage_progress& cancelled)
{
    Usage_receipts events;
    std::map<fs::path, Session> sessions;
    for (const auto& path : sources(request, profile, cancelled)) {
        check(cancelled);
        auto session = read_session(path, events, cancelled);
        if (session) sessions.emplace(path.lexically_normal(), std::move(*session));
    }
    // A fork includes copies of older receipts. Only claim the creating session
    // when its complete parent source is available within this scan, or it is a root.
    for (const auto& [path, session] : sessions) {
        const auto parent = sessions.find(session.parent);
        if (!session.parent.empty() && (parent == sessions.end() || !parent->second.complete)) continue;
        for (const auto& id : session.events) {
            check(cancelled);
            if (parent != sessions.end() && parent->second.events.contains(id)) continue;
            auto& record = events.at(id).record;
            if (record.session_key && *record.session_key != session.id) throw std::runtime_error("pi_usage_event_owner_conflict");
            if (!record.session_key) {
                record.session_key = session.id;
                record.source_path = addon::path_to_utf8(path);
            }
        }
    }
    return events;
}
} // namespace logonomic::pi
