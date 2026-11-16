#include "pi_sessions.h"

#include "pi_preparation.h"
#include "pi_profile.h"

#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;
namespace json  = cli_addon::json;
namespace fs    = std::filesystem;
using Json = addon::Json_value;
using Block_kind = addon::Conversation_block_kind;
using Message = addon::Conversation_message;
using Phase = addon::Conversation_message_phase;

bool fail(addon::Contract_failure* failure, std::string reason, std::string detail)
{
    if (failure) {
        *failure = {std::move(reason), std::move(detail)};
    }
    return false;
}

bool unsupported(addon::Contract_failure* failure)
{
    return fail(failure, "conversation_content_unavailable", "Pi session content uses an unsupported native shape.");
}

bool text_member(const Json& value, std::string_view name)
{
    return json::member(value, name).is_string();
}

void append_block(Message& message, addon::Conversation_block block, std::vector<Phase>* phases,
    Phase phase = Phase::UNKNOWN)
{
    message.content.push_back(std::move(block));
    if (phases) {
        phases->push_back(phase);
    }
}

Phase text_phase(const Json& part)
{
    const auto signature = json::text(part, "textSignature");
    if (!signature.starts_with('{')) {
        return Phase::UNKNOWN;
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = signature.size();
    limits.max_string_bytes = signature.size();
    limits.max_members = signature.size();
    const auto value = addon::parse_json(signature, limits, nullptr);
    if (!value || !json::member(*value, "v").is_number() || json::member(*value, "v").as_number() != 1 ||
        !text_member(*value, "id"))
    {
        return Phase::UNKNOWN;
    }
    const auto phase = json::text(*value, "phase");
    return phase == "commentary" ? Phase::COMMENTARY : phase == "final_answer" ? Phase::FINAL : Phase::UNKNOWN;
}

bool append_content(const Json& content, Message& message, Block_kind kind, bool compact_visible,
    addon::Contract_failure* failure, std::vector<Phase>* phases)
{
    if (content.is_string()) {
        append_block(message, {kind, content.as_string(), {}, compact_visible}, phases);
        return true;
    }
    if (!content.is_array()) {
        return unsupported(failure);
    }
    bool redacted = false;
    for (const auto& part : content.as_array()) {
        const auto type = json::text(part, "type");
        if (type == "text" && text_member(part, "text")) {
            // Pi's TextSignatureV1 describes each native response item.
            // stopReason describes the whole response, which may mix phases.
            const auto phase = message.role == "assistant" ? text_phase(part) : Phase::UNKNOWN;
            append_block(message, {kind, json::text(part, "text"), {}, compact_visible && phase != Phase::COMMENTARY},
                phases, phase);
        }
        else
        if (type == "thinking" && message.role == "assistant" && text_member(part, "thinking")) {
            const auto& hidden = json::member(part, "redacted");
            if (!hidden.is_null() && !hidden.is_boolean()) {
                return unsupported(failure);
            }
            if (hidden.is_boolean() && hidden.as_boolean()) {
                if (!redacted) {
                    append_block(message, {Block_kind::TEXT,
                        "[Redacted thinking content; encrypted payload not exported.]", {}, true}, phases);
                    redacted = true;
                }
            }
            else {
                append_block(message, {Block_kind::TEXT, json::text(part, "thinking"), "Thinking", false}, phases);
            }
        }
        else
        if (type == "toolCall" && message.role == "assistant" && text_member(part, "name") && text_member(part, "id") &&
            json::member(part, "arguments").is_object())
        {
            std::string text = addon::serialize_json(json::member(part, "arguments"));
            if (const auto* error = part.find("argumentsError"); error && error->is_string()) {
                text += "\nArgument error: " + error->as_string();
            }
            append_block(message, {Block_kind::TOOL_CALL, std::move(text),
                json::text(part, "name") + " " + json::text(part, "id"), false}, phases);
        }
        else
        if (type == "image" && message.role != "assistant" && message.role != "system" &&
            text_member(part, "data") && text_member(part, "mimeType"))
        {
            append_block(message, {Block_kind::ATTACHMENT,
                "[Image attachment: " + json::text(part, "mimeType") + "; binary data not exported.]",
                "Image", compact_visible}, phases);
        }
        else {
            return unsupported(failure);
        }
    }
    return true;
}

bool normalize_message(const Json& native, Message& message, addon::Contract_failure* failure,
    std::vector<Phase>* phases)
{
    const auto role = json::text(native, "role");
    message.role = role == "hookMessage" ? "custom" : role;
    if ((role == "assistant" || role == "toolResult") && !json::member(native, "content").is_array()) {
        return unsupported(failure);
    }
    if (role == "user" || role == "assistant" || role == "system" || role == "custom" ||
        role == "hookMessage" || role == "toolResult")
    {
        const bool visible = role == "user" || role == "assistant";
        const auto kind = role == "toolResult" ? Block_kind::TOOL_RESULT : Block_kind::TEXT;
        if (!append_content(json::member(native, "content"), message, kind, visible, failure, phases)) {
            return false;
        }
        if (role == "toolResult") {
            message.role = "tool";
            for (auto& block : message.content) {
                block.label = json::text(native, "toolName") + " " + json::text(native, "toolCallId");
            }
        }
        if (role == "assistant" && !json::text(native, "errorMessage").empty()) {
            append_block(message, {Block_kind::TEXT, json::text(native, "errorMessage"), "Error", false}, phases);
        }
        return true;
    }
    if (role == "bashExecution" && text_member(native, "command") && text_member(native, "output")) {
        message.role = "tool";
        append_block(message, {Block_kind::TOOL_CALL, json::text(native, "command"), "bash", false}, phases);
        std::string output = json::text(native, "output");
        if (const auto* exit = native.find("exitCode"); exit && exit->is_integer()) {
            output += "\n[Exit code: " + std::to_string(exit->as_integer()) + "]";
        }
        if (const auto* cancelled = native.find("cancelled"); cancelled && cancelled->is_boolean() &&
            cancelled->as_boolean())
        {
            output += "\n[Command cancelled.]";
        }
        if (const auto* truncated = native.find("truncated"); truncated && truncated->is_boolean() &&
            truncated->as_boolean())
        {
            output += "\n[Output truncated; referenced output file not read.]";
        }
        append_block(message, {Block_kind::TOOL_RESULT, std::move(output), "bash", false}, phases);
        return true;
    }
    if ((role == "branchSummary" || role == "compactionSummary") && text_member(native, "summary")) {
        message.role = "system";
        append_block(message, {Block_kind::TEXT, json::text(native, "summary"),
            role == "branchSummary" ? "Branch summary" : "Compaction summary", false}, phases);
        return true;
    }
    return unsupported(failure);
}

bool normalize_entry(const Json& entry, Message& message, addon::Contract_failure* failure,
    std::vector<Phase>* phases = nullptr)
{
    const auto type = json::text(entry, "type");
    if (type == "message") {
        return normalize_message(json::member(entry, "message"), message, failure, phases);
    }
    if (type == "custom_message") {
        if (!text_member(entry, "customType") || !json::member(entry, "display").is_boolean()) {
            return unsupported(failure);
        }
        message.role = "custom";
        return append_content(json::member(entry, "content"), message, Block_kind::TEXT, false, failure, phases);
    }
    if (type == "compaction" || type == "branch_summary") {
        if (!text_member(entry, "summary") ||
            (type == "compaction" && (!text_member(entry, "firstKeptEntryId") ||
                !json::member(entry, "tokensBefore").is_number() || json::member(entry, "tokensBefore").as_number() < 0)) ||
            (type == "branch_summary" && !text_member(entry, "fromId")))
        {
            return unsupported(failure);
        }
        message.role = "system";
        append_block(message, {Block_kind::TEXT, json::text(entry, "summary"),
            type == "compaction" ? "Compaction summary" : "Branch summary", false}, phases);
        return true;
    }
    // These records change model state or model-context projection, not the
    // original participant transcript. Custom data is extension state only.
    bool valid = false;
    if (type == "model_change") {
        valid = text_member(entry, "provider") && text_member(entry, "modelId");
    }
    else
    if (type == "thinking_level_change") {
        valid = text_member(entry, "thinkingLevel");
    }
    else
    if (type == "usage") {
        valid = text_member(entry, "kind") && text_member(entry, "provider") && text_member(entry, "model") &&
            json::member(entry, "usage").is_object();
    }
    else
    if (type == "custom") {
        valid = text_member(entry, "customType");
    }
    else
    if (type == "context_edit") {
        const auto* replacement = entry.find("replacement");
        valid = text_member(entry, "targetId") && replacement &&
            (replacement->is_null() || (replacement->is_object() &&
                (json::member(*replacement, "content").is_string() || json::member(*replacement, "content").is_array())));
    }
    else
    if (type == "label") {
        valid = text_member(entry, "targetId") && (!entry.find("label") || text_member(entry, "label"));
    }
    else
    if (type == "session_info") {
        valid = !entry.find("name") || text_member(entry, "name");
    }
    return valid || unsupported(failure);
}

struct Session_source
{
    std::string id;
    fs::path path;
};

std::optional<Session_source> session_source(const Json& record, addon::Contract_failure* failure)
{
    if (!json::exact_keys(record, {"provider_session_id", "session_file"}) ||
        !json::bounded_string(json::member(record, "provider_session_id"), 256) ||
        !json::bounded_string(json::member(record, "session_file"), 4096))
    {
        fail(failure, "resume_record_invalid", "Pi needs the native session identity and exact session file.");
        return std::nullopt;
    }
    Session_source source{json::text(record, "provider_session_id"),
        addon::path_from_utf8(json::text(record, "session_file"))};
    if (!source.path.is_absolute()) {
        fail(failure, "resume_record_invalid", "Pi session file is not an absolute native path.");
        return std::nullopt;
    }
    return source;
}

addon::Resume_content content_state(const Native_session& session)
{
    bool context_edited = false;
    // Inspection covers the exact retained native conversation. A metadata-only
    // selected branch cannot erase content still present on another branch.
    for (const auto& entry : session.entries) {
        const auto type = json::text(entry, "type");
        if (type == "context_edit") {
            // Export retains the original transcript; proving it empty does
            // not prove that an edited native model-context replay is empty.
            context_edited = true;
        }
        if (type == "message" && json::text(json::member(entry, "message"), "role") == "system") {
            continue;
        }
        Message message;
        if (!normalize_entry(entry, message, nullptr)) {
            return addon::Resume_content::UNKNOWN;
        }
        if (std::any_of(message.content.begin(), message.content.end(), [](const auto& block) {
                return !block.text.empty();
            }))
        {
            return addon::Resume_content::NONEMPTY;
        }
    }
    return context_edited ? addon::Resume_content::UNKNOWN : addon::Resume_content::EMPTY;
}

std::vector<std::string> resume_arguments(const Launcher& launcher, const fs::path& path)
{
    std::vector<std::string> words(launcher.argv.begin(), launcher.argv.begin() + launcher.argument_prefix_size);
    constexpr std::string_view selectors[] = {"--continue", "-c", "--resume", "-r", "--no-session"};
    constexpr std::string_view selector_values[] = {"--session", "--session-id", "--fork", "--name", "-n"};
    constexpr std::string_view values[] = {
        "--provider", "--model", "--api-key", "--system-prompt", "--append-system-prompt", "--session-dir",
        "--models", "--tools", "-t", "--exclude-tools", "-xt", "--thinking", "--extension", "-e",
        "--skill", "--prompt-template", "--theme", "--use-theme", "--tui-mode",
    };
    const auto arguments = std::span(launcher.argv).subspan(launcher.argument_prefix_size);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--") {
            break;
        }
        if (std::find(std::begin(selectors), std::end(selectors), argument) != std::end(selectors)) {
            continue;
        }
        if (std::find(std::begin(selector_values), std::end(selector_values), argument) != std::end(selector_values)) {
            ++index;
            continue;
        }
        if (!argument.starts_with('-')) {
            continue; // Original positional prompts and @files must not be submitted again.
        }
        words.push_back(argument);
        const bool known_value = std::find(std::begin(values), std::end(values), argument) != std::end(values);
        // Pi's unknown long flags belong to extensions. Preserve their native
        // lexical value rule, including --flag=value, without guessing types.
        const bool extension_value = argument.starts_with("--") && argument.find('=') == std::string::npos &&
            index + 1 < arguments.size() && !arguments[index + 1].starts_with('-') &&
            !arguments[index + 1].starts_with('@');
        constexpr std::string_view switches[] = {
            "--no-tools", "--no-builtin-tools", "--no-mcp", "--no-skills", "--no-prompt-templates",
            "--no-themes", "--no-context-files", "--verbose", "--approve", "--no-approve", "--offline",
        };
        const bool known_switch = std::find(std::begin(switches), std::end(switches), argument) != std::end(switches);
        if (known_value || (extension_value && !known_switch)) {
            words.push_back(arguments[++index]);
        }
    }
    words.insert(words.end(), {"--session", addon::path_to_utf8(path)});
    return words;
}

} // namespace

std::optional<Native_session> read_native_session(
    const std::filesystem::path& path, std::string_view expected_id,
    const std::function<bool()>& cancelled, cli_addon::Contract_failure* failure)
{
    std::error_code error;
    const auto status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) {
        fail(failure, "conversation_not_found", "The exact Pi session file is absent.");
        return std::nullopt;
    }
    if (error || !fs::is_regular_file(status)) {
        fail(failure, "conversation_source_unavailable", "The exact Pi session file is inaccessible.");
        return std::nullopt;
    }
    Native_session session;
    std::unordered_map<std::string, std::size_t> indices;
    std::vector<std::optional<std::size_t>> parents;
    bool header_seen = false;
    const auto snapshot = addon::read_conversation_jsonl(path, [&](const Json& entry, addon::Contract_failure* detail) {
        if (!header_seen) {
            const auto& version = json::member(entry, "version");
            if (json::text(entry, "type") != "session" || json::text(entry, "id") != expected_id ||
                !json::bounded_string(json::member(entry, "id"), 256) ||
                !json::bounded_string(json::member(entry, "cwd"), 32768) ||
                !json::bounded_string(json::member(entry, "timestamp"), 128))
            {
                return fail(detail, "conversation_source_invalid", "Pi session header does not match the exact native identity.");
            }
            // Versions 2 and 3 have the persistent parent tree. Version 1
            // requires Pi's write migration; newer storage semantics are unknown.
            if (!version.is_integer() || (version.as_integer() != 2 && version.as_integer() != 3)) {
                return fail(detail, "conversation_format_unavailable", "Pi session is not a supported native tree format.");
            }
            session.id = json::text(entry, "id");
            session.cwd = json::text(entry, "cwd");
            header_seen = true;
            return true;
        }
        const auto& id = json::member(entry, "id");
        const auto* parent = entry.find("parentId");
        if (!json::bounded_string(id, 256) || !parent ||
            (!parent->is_null() && !json::bounded_string(*parent, 256)) || indices.contains(id.as_string()) ||
            !json::bounded_string(json::member(entry, "timestamp"), 128))
        {
            return fail(detail, "conversation_source_invalid", "Pi session has invalid or duplicate tree identities.");
        }
        std::optional<std::size_t> parent_index;
        if (parent->is_string()) {
            const auto found = indices.find(parent->as_string());
            if (found == indices.end()) {
                return fail(detail, "conversation_branch_unavailable", "Pi session ancestry is incomplete.");
            }
            parent_index = found->second;
        }
        Message message;
        if (!normalize_entry(entry, message, detail)) {
            return false;
        }
        indices.emplace(id.as_string(), session.entries.size());
        parents.push_back(parent_index);
        session.entries.push_back(entry);
        return true;
    }, cancelled, failure);
    if (!snapshot) {
        return std::nullopt;
    }
    if (!header_seen) {
        fail(failure, "conversation_source_invalid", "Pi session has no complete native header.");
        return std::nullopt;
    }
    session.incomplete_tail_omitted = snapshot->incomplete_tail_omitted;
    if (!session.entries.empty()) {
        std::optional<std::size_t> next = session.entries.size() - 1;
        while (next) {
            session.branch.push_back(*next);
            next = parents[*next];
        }
        std::reverse(session.branch.begin(), session.branch.end());
    }
    return session;
}

cli_addon::Resume_inspect_result inspect_resume(
    const cli_addon::Resume_inspect_request& request, const std::function<bool()>& cancelled)
{
    addon::Resume_inspect_result result;
    result.request_digest_hex = addon::resume_inspect_request_digest_hex(request);
    addon::Contract_failure failure;
    if (!validate_account_profile(request.context.account_context, request.context.launch_environment,
            request.context.working_directory, &failure))
    {
        return result;
    }
    const auto source = session_source(request.resume_record, &failure);
    if (!source) {
        return result;
    }
    const auto session = read_native_session(source->path, source->id, cancelled, &failure);
    if (!session) {
        if (failure.reason_code == "conversation_not_found") {
            result.availability = addon::Resume_availability::ABSENT;
        }
        return result;
    }
    if (!session->incomplete_tail_omitted) {
        result.availability = addon::Resume_availability::AVAILABLE;
        result.content = content_state(*session);
    }
    return result;
}

std::optional<cli_addon::Resume_prepare_result> prepare_resume(
    const cli_addon::Resume_prepare_request& request, const std::filesystem::path& package_root,
    const std::function<bool()>& cancelled, cli_addon::Contract_failure* failure)
{
    if (!validate_account_profile(request.context.account_context, request.context.launch_environment,
            request.context.working_directory, failure))
    {
        return std::nullopt;
    }
    const auto source = session_source(request.resume_record, failure);
    if (!source) {
        return std::nullopt;
    }
    const auto session = read_native_session(source->path, source->id, cancelled, failure);
    if (!session) {
        return std::nullopt;
    }
    if (session->incomplete_tail_omitted) {
        fail(failure, "conversation_source_unavailable", "Pi session has an incomplete native record.");
        return std::nullopt;
    }
    addon::Prepare_request launch;
    launch.launcher = request.context.launcher;
    launch.launcher_argv = request.context.launcher_argv;
    launch.launch_environment = request.context.launch_environment;
    launch.working_directory = request.context.working_directory;
    launch.declared_assets = request.context.declared_assets;
    const auto launcher = resolve_interactive_launcher(launch);
    if (!launcher) {
        fail(failure, "launcher_program_unavailable", "Pi launcher does not provide the supported interactive contract.");
        return std::nullopt;
    }
    const auto extension = package_root / "assets" / "logonomic-pi.mjs";
    std::error_code error;
    if (!package_root.is_absolute() || !fs::is_regular_file(extension, error) || error) {
        fail(failure, "launch_patch_invalid", "The host-owned Pi extension asset is unavailable.");
        return std::nullopt;
    }
    addon::Resume_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/pi_launch_bridge.exe";
#else
    result.process.executable = "helpers/pi_launch_bridge";
#endif
    result.process.argv = resume_arguments(*launcher, source->path);
    result.process.argv.insert(result.process.argv.begin() + launcher->argument_prefix_size,
        {"--extension", addon::path_to_utf8(extension)});
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set = launcher->leading_environment;
    result.patch.profile_storage = request.context.profile_storage;
    result.patch.account_context = request.context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request.resume_record);
    return result;
}

std::optional<cli_addon::Conversation_export_result> export_conversation(
    const cli_addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    cli_addon::Contract_failure* failure)
{
    if (!validate_account_profile(request.account_context, request.launch_environment, request.working_directory, failure)) {
        return std::nullopt;
    }
    if (!request.resume_record) {
        fail(failure, "conversation_source_unavailable", "Pi export needs the source session's exact native file record.");
        return std::nullopt;
    }
    const auto source = session_source(*request.resume_record, failure);
    if (!source || source->id != request.provider_session_id) {
        fail(failure, "conversation_source_unavailable", "Pi export record does not identify the requested conversation.");
        return std::nullopt;
    }
    const auto session = read_native_session(source->path, source->id, cancelled, failure);
    if (!session) {
        return std::nullopt;
    }
    addon::Conversation_writer writer(request, cancelled);
    bool opened = false;
    const auto append = [&](const Message& message) {
        if (!opened) {
            if (!writer.open(failure)) {
                return false;
            }
            opened = true;
        }
        return writer.append(message, failure);
    };
    for (const auto index : session->branch) {
        Message message;
        std::vector<Phase> phases;
        if (!normalize_entry(session->entries[index], message, failure, &phases)) {
            return std::nullopt;
        }
        // The SDK phase belongs to a message, while Pi's precise phase belongs
        // to a text block. Emit adjacent equal-phase groups in native order.
        Message group;
        group.role = message.role;
        for (std::size_t offset = 0; offset < message.content.size(); ++offset) {
            auto& block = message.content[offset];
            if (block.text.empty() || (request.compact && !block.compact_visible)) {
                continue;
            }
            if (!group.content.empty() && group.phase != phases[offset]) {
                if (!append(group)) {
                    return std::nullopt;
                }
                group.content.clear();
            }
            group.phase = phases[offset];
            group.content.push_back(std::move(block));
        }
        if (!group.content.empty() && !append(group)) {
            return std::nullopt;
        }
    }
    if (!opened) {
        fail(failure, "conversation_empty", "Pi session has no retained content in the requested projection.");
        return std::nullopt;
    }
    return writer.finish(session->incomplete_tail_omitted, failure);
}

} // namespace logonomic::pi
