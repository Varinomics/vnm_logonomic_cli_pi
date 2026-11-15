#include "pi_preparation.h"
#include "pi_launcher.h"

#include "logonomic_cli_addon/launcher_utilities.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;

constexpr std::string_view k_extension = "assets/logonomic-pi.mjs";

bool lifecycle_contract(std::string_view version)
{
    // Pi 1.1.0 added agent_settled.aborted, completing the lifecycle facts
    // this package reports. Compatible newer releases have no version ceiling.
    unsigned major = 0;
    unsigned minor = 0;
    const auto first = std::from_chars(version.data(), version.data() + version.size(), major);
    if (first.ec != std::errc{} || first.ptr == version.data() + version.size() || *first.ptr != '.') {
        return false;
    }
    const auto second = std::from_chars(first.ptr + 1, version.data() + version.size(), minor);
    if (second.ec != std::errc{} || second.ptr == version.data() + version.size() || *second.ptr != '.') {
        return false;
    }
    if (major > 1 || (major == 1 && minor > 1)) {
        return true;
    }
    if (major != 1 || minor != 1) {
        return false;
    }
    unsigned patch = 0;
    const auto third = std::from_chars(second.ptr + 1, version.data() + version.size(), patch);
    return third.ec == std::errc{} && (patch > 0 || third.ptr == version.data() + version.size() || *third.ptr != '-');
}

bool interactive_arguments(std::span<const std::string> arguments)
{
    constexpr std::string_view commands[] = {
        "auth", "mcp", "install", "uninstall", "remove", "update", "list", "config", "shell",
    };
    constexpr std::string_view excluded[] = {
        "--help", "-h", "--version", "-v", "--print", "-p", "--export",
        "--mode", "--list-models", "--no-extensions", "-ne",
    };
    constexpr std::string_view with_value[] = {
        "--provider", "--model", "--api-key", "--system-prompt", "--append-system-prompt",
        "--name", "-n", "--session", "--session-id", "--fork", "--session-dir", "--models",
        "--tools", "-t", "--exclude-tools", "-xt", "--thinking", "--extension", "-e",
        "--skill", "--prompt-template", "--theme", "--use-theme", "--tui-mode",
    };
    if (!arguments.empty() &&
        std::find(std::begin(commands), std::end(commands), arguments.front()) != std::end(commands))
    {
        return false;
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--") {
            break;
        }
        if (std::find(std::begin(excluded), std::end(excluded), argument) != std::end(excluded)) {
            return false;
        }
        if (std::find(std::begin(with_value), std::end(with_value), argument) != std::end(with_value)) {
            if (++index == arguments.size()) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

cli_addon::Launch_patch prepare(const cli_addon::Prepare_request& request)
{
    addon::Launch_patch patch;
    patch.disposition     = addon::Launch_disposition{false, false, false};
    patch.profile_storage = request.profile_storage;
    patch.account_context = request.account_context;
    const auto& environment = *request.launch_environment;
    const auto launcher = resolve_launcher(request.launcher,
        addon::environment_pairs(environment.effective_environment), environment.platform_id,
        request.working_directory, std::span<const std::string>(request.launcher_argv));
    if (!launcher || !lifecycle_contract(launcher->version) ||
        !interactive_arguments(std::span(launcher->argv).subspan(launcher->argument_prefix_size)))
    {
        return patch;
    }
    if (std::find(request.declared_assets.begin(), request.declared_assets.end(), k_extension) ==
        request.declared_assets.end())
    {
        return patch;
    }
    auto words = launcher->argv;
    words.insert(words.begin() + (std::ptrdiff_t)launcher->argument_prefix_size,
        {"--extension", "{{asset:assets/logonomic-pi.mjs}}"});
    patch.argv        = std::move(words);
    patch.disposition = addon::Launch_disposition{true, true, true};
    return patch;
}

} // namespace logonomic::pi
