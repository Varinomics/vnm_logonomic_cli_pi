#pragma once

#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/launcher_utilities.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::pi {

struct Launcher
{
    std::vector<std::string> argv;
    std::string package_name;
    std::string version;
    std::size_t argument_prefix_size = 0;
};

std::optional<Launcher> resolve_launcher(
    const cli_addon::Launcher_request& launcher,
    const cli_addon::Environment& environment,
    std::string_view platform,
    std::string_view working_directory,
    std::optional<std::span<const std::string>> host_argv = std::nullopt);

cli_addon::Json_value discover(const cli_addon::Discover_request& request);
cli_addon::Json_value recognize(const cli_addon::Recognize_request& request);

} // namespace logonomic::pi
