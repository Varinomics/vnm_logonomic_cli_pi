#include "pi_structured_tasks.h"

#include "pi_launcher.h"
#include "pi_preparation.h"
#include "pi_profile.h"
#include "pi_sessions.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;
namespace json  = cli_addon::json;
using Json = addon::Json_value;

bool fresh_arguments(std::span<const std::string> arguments)
{
    constexpr std::string_view switches[] = {
        "--no-session", "--no-tools", "-nt", "--no-builtin-tools", "-nbt", "--no-extensions", "-ne",
        "--no-mcp", "--no-skills", "-ns", "--no-prompt-templates", "-np", "--no-themes",
        "--no-context-files", "-nc", "--approve", "-a", "--no-approve", "-na", "--offline", "--verbose",
    };
    constexpr std::string_view parameters[] = {
        "--provider", "--model", "--api-key", "--system-prompt", "--append-system-prompt", "--name", "-n",
        "--models", "--session-dir", "--tools", "-t", "--exclude-tools", "-xt", "--thinking",
        "--extension", "-e", "--skill", "--prompt-template", "--theme", "--use-theme", "--tui-mode",
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (std::find(std::begin(switches), std::end(switches), argument) != std::end(switches)) {
            continue;
        }
        if (std::find(std::begin(parameters), std::end(parameters), argument) == std::end(parameters) ||
            ++index == arguments.size())
        {
            return false;
        }
        if (argument == "--thinking") {
            constexpr std::string_view levels[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
            if (std::find(std::begin(levels), std::end(levels), arguments[index]) == std::end(levels)) {
                return false;
            }
        }
    }
    return true;
}

enum class Phase { STARTING, CHECKING_COMMANDS, SUBMITTED, RUNNING, CHECKING, DRAINING };

struct Task
{
    Phase       phase = Phase::STARTING;
    std::string prompt;
    std::string session;
    std::string partial_line;
    std::string answer;
    bool        saw_user = false;
    std::string stop_reason;
};

std::string save(const Task& state)
{
    return addon::serialize_json(json::object({
        {"phase",   Json((std::int64_t)state.phase)},
        {"prompt",  Json(state.prompt)},
        {"session", Json(state.session)},
        {"tail",    Json(addon::encode_base64(state.partial_line))},
        {"answer",  Json(state.answer)},
        {"user",    Json(state.saw_user)},
        {"stop",    Json(state.stop_reason)},
    }));
}

std::optional<Task> restore(std::string_view encoded)
{
    const auto value = addon::parse_exchange_protocol_state(encoded);
    if (!value || !json::exact_keys(*value, {"phase", "prompt", "session", "tail", "answer", "user", "stop"})) {
        return std::nullopt;
    }
    const auto& phase = json::member(*value, "phase");
    if (!phase.is_integer() || phase.as_integer() < 0 || phase.as_integer() > (std::int64_t)Phase::DRAINING ||
        !json::bounded_string(json::member(*value, "prompt"), addon::limits::k_max_structured_prompt_bytes) ||
        !json::bounded_string(json::member(*value, "session"), 256, true) ||
        !json::bounded_string(json::member(*value, "answer"), addon::limits::k_max_structured_stream_bytes, true) ||
        !json::member(*value, "user").is_boolean() ||
        !json::bounded_string(json::member(*value, "stop"), 32, true) || !json::member(*value, "tail").is_string())
    {
        return std::nullopt;
    }
    auto tail = addon::decode_base64(json::text(*value, "tail"));
    if (!tail) {
        return std::nullopt;
    }
    return Task{static_cast<Phase>(phase.as_integer()), json::text(*value, "prompt"), json::text(*value, "session"),
        std::move(*tail), json::text(*value, "answer"), json::member(*value, "user").as_boolean(),
        json::text(*value, "stop")};
}

std::string command(const char* type, const char* id)
{
    return addon::serialize_json(json::object({{"type", Json(type)}, {"id", Json(id)}})) + '\n';
}

bool configure_authoring(Launcher& launcher, const addon::Session_launch_context& context,
    addon::Invocation_context& invocation)
{
    const auto options = std::span(launcher.argv).subspan(launcher.argument_prefix_size);
    // A saved extension or tool selector can re-enable capabilities even with
    // discovery disabled. Keep model selection and compatible restrictions.
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i] == "--offline" || options[i] == "--no-session" || options[i] == "--no-tools" ||
            options[i] == "--no-extensions" || options[i] == "--no-mcp" || options[i] == "--no-skills" ||
            options[i] == "--no-prompt-templates" || options[i] == "--no-context-files") continue;
        if ((options[i] != "--provider" && options[i] != "--model" && options[i] != "--thinking") ||
            ++i >= options.size() || options[i].empty() || options[i].front() == '-')
        {
            return false;
        }
    }
    constexpr const char* restrictions[] = {
        "--no-tools", "--no-extensions", "--no-mcp", "--no-skills", "--no-prompt-templates",
        "--no-context-files", "--no-session",
    };
    std::vector<std::string> help(launcher.argv.begin(), launcher.argv.begin() + launcher.argument_prefix_size);
    help.insert(help.end(), std::begin(restrictions), std::end(restrictions));
    help.push_back("--help");
    const auto output = addon::probe(help, addon::environment_pairs(context.launch_environment->effective_environment),
        context.working_directory, 5000, invocation);
    if (!output || std::any_of(std::begin(restrictions), std::end(restrictions), [&](const char* flag) {
            return output->output.find(flag) == std::string::npos;
        }))
    {
        return false;
    }
    for (const char* flag : restrictions) {
        if (std::find(launcher.argv.begin(), launcher.argv.end(), flag) == launcher.argv.end()) {
            launcher.argv.push_back(flag);
        }
    }
    return true;
}

void prepare(addon::Invocation_context& invocation, const addon::Structured_prepare_request& request, bool authoring)
{
    const auto& context = request.context;
    addon::Contract_failure failure;
    if (!addon::is_valid_utf8(request.prompt) || request.prompt.empty() ||
        request.prompt.find('\0') != std::string::npos)
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "pi_prompt_unavailable");
        return;
    }
    if (!validate_account_profile(context.account_context, context.launch_environment, context.working_directory, &failure)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    const auto& environment = *context.launch_environment;
    auto launcher = resolve_frozen_launcher(context.launcher, environment, context.working_directory,
        std::span<const std::string>(context.launcher_argv));
    if (!launcher || !supports_settled_events(launcher->version) ||
        !fresh_arguments(std::span(launcher->argv).subspan(launcher->argument_prefix_size)))
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable");
        return;
    }
    if (authoring && (request.prompt.starts_with('/') || request.prompt.starts_with('!') ||
        !configure_authoring(*launcher, context, invocation))) {
        invocation.fail(invocation.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            invocation.cancel_requested() ? "cancelled" : "authoring_unavailable");
        return;
    }
    addon::Structured_prepare_result result;
    auto& process = result.process;
#ifdef _WIN32
    process.executable = "helpers/pi_launch_bridge.exe";
#else
    process.executable = "helpers/pi_launch_bridge";
#endif
    process.argv = std::move(launcher->argv);
    process.argv.insert(process.argv.end(), {"--mode", "rpc"});
    for (const auto& entry : environment.effective_environment.as_object()) {
        if (!addon::is_reserved_environment_name(entry.name)) {
            process.environment_names.push_back(entry.name);
        }
    }
    process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    process.stdin_payload = command("get_state", "initial");
    if (!addon::validate_structured_process_proposal(process, &failure)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    Task state;
    state.prompt = request.prompt;
    result.inspection      = {true, save(state), true};
    result.account_context = context.account_context;
    result.profile_storage = context.profile_storage;
    invocation.succeed(addon::encode_structured_prepare_result(result));
}

bool idle_session(const Json& data)
{
    return json::member(data, "isStreaming") == Json(false) && json::member(data, "isCompacting") == Json(false) &&
        json::member(data, "pendingMessageCount") == Json(0) && json::member(data, "messageCount").is_integer() &&
        json::bounded_string(json::member(data, "sessionId"), 256);
}

bool selects_command(std::string_view prompt, const Json& entry)
{
    const auto name = json::text(entry, "name");
    if (name.empty()) return true;
    prompt.remove_prefix(1);
    if (!prompt.starts_with(name)) return false;
    prompt.remove_prefix(name.size());
    if (prompt.empty() || prompt.front() == ' ') return true;
    if (json::text(entry, "source") != "prompt") return false;
    // Extensions and skills split on a literal space. Prompt templates use
    // JavaScript's Unicode whitespace, including the byte order mark.
    std::size_t width = 1;
    while (width < prompt.size() && (static_cast<unsigned char>(prompt[width]) & 0xc0) == 0x80) ++width;
    const auto first = prompt.substr(0, width);
    return first == "\xef\xbb\xbf" || addon::trim(first).empty();
}

std::string_view handle_response(Task& task, const Json& record, addon::Structured_inspect_result& result)
{
    const auto id      = json::text(record, "id");
    const auto type    = json::text(record, "command");
    const auto& data   = json::member(record, "data");
    if (json::member(record, "success") != Json(true)) {
        return "pi_command_failed";
    }
    if (task.phase == Phase::STARTING && id == "initial" && type == "get_state") {
        if (!idle_session(data) || json::member(data, "messageCount") != Json(0)) {
            return "pi_session_not_fresh";
        }
        task.session = json::text(data, "sessionId");
        if (task.prompt.starts_with('/')) {
            task.phase = Phase::CHECKING_COMMANDS;
            result.stdin_bytes = command("get_commands", "commands");
            return {};
        }
    }
    else if (task.phase == Phase::CHECKING_COMMANDS && id == "commands" && type == "get_commands") {
        const auto& commands = json::member(data, "commands");
        if (!commands.is_array()) return "pi_invalid_output";
        for (const auto& entry : commands.as_array()) {
            if (selects_command(task.prompt, entry)) return "pi_command_prompt";
        }
    }
    else if (task.phase == Phase::SUBMITTED && id == "task" && type == "prompt" &&
        json::text(data, "disposition") == "started")
    {
        task.phase = Phase::RUNNING;
        return {};
    }
    else if (task.phase == Phase::CHECKING && id == "finished" && type == "get_state" && idle_session(data) &&
        json::text(data, "sessionId") == task.session && json::member(data, "messageCount").as_integer() > 0)
    {
        task.phase = Phase::DRAINING;
        result.close_stdin = true;
        return {};
    }
    else {
        return "pi_unexpected_response";
    }
    task.phase = Phase::SUBMITTED;
    result.stdin_bytes = addon::serialize_json(json::object({
        {"id", Json("task")}, {"type", Json("prompt")}, {"message", Json(task.prompt)},
    })) + '\n';
    return {};
}

std::string_view read_answer(Task& task, const Json& record)
{
    const auto& messages = json::member(record, "messages");
    const auto& retry = json::member(record, "willRetry");
    if (!messages.is_array() || messages.as_array().empty() || !retry.is_boolean()) {
        return "pi_invalid_output";
    }
    task.stop_reason.clear();
    task.answer.clear();
    if (retry.as_boolean()) {
        return {};
    }
    const auto& last = messages.as_array().back();
    const auto& content = json::member(last, "content");
    if (json::text(last, "role") != "assistant" || !content.is_array()) {
        return "pi_incomplete_task";
    }
    // The final settled event decides cancellation. An unsuccessful attempt
    // may still be followed by a native retry or auto-compaction.
    task.stop_reason = json::text(last, "stopReason");
    if (task.stop_reason != "stop") {
        return {};
    }
    for (const auto& part : content.as_array()) {
        if (json::text(part, "type") == "text" && text_phase(part) != addon::Conversation_message_phase::COMMENTARY) {
            const auto& text = json::member(part, "text");
            if (!text.is_string() || text.as_string().find('\0') != std::string::npos) {
                return "pi_invalid_output";
            }
            if (text.as_string().size() > addon::limits::k_max_structured_stream_bytes - task.answer.size()) {
                return "pi_output_too_large";
            }
            task.answer += text.as_string();
        }
    }
    return {};
}

std::string_view consume(Task& task, const Json& record, addon::Structured_inspect_result& result, bool observe_model)
{
    const auto type = json::text(record, "type");
    if (type.empty()) {
        return "pi_invalid_output";
    }
    // The host cannot have delivered the next command until this inspection
    // returns. Bytes already in the pipe cannot acknowledge that command.
    if (result.stdin_bytes && (type == "response" || type == "agent_start" || type == "message_end" ||
        type == "agent_end" || type == "agent_settled"))
    {
        return "pi_unexpected_response";
    }
    if (type == "response") {
        return handle_response(task, record, result);
    }
    if (type == "extension_ui_request") {
        const auto method = json::text(record, "method");
        if (method == "notify" || method == "setStatus" || method == "setWidget" || method == "setTitle" ||
            method == "set_editor_text") return {};
        return "hook_approval_required";
    }
    if (type == "message_end") {
        if (task.phase != Phase::RUNNING) {
            return "pi_unexpected_message";
        }
        const auto& message = json::member(record, "message");
        const auto role = json::text(message, "role");
        if (role == "user") {
            const auto& content = json::member(message, "content");
            if (task.saw_user || !content.is_array() || content.as_array().size() != 1 ||
                json::text(content.as_array().front(), "type") != "text" ||
                json::text(content.as_array().front(), "text") != task.prompt)
            {
                return "pi_prompt_mismatch";
            }
            task.saw_user = true;
        }
        if (role == "assistant" && observe_model && task.saw_user) {
            const auto api  = json::text(message, "api");
            const auto stop = json::text(message, "stopReason");
            // Failed requests and unrouted virtual models carry selected
            // labels even when no physical model served a response.
            if (api.empty() || api == "pi-virtual" || (stop != "stop" && stop != "length" && stop != "toolUse")) {
                return {};
            }
            addon::Structured_model_observation sample{json::text(message, "provider"), json::text(message, "model"),
                "pi.message_end"};
            if (addon::is_valid_structured_model_observation(sample)) {
                result.model_observation = std::move(sample);
            }
        }
    }
    else if (type == "agent_start") {
        if (task.phase != Phase::RUNNING) {
            return "pi_unexpected_turn";
        }
        task.stop_reason.clear();
        task.answer.clear();
    }
    else if (type == "agent_end") {
        if (task.phase != Phase::RUNNING || !task.saw_user) {
            return "pi_unexpected_turn";
        }
        return read_answer(task, record);
    }
    else if (type == "agent_settled") {
        if (json::member(record, "aborted") == Json(true)) {
            return "pi_task_aborted";
        }
        if (task.stop_reason == "error") return "pi_model_error";
        if (task.stop_reason == "length") return "pi_output_truncated";
        if (task.phase != Phase::RUNNING || json::member(record, "aborted") != Json(false) ||
            !task.saw_user || task.stop_reason != "stop" || task.answer.empty())
        {
            return "pi_incomplete_task";
        }
        task.phase = Phase::CHECKING;
        result.stdin_bytes = command("get_state", "finished");
    }
    return {};
}

std::optional<Json> parse_record(std::string_view record)
{
    addon::Json_parse_limits limits;
    limits.max_members = record.size();
    limits.max_string_bytes = record.size();
    return addon::parse_json(record, limits, nullptr);
}

// Pi writes an agent_end as a single line and repeats every tool receipt
// and assistant message in its messages array. Only its last message can
// supply the result. Validate and discard completed earlier elements while
// they arrive so a many-tool turn need not fit inside the continuation.
std::optional<std::string> compact_turn(std::string_view line)
{
    constexpr std::string_view prefix = R"({"type":"agent_end","messages":[)";
    if (!line.starts_with(prefix) || line.size() < addon::limits::k_exchange_chunk_bytes) {
        return std::nullopt;
    }
    std::size_t first = prefix.size();
    while (first < line.size() && line[first] == '{') {
        unsigned    depth = 0;
        bool        quoted = false;
        std::size_t end = first;
        for (; end < line.size(); ++end) {
            const char byte = line[end];
            if (quoted && byte == '\\') {
                if (end + 1 < line.size()) ++end;
            }
            else if (byte == '"') quoted = !quoted;
            else if (!quoted && (byte == '{' || byte == '[')) ++depth;
            else if (!quoted && (byte == '}' || byte == ']') && --depth == 0) break;
        }
        // A following object proves this element is not the final message.
        if (end == line.size() || !line.substr(end + 1).starts_with(",{")) break;
        if (!parse_record(line.substr(first, end - first + 1))) break;
        first = end + 2;
    }
    if (first == prefix.size()) return std::nullopt;
    std::string compact(prefix);
    compact.append(line.substr(first));
    return compact;
}

addon::Structured_inspect_result inspect(const addon::Structured_inspect_request& request,
    const std::function<bool()>& cancelled)
{
    addon::Structured_inspect_result result;
    std::string_view reason;
    const auto& inspection = request.inspection;
    auto task = restore(inspection.protocol_state);
    if (!task) {
        reason = "pi_invalid_continuation";
    }
    else {
        std::string bytes;
        bytes.swap(task->partial_line);
        bytes += inspection.stdout_bytes;
        std::size_t first = 0;
        while (first < bytes.size() && reason.empty()) {
            if (cancelled()) {
                return {};
            }
            const auto end = bytes.find('\n', first);
            std::string_view line(bytes.data() + first, (end == bytes.npos ? bytes.size() : end) - first);
            const auto compact = compact_turn(line);
            if (compact) line = *compact;
            if (end == bytes.npos) {
                task->partial_line = line;
                break;
            }
            const auto record = parse_record(line);
            reason = record ? consume(*task, *record, result, request.model_state_version == 1) : "pi_invalid_output";
            first = end + 1;
        }
        if (inspection.timed_out) {
            reason = "pi_task_timed_out";
        }
        else if (inspection.final) {
            if (inspection.exit_code != 0 && reason.empty()) {
                reason = "pi_process_failed";
            }
            if (reason.empty() && (!task->partial_line.empty() || task->phase != Phase::DRAINING)) {
                reason = "pi_incomplete_task";
            }
            if (reason.empty()) {
                result.complete = true;
                result.stdin_bytes.reset();
                result.close_stdin = false;
                result.result_bytes = std::move(task->answer);
            }
        }
        else if (reason.empty()) {
            result.protocol_state = save(*task);
            if (!addon::exchange_protocol_state_valid(result.protocol_state) ||
                !addon::validate_session_exchange_payload(addon::encode_structured_inspect_result(result), nullptr))
            {
                reason = "pi_output_too_large";
            }
        }
    }
    if (!reason.empty()) {
        result.complete = true;
        result.protocol_state.clear();
        result.stdin_bytes.reset();
        result.close_stdin = false;
        result.failure_reason = std::string(reason);
    }
    return result;
}

} // namespace

void handle_structured_task(cli_addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto& invocation = context.request();
    const bool authoring = invocation.capability.capability_id == "authoring-session";
    const auto purpose = authoring ? addon::Structured_purpose::READ_ONLY_REVIEW : addon::Structured_purpose::IMPLEMENTATION;
    if (invocation.operation == addon::k_prepare_operation) {
        const auto request = addon::decode_structured_prepare_request(invocation.payload, &failure);
        if (request) {
            if (request->purpose != purpose) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            prepare(context, *request, authoring);
            return;
        }
    }
    else if (invocation.operation == addon::k_inspect_operation) {
        const auto request = addon::decode_structured_inspect_request(invocation.payload, &failure);
        if (request) {
            if (request->purpose != purpose) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            const auto result = inspect(*request, [&context] { return context.cancel_requested(); });
            if (context.cancel_requested()) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
            }
            else {
                context.succeed(addon::encode_structured_inspect_result(result));
            }
            return;
        }
    }
    else {
        context.reject_unknown_operation();
        return;
    }
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

} // namespace logonomic::pi
