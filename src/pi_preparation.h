#pragma once

#include "pi_launcher.h"

#include "logonomic_cli_addon/launch_preparation.h"

namespace logonomic::pi {

std::optional<Launcher> resolve_interactive_launcher(const cli_addon::Prepare_request& request);

std::optional<cli_addon::Launch_patch> prepare(
    const cli_addon::Prepare_request& request, cli_addon::Contract_failure* failure);

} // namespace logonomic::pi
