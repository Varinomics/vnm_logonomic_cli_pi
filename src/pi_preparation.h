#pragma once

#include "pi_launcher.h"

#include "logonomic_cli_addon/launch_preparation.h"

namespace logonomic::pi {

std::optional<Launcher> resolve_interactive_launcher(const cli_addon::Prepare_request& request);

cli_addon::Launch_patch prepare(const cli_addon::Prepare_request& request);

} // namespace logonomic::pi
