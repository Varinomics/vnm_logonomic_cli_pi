#include "pi_launcher.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;
namespace fs    = std::filesystem;
namespace json  = cli_addon::json;

constexpr std::string_view k_package_names[] = {
    "@earendil-works/pi-coding-agent",
    "@mariozechner/pi-coding-agent",
};

struct Package
{
    fs::path root;
    fs::path entrypoint;
    std::string name;
    std::string version;
};

std::optional<Package> read_package(const fs::path& root)
{
    std::error_code error;
    const auto size = fs::file_size(root / "package.json", error);
    if (error || size > 64 * 1024) {
        return std::nullopt;
    }
    std::ifstream input(root / "package.json", std::ios::binary);
    std::string bytes(64 * 1024 + 1, '\0');
    input.read(bytes.data(), (std::streamsize)bytes.size());
    bytes.resize((std::size_t)input.gcount());
    if (bytes.size() > 64 * 1024 || input.bad()) {
        return std::nullopt;
    }
    const auto value = addon::parse_json(bytes, {}, nullptr);
    if (!value || !value->is_object()) {
        return std::nullopt;
    }
    const auto name    = json::text(*value, "name");
    const auto version = json::text(*value, "version");
    if (std::find(std::begin(k_package_names), std::end(k_package_names), name) ==
            std::end(k_package_names) ||
        !addon::is_semantic_version(version))
    {
        return std::nullopt;
    }
    const auto entry = json::text(json::member(*value, "bin"), "pi");
    if (entry.empty()) {
        return std::nullopt;
    }
    return Package{root, root / addon::path_from_utf8(entry), name, version};
}

std::optional<Package> package_for_entrypoint(const fs::path& entrypoint)
{
    std::error_code error;
    const auto canonical = fs::canonical(entrypoint, error);
    if (error) {
        return std::nullopt;
    }
    auto parent = canonical.parent_path();
    for (int depth = 0; depth < 8 && !parent.empty(); ++depth) {
        const auto package = read_package(parent);
        if (package && fs::equivalent(package->entrypoint, canonical, error) && !error) {
            return package;
        }
        const auto next = parent.parent_path();
        if (next == parent) {
            break;
        }
        parent = next;
    }
    // Pi's standalone bundle places its package metadata beside the native binary.
    const auto package = read_package(canonical.parent_path());
    const auto name    = addon::lower(addon::path_to_utf8(canonical.filename()));
    if (package && (name == "pi" || name == "pi.exe") && addon::native_image(canonical)) {
        return package;
    }
    return std::nullopt;
}

std::optional<Launcher> resolve_words(
    std::vector<std::string> words,
    const addon::Environment& environment,
    std::string_view working_directory)
{
    const auto program = addon::resolve_program(words.front(), environment, working_directory);
    if (!program) {
        return std::nullopt;
    }
    const auto name = addon::lower(addon::path_to_utf8(program->filename()));
    const bool node = name == "node" || name == "node.exe";
    if (node && (words.size() < 2 || !addon::native_image(*program))) {
        return std::nullopt;
    }
    fs::path entry_path;
    if (node) {
        entry_path = addon::path_from_utf8(words[1]);
        if (entry_path.is_relative()) {
            if (working_directory.empty()) {
                return std::nullopt;
            }
            entry_path = addon::path_from_utf8(working_directory) / entry_path;
        }
    }
    const auto entry = node
        ? addon::regular_file(entry_path)
        : program;
    const auto package = entry ? package_for_entrypoint(*entry) : std::nullopt;
    if (!package) {
        return std::nullopt;
    }

    std::vector<std::string> prefix;
    if (node || addon::native_image(*program)) {
        prefix.push_back(addon::path_to_utf8(*program));
        if (node) {
            prefix.push_back(addon::path_to_utf8(*entry));
        }
    }
    else {
        const auto runtime = addon::resolve_program("node", environment, working_directory);
        if (!runtime || !addon::native_image(*runtime)) {
            return std::nullopt;
        }
        prefix = {addon::path_to_utf8(*runtime), addon::path_to_utf8(package->entrypoint)};
    }
    const auto prefix_size = prefix.size();
    prefix.insert(prefix.end(), words.begin() + (node ? 2 : 1), words.end());
    return Launcher{std::move(prefix), package->name, package->version, prefix_size};
}

std::string direct_script(const std::vector<std::string>& words)
{
    std::string result;
    for (const auto& word : words) {
        if (!result.empty()) {
            result += ' ';
        }
        result += '"';
        for (const char value : word) {
            result += value == '"' ? "\"\"\"" : std::string(1, value);
        }
        result += '"';
    }
    return result;
}

addon::Json_value evidence(const Launcher& launcher)
{
    return json::object({
        {"package_name", addon::Json_value(launcher.package_name)},
        {"version",      addon::Json_value(launcher.version)},
    });
}

} // namespace

std::optional<Launcher> resolve_launcher(
    const cli_addon::Launcher_request& launcher,
    const cli_addon::Environment& environment,
    std::string_view platform,
    std::string_view working_directory,
    std::optional<std::span<const std::string>> host_argv)
{
    const auto invocation = cli_addon::literal_launcher_invocation(launcher, platform, host_argv);
    if (!invocation || invocation->argv.empty()) {
        return std::nullopt;
    }
    // Recognition receives the ambient view, not the launcher's effective
    // overrides. A different PATH may select another program with the same name.
    if (!launcher.environment_text.empty() || !invocation->environment_names.empty()) {
        return std::nullopt;
    }
    return resolve_words(invocation->argv, environment, working_directory);
}

cli_addon::Json_value discover(const cli_addon::Discover_request& request)
{
    const auto environment = cli_addon::environment_pairs(request.ambient_environment);
    auto launcher = resolve_words({"pi"}, environment, request.default_home_fallback);
    if (!launcher) {
        // Windows npm launchers are shell scripts. Discovery can offer the package's
        // direct Node entrypoint without executing or rewriting the user's shim.
        const auto shim = cli_addon::resolve_program("pi", environment, request.default_home_fallback);
        if (shim) {
            const auto node = cli_addon::resolve_program("node", environment, request.default_home_fallback);
            for (const auto package_name : k_package_names) {
                const auto package = read_package(shim->parent_path() / "node_modules" /
                    cli_addon::path_from_utf8(package_name));
                if (package && node) {
                    launcher = resolve_words(
                        {cli_addon::path_to_utf8(*node), cli_addon::path_to_utf8(package->entrypoint)},
                        environment, request.default_home_fallback);
                    if (launcher) {
                        break;
                    }
                }
            }
        }
    }
    if (!launcher) {
        return cli_addon::encode_candidates({});
    }
    cli_addon::Launcher_candidate candidate;
    candidate.candidate_id = "pi";
    candidate.display_name = "Pi";
    candidate.script_body  = direct_script(launcher->argv);
    candidate.runner_key   = cli_addon::k_runner_direct;
    candidate.is_default   = true;
    candidate.evidence     = evidence(*launcher);
    return cli_addon::encode_candidates({candidate});
}

cli_addon::Json_value recognize(const cli_addon::Recognize_request& request)
{
    const auto launcher = resolve_launcher(request.launcher,
        cli_addon::environment_pairs(request.ambient_environment), request.platform_id, {});
    cli_addon::Launcher_recognition result;
    if (launcher) {
        result.recognized   = true;
        result.display_name = "Pi";
        result.evidence     = evidence(*launcher);
    }
    return cli_addon::encode_recognition(result);
}

} // namespace logonomic::pi
