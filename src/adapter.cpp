#include "pi_launcher.h"
#include "pi_lifecycle.h"
#include "pi_preparation.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/protocol.h"

#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    addon::Contract_failure failure;
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
    if (request.capability.capability_id == "launch-preparation" && request.capability.version == 3 &&
        request.operation == addon::k_prepare_operation)
    {
        const auto input = addon::decode_prepare_request(request.payload, &failure, 3);
        if (input) {
            context.succeed(addon::encode_launch_patch(logonomic::pi::prepare(*input), 3));
            return;
        }
    }
    else
    if (request.capability.capability_id == "helper-ingress" && request.capability.version == 2 &&
        request.operation == addon::k_classify_event_operation)
    {
        const auto input = addon::decode_classify_event_request(request.payload, &failure);
        if (input) {
            const auto result = logonomic::pi::classify_event(*input, &failure);
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
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
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
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
