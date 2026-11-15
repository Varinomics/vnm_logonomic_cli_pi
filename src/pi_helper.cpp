#include "logonomic_cli_addon/helper_client.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;
namespace json  = logonomic::cli_addon::json;

constexpr std::size_t k_max_input_bytes = 256 * 1024;

bool input_closed() noexcept
{
    // The extension retains this pipe after the request line. Closing it cancels
    // through the SDK so the wrapper is terminated and reaped before we exit.
#ifdef _WIN32
    DWORD available = 0;
    if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr)) {
        return true;
    }
    return available != 0;
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    return poll(&input, 1, 0) > 0 && input.revents != 0;
#endif
}

int output(addon::Json_value value, int code)
{
    std::cout << addon::serialize_json(value) << '\n';
    return code;
}

int failure(std::string_view reason, int code = 2)
{
    return output(json::object({
        {"ok",     addon::Json_value(false)},
        {"reason", addon::Json_value(reason)},
    }), code);
}

int event(addon::Helper_client& client, const addon::Json_value& input)
{
    if (!json::exact_keys(input, {"name", "body", "idempotency_key", "transition_nonce"}) ||
        !json::bounded_string(json::member(input, "name"), 256) ||
        !json::member(input, "body").is_object() ||
        !json::bounded_string(json::member(input, "idempotency_key"), 256) ||
        !addon::is_valid_transition_nonce_hex(json::text(input, "transition_nonce")))
    {
        return failure("invalid_pi_event");
    }
    const auto key = json::text(input, "idempotency_key");
    const auto outcome = client.deliver_provider_event(json::text(input, "name"),
        json::member(input, "body"), key, json::text(input, "transition_nonce"));
    const bool acknowledged = outcome.committed && client.acknowledge_provider_event(key) == 0;
    auto response = json::object({
        {"committed",    addon::Json_value(outcome.committed)},
        {"accepted",     addon::Json_value(outcome.accepted)},
        {"reason",       addon::Json_value(outcome.reason)},
        {"acknowledged", addon::Json_value(acknowledged)},
    });
    if (outcome.epoch) {
        response.set("epoch", addon::Json_value(*outcome.epoch));
    }
    return output(std::move(response), outcome.committed && outcome.accepted ? 0 : 1);
}

int title_instruction(addon::Helper_client& client, const addon::Json_value& input)
{
    if (!json::exact_keys(input, {"source", "hook_event_name", "session_id", "transition_nonce"}) ||
        json::text(input, "source") != "pi-extension" ||
        json::text(input, "hook_event_name") != "before_agent_start" ||
        !json::bounded_string(json::member(input, "session_id"), 256) ||
        !addon::is_valid_transition_nonce_hex(json::text(input, "transition_nonce")))
    {
        return failure("invalid_title_instruction");
    }
    addon::Model_context_delivery delivery;
    delivery.activation = addon::Model_context_activation{
        json::text(input, "session_id"), json::text(input, "transition_nonce")};
    const auto result = client.request_title_instruction("pi-extension", "before_agent_start", false, delivery);
    return output(json::object({
        {"completed",            addon::Json_value(result.completed)},
        {"accepted",             addon::Json_value(result.accepted)},
        {"instruction_required", addon::Json_value(result.instruction_required)},
        {"instruction",          addon::Json_value(result.instruction)},
        {"reason",               addon::Json_value(result.reason)},
    }), result.completed ? 0 : 1);
}

int control(addon::Helper_client& client, std::string_view operation, const addon::Json_value& input)
{
    if (operation == "title-instruction") {
        return title_instruction(client, input);
    }
    int code = 2;
    if (operation == "set-title" && json::exact_keys(input, {"title"}) &&
        json::bounded_string(json::member(input, "title"), 4096))
    {
        code = client.set_title(json::text(input, "title"));
    }
    else
    if (operation == "show-changed-files" && json::exact_keys(input, {"paths"})) {
        const auto& values = json::member(input, "paths");
        if (!values.is_array() || values.as_array().empty() || values.as_array().size() > 512) {
            return failure("invalid_changed_files");
        }
        std::vector<std::string> paths;
        paths.reserve(values.as_array().size());
        for (const auto& value : values.as_array()) {
            if (!json::bounded_string(value, 4096)) {
                return failure("invalid_changed_files");
            }
            paths.push_back(value.as_string());
        }
        code = client.show_changed_files(paths);
    }
    else {
        return failure("invalid_control_request");
    }
    return code == 0
        ? output(json::object({{"ok", addon::Json_value(true)}}), 0)
        : failure("logonomic_control_failed", code);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) {
        return failure("invalid_helper_operation");
    }
    std::string bytes;
    for (char character = 0; std::cin.get(character);) {
        if (character == '\n') {
            break;
        }
        bytes.push_back(character);
        if (bytes.size() > k_max_input_bytes) {
            return failure("helper_input_too_large");
        }
    }
    if (!std::cin) {
        return failure("invalid_helper_input");
    }
    const auto input = addon::parse_json(bytes, {}, nullptr);
    if (!input || !input->is_object()) {
        return failure("invalid_helper_input");
    }
    addon::Contract_failure error;
    auto environment = addon::read_helper_environment(&error);
    if (!environment) {
        return failure(error.reason_code);
    }
    if (environment->package_id != "logonomic.cli.pi") {
        return failure("wrong_provider_package");
    }
    addon::Helper_client client(std::move(*environment), input_closed);
    if (argc == 2 && std::string_view(argv[1]) == "event") {
        return event(client, *input);
    }
    if (argc == 3 && std::string_view(argv[1]) == "control") {
        return control(client, argv[2], *input);
    }
    return failure("invalid_helper_operation");
}
