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
#include <sstream>
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

std::optional<std::string> read_text(const fs::path& path, std::size_t maximum = 64 * 1024)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes(maximum + 1, '\0');
    input.read(bytes.data(), (std::streamsize)bytes.size());
    bytes.resize((std::size_t)input.gcount());
    if (bytes.size() > maximum || input.bad()) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<addon::Json_value> read_json(const fs::path& path)
{
    auto bytes = read_text(path);
    if (!bytes) {
        return std::nullopt;
    }
    // Pi accepts markers rewritten with a UTF-8 BOM by Windows PowerShell 5.1.
    if (bytes->starts_with("\xEF\xBB\xBF")) {
        bytes->erase(0, 3);
    }
    return addon::parse_json(*bytes, {}, nullptr);
}

std::optional<Package> read_package(const fs::path& root)
{
    const auto value = read_json(root / "package.json");
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

std::optional<Package> managed_package(const fs::path& entrypoint)
{
    const auto bin = entrypoint.parent_path();
    if (bin.filename() != "bin") {
        return std::nullopt;
    }
    const auto name = addon::lower(addon::path_to_utf8(entrypoint.filename()));
    if (name != "pi" && name != "pi.cmd" && name != "pi.ps1" && name != "pi-launcher.js") {
        return std::nullopt;
    }
    const auto root = bin.parent_path() / "install";
    const auto marker = read_json(root / "managed-install.json");
    if (!marker || json::text(*marker, "kind") != "pi-managed-install" ||
        json::text(*marker, "layout") != "releases-v1")
    {
        return std::nullopt;
    }
    const auto& schema = json::member(*marker, "schemaVersion");
    if (!schema.is_number() || schema.as_number() != 1) {
        return std::nullopt;
    }
    const auto current = read_text(root / "current-version", 256);
    if (!current) {
        return std::nullopt;
    }
    const auto version = addon::trim(*current, addon::Launcher_whitespace::ASCII);
    if (!addon::is_semantic_version(version)) {
        return std::nullopt;
    }
    const auto release = root / "releases" / addon::path_from_utf8(version);
    for (const auto package_name : k_package_names) {
        const auto package = read_package(release / "node_modules" / addon::path_from_utf8(package_name));
        if (!package || package->version != version || !addon::regular_file(package->entrypoint)) {
            continue;
        }
        if (name == "pi" && !addon::regular_file(bin / "pi-launcher.js")) {
            // The POSIX dispatcher executes this npm bin, not a guessed CLI path.
            std::error_code error;
            if (fs::equivalent(release / "node_modules/.bin/pi", package->entrypoint, error) && !error) {
                return package;
            }
        }
        else if (package_name == k_package_names[0] && addon::regular_file(bin / "pi-launcher.js")) {
            // The Windows dispatcher selects this exact package and confines its
            // declared bin to the package directory before starting Node.
            const auto relative = package->entrypoint.lexically_normal().lexically_relative(
                package->root.lexically_normal());
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
                return package;
            }
        }
    }
    return std::nullopt;
}

std::optional<bool> windows_managed_node_policy(const fs::path& entrypoint)
{
    auto bytes = read_text(entrypoint);
    if (!bytes) {
        return std::nullopt;
    }
    std::erase(*bytes, '\r');
    // These are the two generated runtime choices in pi.dev/install.ps1.
    // Match the complete wrapper so custom shell behavior is never discarded.
    // Attribution for the upstream wrapper text is in notices/pi-launcher.txt.
    const auto name = addon::lower(addon::path_to_utf8(entrypoint.filename()));
    if (name == "pi.cmd") {
        constexpr std::string_view prefix = "@ECHO off\nSETLOCAL\nSET \"node=node\"\n";
        constexpr std::string_view standalone =
            "IF EXIST \"%LOCALAPPDATA%\\pi-node\\current\\node.exe\" SET \"node=%LOCALAPPDATA%\\pi-node\\current\\node.exe\"\n";
        constexpr std::string_view suffix = R"pi("%node%" "%~dp0pi-launcher.js" %*
SET "pi_exit_code=%ERRORLEVEL%"
IF "%pi_exit_code%"=="9009" (
  IF "%node%"=="node" (
    "%SystemRoot%\System32\where.exe" /Q node
    IF ERRORLEVEL 1 GOTO node_missing
  ) ELSE IF NOT EXIST "%node%" GOTO node_missing
)
EXIT /B %pi_exit_code%
:node_missing
ECHO Pi could not find Node.js. Install Node.js 22.19.0 or newer, or rerun the installer at https://pi.dev. 1>&2
EXIT /B %pi_exit_code%
)pi";
        if (*bytes == std::string(prefix) + std::string(suffix)) {
            return false;
        }
        if (*bytes == std::string(prefix) + std::string(standalone) + std::string(suffix)) {
            return true;
        }
    }
    if (name == "pi.ps1") {
        constexpr std::string_view prefix =
            "#!/usr/bin/env pwsh\n$basedir=Split-Path $MyInvocation.MyCommand.Definition -Parent\n"
            "$launcher=Join-Path $basedir \"pi-launcher.js\"\n$node=\"node\"\n";
        constexpr std::string_view standalone =
            "$standaloneNode=Join-Path $env:LOCALAPPDATA \"pi-node/current/node.exe\"\n"
            "if (Test-Path $standaloneNode -PathType Leaf) {\n  $node=$standaloneNode\n}\n";
        constexpr std::string_view suffix = R"pi(try {
  if ($MyInvocation.ExpectingInput) {
    $input | & $node $launcher $args
  } else {
    & $node $launcher $args
  }
} catch [System.Management.Automation.CommandNotFoundException] {
  [Console]::Error.WriteLine("Pi could not find Node.js. Install Node.js 22.19.0 or newer, or rerun the installer at https://pi.dev.")
  exit 127
}
exit $LASTEXITCODE
)pi";
        if (*bytes == std::string(prefix) + std::string(suffix)) {
            return false;
        }
        if (*bytes == std::string(prefix) + std::string(standalone) + std::string(suffix)) {
            return true;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> windows_managed_node(
    const fs::path& wrapper, const addon::Environment& environment, std::string_view working_directory)
{
    const auto standalone = windows_managed_node_policy(wrapper);
    if (!standalone) {
        return std::nullopt;
    }
    if (*standalone) {
        const auto local = addon::path_from_utf8(addon::environment_value(environment, "LOCALAPPDATA"));
        if (!local.is_absolute()) {
            return std::nullopt;
        }
        const auto selected = local / "pi-node/current/node.exe";
        std::error_code error;
        const bool exists = wrapper.extension() == ".ps1"
            ? fs::is_regular_file(selected, error) : fs::exists(selected, error);
        if (exists && !error) {
            return addon::native_image(selected) ? std::optional(selected) : std::nullopt;
        }
    }
    const bool cmd = addon::lower(addon::path_to_utf8(wrapper.extension())) == ".cmd";
    const bool omit_current_directory = std::any_of(environment.begin(), environment.end(), [](const auto& entry) {
        return addon::lower(entry.first) == "nodefaultcurrentdirectoryinexepath";
    });
    if (cmd && !omit_current_directory) {
        // CMD searches .;%PATH% for a bare executable unless this switch exists,
        // regardless of its value (NeedCurrentDirectoryForExePath). Use the
        // frozen launch inputs rather than the adapter process's environment.
        const auto directory = addon::path_from_utf8(working_directory);
        if (!directory.is_absolute()) {
            return std::nullopt;
        }
        auto extensions = addon::environment_value(environment, "PATHEXT");
        if (std::none_of(environment.begin(), environment.end(), [](const auto& entry) {
                return addon::lower(entry.first) == "pathext";
            }))
        {
            extensions = ".COM;.EXE;.BAT;.CMD";
        }
        std::istringstream parts(extensions);
        for (std::string extension; std::getline(parts, extension, ';');) {
            extension = addon::trim(extension);
            if (extension.empty()) {
                continue;
            }
            if (!extension.starts_with('.')) {
                extension.insert(extension.begin(), '.');
            }
            const auto selected = addon::resolve_program(
                addon::path_to_utf8(directory / addon::path_from_utf8("node" + extension)),
                environment, working_directory);
            if (selected) {
                // A selected script must keep its ordinary CMD behavior, not be
                // skipped in favor of a different native executable on PATH.
                return addon::native_image(*selected) ? selected : std::nullopt;
            }
        }
    }
    const auto runtime = addon::resolve_program("node", environment, working_directory);
    return runtime && addon::native_image(*runtime) ? runtime : std::nullopt;
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
    const auto managed = entry ? managed_package(*entry) : std::nullopt;
    const auto package = managed ? managed : (entry ? package_for_entrypoint(*entry) : std::nullopt);
    if (!package) {
        return std::nullopt;
    }

    std::vector<std::string> prefix;
    if (managed) {
        const auto filename = addon::lower(addon::path_to_utf8(entry->filename()));
        if (node && filename == "pi-launcher.js") {
            prefix = {addon::path_to_utf8(*program), addon::path_to_utf8(*entry)};
        }
        else if (!node && filename == "pi" &&
                 !addon::regular_file(entry->parent_path() / "pi-launcher.js"))
        {
            // Keep the upstream dispatcher: it selects the active release and
            // installer-owned Node on every launch and preserves update mode.
            prefix = {addon::path_to_utf8(*entry)};
        }
        else if (!node) {
            const auto runtime = windows_managed_node(*entry, environment, working_directory);
            if (!runtime) {
                return std::nullopt;
            }
            prefix = {addon::path_to_utf8(*runtime),
                addon::path_to_utf8(entry->parent_path() / "pi-launcher.js")};
        }
        else {
            return std::nullopt;
        }
    }
    else if (node || addon::native_image(*program)) {
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

addon::Json_value evidence(std::string_view package_name, std::string_view version)
{
    return json::object({
        {"package_name", addon::Json_value(package_name)},
        {"version",      addon::Json_value(version)},
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
    const auto program = cli_addon::resolve_program("pi", environment, request.default_home_fallback);
    if (program && managed_package(*program) && windows_managed_node_policy(*program)) {
        // Persist the wrapper so later preparation re-evaluates its runtime
        // preference, including fallback when standalone Node was removed.
        const auto filename = cli_addon::lower(cli_addon::path_to_utf8(program->filename()));
        if (filename == "pi.cmd") {
            candidate.script_body = direct_script({cli_addon::path_to_utf8(*program)});
            candidate.runner_key = cli_addon::k_runner_cmd;
        }
        else {
            std::string quoted;
            for (const char value : cli_addon::path_to_utf8(*program)) {
                quoted += value == '\'' ? "''" : std::string(1, value);
            }
            candidate.script_body = "& '" + quoted + "'";
            candidate.runner_key = cli_addon::k_runner_powershell;
        }
        const auto literal = cli_addon::literal_launcher_invocation(
            {candidate.script_body, candidate.runner_key, {}}, request.platform_id);
        if (!literal || literal->argv != std::vector<std::string>{cli_addon::path_to_utf8(*program)}) {
            return cli_addon::encode_candidates({});
        }
    }
    candidate.is_default   = true;
    candidate.evidence     = evidence(launcher->package_name, launcher->version);
    return cli_addon::encode_candidates({candidate});
}

cli_addon::Json_value recognize(const cli_addon::Recognize_request& request)
{
    const auto environment = cli_addon::environment_pairs(request.ambient_environment);
    const auto launcher = resolve_launcher(request.launcher,
        environment, request.platform_id, {});
    cli_addon::Launcher_recognition result;
    if (launcher) {
        result.recognized   = true;
        result.display_name = "Pi";
        result.evidence     = evidence(launcher->package_name, launcher->version);
    }
    else if (request.launcher.environment_text.empty()) {
        // Recognition has no working directory. The known wrapper and managed
        // package establish identity without guessing CMD's selected runtime.
        // Preparation resolves its complete invocation only with frozen cwd.
        const auto invocation = cli_addon::literal_launcher_invocation(request.launcher, request.platform_id);
        if (invocation && !invocation->argv.empty() && invocation->environment_names.empty()) {
            const auto program = cli_addon::resolve_program(invocation->argv.front(), environment, {});
            if (program && cli_addon::lower(cli_addon::path_to_utf8(program->extension())) == ".cmd" &&
                windows_managed_node_policy(*program))
            {
                if (const auto package = managed_package(*program)) {
                    result.recognized = true;
                    result.display_name = "Pi";
                    result.evidence = evidence(package->name, package->version);
                }
            }
        }
    }
    return cli_addon::encode_recognition(result);
}

} // namespace logonomic::pi
