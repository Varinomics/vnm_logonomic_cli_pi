#include "pi_launcher.h"
#include "pi_lifecycle.h"
#include "pi_preparation.h"
#include "pi_profile.h"
#include "pi_sessions.h"
#include "pi_structured_tasks.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/profile_storage.h"

#include <filesystem>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    addon::Contract_failure failure;
    if (request.capability.capability_id == "structured-session" && request.capability.version == 3) {
        logonomic::pi::handle_structured_task(context);
        return;
    }
    if (request.capability.capability_id == "accounts" && request.capability.version == 2) {
        logonomic::pi::handle_accounts(context);
        return;
    }
    if (request.capability.capability_id == "profile-storage" && request.capability.version == 1) {
        if (request.operation == addon::k_profile_storage_acknowledge) {
            addon::acknowledge_profile_storage(context);
        }
        else {
            context.reject_unknown_operation();
        }
        return;
    }
    if (request.capability.capability_id == "launcher-discovery" && request.capability.version == 1) {
        if (request.operation == addon::k_discover_operation) {
            const auto input = addon::decode_discover_request(request.payload, &failure);
            if (input) {
                context.succeed(logonomic::pi::discover(*input));
                return;
            }
        }
        else
        if (request.operation == addon::k_recognize_operation) {
            const auto input = addon::decode_recognize_request(request.payload, &failure);
            if (input) {
                context.succeed(logonomic::pi::recognize(*input));
                return;
            }
        }
        else {
            context.reject_unknown_operation();
            return;
        }
    }
    else
    if (request.capability.capability_id == "interactive-resume") {
        if ((request.capability.version == 3 || request.capability.version == 4) &&
            request.operation == addon::k_inspect_resume_operation)
        {
            const auto input = addon::decode_resume_inspect_request(request.payload, &failure);
            if (input) {
                const auto result = logonomic::pi::inspect_resume(*input, [&context] { return context.cancel_requested(); });
                if (context.cancel_requested()) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                }
                else {
                    context.succeed(addon::encode_resume_inspect_result(result, request.capability.version));
                }
                return;
            }
        }
        else
        if (request.capability.version == 2 && request.operation == addon::k_prepare_resume_operation) {
            const auto input = addon::decode_resume_prepare_request(request.payload, &failure);
            if (input) {
                // The SDK process contract sets the adapter cwd to the
                // immutable published generation; resume argv is literal.
                std::error_code error;
                const auto root = std::filesystem::current_path(error);
                const auto result = logonomic::pi::prepare_resume(*input, root,
                    [&context] { return context.cancel_requested(); }, &failure);
                if (result) {
                    context.succeed(addon::encode_resume_prepare_result(*result));
                    return;
                }
            }
        }
        else {
            context.reject_unknown_operation();
            return;
        }
    }
    else
    if (request.capability.capability_id == "conversation-export" && request.capability.version == 1) {
        addon::handle_conversation_export(context, logonomic::pi::export_conversation);
        return;
    }
    else
    if (request.capability.capability_id == "launch-preparation" && request.capability.version == 3 &&
        request.operation == addon::k_prepare_operation)
    {
        const auto input = addon::decode_prepare_request(request.payload, &failure, 3);
        if (input) {
            const auto result = logonomic::pi::prepare(*input, &failure);
            if (result) {
                context.succeed(addon::encode_launch_patch(*result, 3));
                return;
            }
        }
    }
    else
    if (request.capability.capability_id == "helper-ingress" && request.capability.version == 2 &&
        request.operation == addon::k_classify_event_operation)
    {
        const auto input = addon::decode_classify_event_request(request.payload, &failure);
        if (input) {
            const auto result = logonomic::pi::classify_event(
                *input, [&context] { return context.cancel_requested(); }, &failure);
            if (result) {
                context.succeed(addon::encode_classify_event_result(*result));
                return;
            }
        }
    }
    else {
        context.reject_unknown_operation();
        return;
    }
    context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
        context.cancel_requested() ? "cancelled" : failure.reason_code, failure.detail);
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "Pi for Logonomic";
    options.adapter_version = "1.0.0";
    options.capabilities    = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {"managed-assets", 1}, {"helper-ingress", 2}, {"lifecycle-events", 2},
        {"model-context-delivery", 2}, {"coordination-delivery", 1},
        {"context-state", 2}, {"execution-context", 1}, {"model-state", 1}, {"model-catalog", 1},
        {"interactive-resume", 2}, {"interactive-resume", 3}, {"interactive-resume", 4},
        {"conversation-export", 1},
        {"profile-storage", 1}, {"accounts", 2},
        {"structured-session", 3}, {"structured-model-observation", 1},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
