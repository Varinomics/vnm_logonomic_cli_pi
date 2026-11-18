#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/usage_scan.h"

#include <functional>
#include <map>
#include <string>

namespace logonomic::pi {

struct Native_profile;

struct Usage_receipt
{
    cli_addon::Usage_record record;
    std::string instant;
    std::string evidence_digest;
    bool atomic = false;
};

using Usage_receipts = std::map<std::string, Usage_receipt, std::less<>>;
using Usage_progress = std::function<bool()>;

Usage_receipts collect_native_usage(const cli_addon::Usage_scan_request& request,
    const Native_profile& profile, const Usage_progress& cancelled);

void handle_usage_scan(cli_addon::Invocation_context& context);
void handle_usage_session(cli_addon::Invocation_context& context);

} // namespace logonomic::pi
