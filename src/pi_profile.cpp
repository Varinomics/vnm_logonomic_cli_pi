#include "pi_profile.h"
#include "pi_launcher.h"

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;
namespace fs = std::filesystem;

bool fail(addon::Contract_failure* failure, std::string reason, std::string detail)
{
    if (failure) {
        *failure = {std::move(reason), std::move(detail)};
    }
    return false;
}

const std::string* environment_value(const addon::Launch_environment& environment, std::string_view name)
{
    for (const auto& item : environment.effective_environment.as_object()) {
#ifdef _WIN32
        const bool matches = addon::lower(item.name) == addon::lower(std::string(name));
#else
        const bool matches = item.name == name;
#endif
        if (matches && item.value.is_string()) {
            return &item.value.as_string();
        }
    }
    return nullptr;
}

std::optional<std::string> native_home(
    const addon::Launch_environment& environment, std::string_view fallback)
{
#ifdef _WIN32
    const auto* home = environment_value(environment, "USERPROFILE");
    // libuv's Windows homedir rejects an explicitly empty/invalid value.
    if (home && home->size() < 3) {
        return std::nullopt;
    }
#else
    const auto* home = environment_value(environment, "HOME");
#endif
    if (home) {
        return *home;
    }
    if (!fallback.empty()) {
        return std::string(fallback);
    }
    return std::nullopt;
}

int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> local_file_url(std::string_view input)
{
    // Admit the local file URLs produced by pathToFileURL. Other URL syntax
    // remains unavailable rather than being treated as a different directory.
    if (std::any_of(input.begin(), input.end(), [](unsigned char value) { return value <= 0x20; }) ||
        input.find_first_of("?#\\") != std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto slash = input.find('/', 7);
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto authority = addon::lower(std::string(input.substr(7, slash - 7)));
    if (!authority.empty() && authority != "localhost") {
        return std::nullopt;
    }
    std::string decoded;
    for (std::size_t index = slash; index < input.size(); ++index) {
        char value = input[index];
        if (value == '%') {
            if (index + 2 >= input.size()) return std::nullopt;
            const int high = hex_digit(input[index + 1]);
            const int low = hex_digit(input[index + 2]);
            if (high < 0 || low < 0) return std::nullopt;
            value = static_cast<char>((high << 4) | low);
            if (value == '/' || value == '\0') return std::nullopt;
#ifdef _WIN32
            if (value == '\\') return std::nullopt;
#endif
            index += 2;
        }
        decoded += value;
    }
#ifdef _WIN32
    if (decoded.size() < 4 || decoded[0] != '/' || decoded[2] != ':' || decoded[3] != '/' ||
        !((decoded[1] >= 'a' && decoded[1] <= 'z') || (decoded[1] >= 'A' && decoded[1] <= 'Z')))
    {
        return std::nullopt;
    }
    decoded.erase(0, 1);
#endif
    return decoded;
}

std::string windows_shell_path(std::string input)
{
#ifdef _WIN32
    // Native Pi accepts /c/, /mnt/c/ and /cygdrive/c/ shell drive paths.
    if (input.starts_with('/') && !input.starts_with("//") && input.find('\\') == std::string::npos) {
        std::string_view suffix(input);
        const auto prefix = addon::lower(input.substr(0, 10));
        suffix.remove_prefix(prefix.starts_with("/mnt/") ? 5 : prefix == "/cygdrive/" ? 10 : 1);
        if (!suffix.empty() && (suffix.size() == 1 || suffix[1] == '/') &&
            ((suffix[0] >= 'a' && suffix[0] <= 'z') || (suffix[0] >= 'A' && suffix[0] <= 'Z')))
        {
            const char drive = suffix[0] >= 'a' && suffix[0] <= 'z' ? suffix[0] - 'a' + 'A' : suffix[0];
            input = std::string(1, drive) + ":/" + std::string(suffix.size() > 1 ? suffix.substr(2) : "");
        }
    }
#endif
    return input;
}

std::string profile_key(std::string_view identity)
{
    return "profile-" + addon::to_hex(addon::Sha256::digest(identity));
}

bool matches_grant(const Native_profile& profile, const addon::Account_context& grant)
{
    if (grant.version == 1) return grant.adapter_account_key == profile.account_key;
    if (grant.version != 2 || !grant.profile_path) return false;
    const auto retained = addon::path_from_utf8(*grant.profile_path);
    if (grant.adapter_account_key == profile.account_key) {
        return addon::same_native_account_profile(retained, profile.path);
    }
    // A legacy folded key can describe two siblings. Only a verified original
    // path can disambiguate that historical grant.
    const auto previous = addon::historical_account_profile_identity(retained, profile.path);
    return previous && grant.adapter_account_key == profile_key(*previous);
}

addon::Account_status availability(const fs::path& path)
{
    std::error_code error;
    const auto io_path = native_io_path(path);
    const auto status = fs::status(io_path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
        (!error && status.type() == fs::file_type::not_found))
    {
        return addon::Account_status::UNAVAILABLE;
    }
    if (error || status.type() == fs::file_type::none || status.type() == fs::file_type::unknown) {
        return addon::Account_status::UNKNOWN;
    }
    if (!fs::is_directory(status)) {
        return addon::Account_status::UNAVAILABLE;
    }
    const fs::directory_iterator readable(io_path, error);
    return error ? addon::Account_status::UNKNOWN : addon::Account_status::AVAILABLE;
}

} // namespace

std::filesystem::path native_io_path(const std::filesystem::path& absolute_path)
{
#ifdef _WIN32
    const auto spelling = absolute_path.lexically_normal().make_preferred().native();
    if (spelling.starts_with(L"\\\\?\\") || spelling.starts_with(L"\\\\.\\")) {
        return spelling;
    }
    return spelling.starts_with(L"\\\\")
        ? L"\\\\?\\UNC\\" + spelling.substr(2)
        : L"\\\\?\\" + spelling;
#else
    return absolute_path;
#endif
}

std::optional<Native_profile> resolve_native_profile(
    const cli_addon::Launch_environment& environment, std::string_view working_directory,
    std::string_view default_home_fallback, cli_addon::Contract_failure* failure)
{
    if (environment.platform_id != addon::host_platform_id()) {
        fail(failure, "pi_profile_unavailable", "Pi profile needs the native platform's path semantics.");
        return std::nullopt;
    }
    const auto home = native_home(environment, default_home_fallback);
    const auto* override = environment_value(environment, "PI_CODING_AGENT_DIR");
    std::string selected;
    if (override && !override->empty()) {
        selected = windows_shell_path(*override);
        if (selected == "~" || selected.starts_with("~/")
#ifdef _WIN32
            || selected.starts_with("~\\")
#endif
        ) {
            if (!home) {
                fail(failure, "pi_profile_unavailable", "Pi profile expansion needs the host's frozen native home.");
                return std::nullopt;
            }
            selected = *home + (selected.size() == 1 ? "" : (home->empty() ? "" : "/") + selected.substr(2));
        }
        else if (selected.starts_with("file://")) {
            const auto path = local_file_url(selected);
            if (!path) {
                fail(failure, "pi_profile_unavailable", "Pi profile file URL cannot be interpreted exactly.");
                return std::nullopt;
            }
            selected = *path;
        }
    }
    else {
        if (!home) {
            fail(failure, "pi_profile_unavailable", "Pi default profile needs the host's frozen native home.");
            return std::nullopt;
        }
        selected = *home + (home->empty() ? "" : "/") + ".pi/agent";
    }
    auto path = addon::path_from_utf8(selected);
#ifdef _WIN32
    if (path.has_root_name() && !path.has_root_directory()) {
        fail(failure, "pi_profile_unavailable", "A drive-relative Pi profile has no frozen drive directory.");
        return std::nullopt;
    }
#endif
    if (!path.is_absolute()) {
        const auto directory = addon::normalized_resume_directory(working_directory);
        if (!directory) {
            fail(failure, "pi_profile_unavailable", "A relative Pi profile needs an absolute frozen directory.");
            return std::nullopt;
        }
        path = *directory / path;
    }
    const auto normalized = addon::normalized_resume_directory(addon::path_to_utf8(path));
    if (!normalized || addon::path_to_utf8(*normalized).size() > 4096) {
        fail(failure, "pi_profile_unavailable", "Pi profile is not a bounded absolute native path.");
        return std::nullopt;
    }
    const auto identity = addon::native_account_profile_identity(*normalized);
    if (!identity) {
        fail(failure, "pi_profile_unavailable", "The filesystem cannot establish the selected Pi profile's identity.");
        return std::nullopt;
    }
    return Native_profile{*normalized, profile_key(*identity)};
}

bool validate_account_profile(
    const std::optional<cli_addon::Account_context>& account,
    const std::optional<cli_addon::Launch_environment>& environment,
    std::string_view working_directory, cli_addon::Contract_failure* failure)
{
    if (!account) return true;
    if (!environment) {
        return fail(failure, "pi_profile_unavailable", "Selected Pi account needs the frozen launch environment.");
    }
    const auto profile = resolve_native_profile(*environment, working_directory, {}, failure);
    if (!profile) return false;
    if (!matches_grant(*profile, *account)) {
        return fail(failure, "account_profile_mismatch", "The selected Pi account does not match this frozen native profile.");
    }
    return true;
}

void handle_accounts(cli_addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.operation != addon::k_accounts_discover_operation && request.operation != addon::k_accounts_resolve_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(request.payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto launcher = resolve_frozen_launcher(input->launcher,
        input->launch_environment, input->normalization_directory);
    if (!launcher) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable",
            "The frozen launcher cannot be identified as a supported Pi installation.");
        return;
    }
    const auto profile = resolve_native_profile(input->launch_environment, input->normalization_directory,
        input->default_home_fallback, &failure);
    if (!profile) {
        context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    if (request.operation == addon::k_accounts_resolve_operation) {
        context.succeed(addon::encode_account_resolution({profile->account_key}));
        return;
    }
    addon::Account_record account;
    account.adapter_account_key = profile->account_key;
    account.profile_path = addon::path_to_utf8(profile->path);
    account.display_name = addon::account_display_name(*account.profile_path);
    account.status = availability(profile->path);
    context.succeed(addon::encode_accounts({account}));
}

} // namespace logonomic::pi
