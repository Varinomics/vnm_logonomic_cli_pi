#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/adapter_runtime.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace logonomic::pi {

struct Native_profile
{
    std::filesystem::path path;
    std::string account_key;
};

// Node uses namespaced Windows paths for filesystem operations, preserving
// components ending in spaces or dots. Keep native display and identity paths.
std::filesystem::path native_io_path(const std::filesystem::path& absolute_path);

// Pi's getAgentDir/normalizePath policy, using only the host's frozen input.
// default_home_fallback is available to account discovery, not later sessions.
std::optional<Native_profile> resolve_native_profile(
    const cli_addon::Launch_environment& environment,
    std::string_view working_directory,
    std::string_view default_home_fallback,
    cli_addon::Contract_failure* failure);

bool validate_account_profile(
    const std::optional<cli_addon::Account_context>& account,
    const std::optional<cli_addon::Launch_environment>& environment,
    std::string_view working_directory,
    cli_addon::Contract_failure* failure);

void handle_accounts(cli_addon::Invocation_context& context);

} // namespace logonomic::pi
