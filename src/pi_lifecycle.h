#pragma once

#include "logonomic_cli_addon/helper_ingress.h"
#include "logonomic_cli_addon/ids.h"

#include <optional>

namespace logonomic::pi {

std::optional<cli_addon::Classify_event_result> classify_event(
    const cli_addon::Classify_event_request& request,
    cli_addon::Contract_failure* failure);

} // namespace logonomic::pi
