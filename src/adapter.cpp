#include "pi_launcher.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/protocol.h"

#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id != "launcher-discovery" || request.capability.version != 1) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
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
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "Pi for Logonomic";
    options.adapter_version = "1.0.0";
    options.capabilities    = {{"presentation", 1}, {"launcher-discovery", 1}};
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
