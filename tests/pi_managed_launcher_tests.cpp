#include "pi_launcher.h"
#include "pi_preparation.h"

#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace pi    = logonomic::pi;
namespace json  = logonomic::cli_addon::json;
namespace fs    = std::filesystem;

int assertions = 0;

void check(bool condition, const std::string& message)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read(const fs::path& file)
{
    std::ifstream input(file, std::ios::binary);
    check(input.good(), "fixture is readable: " + addon::path_to_utf8(file));
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void write(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    output << bytes;
    output.close();
    check(output.good(), "fixture is writable: " + addon::path_to_utf8(file));
    fs::permissions(file, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

std::string quote(const fs::path& path)
{
    return "\"" + addon::path_to_utf8(path) + "\"";
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }

private:
    addon::Invoke_request m_request;
};

struct Installation
{
    fs::path home = fs::temp_directory_path() / ("pi managed " + addon::random_nonce_hex());
    fs::path agent = home / ".pi/agent";
    fs::path root = agent / "install";
    fs::path bin = agent / "bin";
    fs::path user_bin = home / "user-bin";
    fs::path fixtures;
    addon::Environment environment;

    Installation(const fs::path& source, const fs::path& node) : fixtures(source)
    {
        fs::create_directories(user_bin);
        environment = {
            {"HOME", addon::path_to_utf8(home)},
            {"XDG_DATA_HOME", addon::path_to_utf8(home / "data")},
            {"LOCALAPPDATA", addon::path_to_utf8(home / "local")},
            {"PATH", addon::path_to_utf8(user_bin) + ":" + addon::path_to_utf8(node.parent_path()) + ":/usr/bin:/bin"},
        };
        marker();
        release("1.5.2");
        activate("1.5.2");
    }

    ~Installation()
    {
        std::error_code error;
        fs::remove_all(home, error);
    }

    void marker()
    {
        write(root / "managed-install.json",
            "{\"kind\":\"pi-managed-install\",\"schemaVersion\":1,\"layout\":\"releases-v1\"}\n");
    }

    void release(std::string version)
    {
        const auto modules = root / "releases" / version / "node_modules";
        const auto package = modules / "@earendil-works/pi-coding-agent";
        write(package / "package.json",
            "{\"name\":\"@earendil-works/pi-coding-agent\",\"version\":\"" + version +
            "\",\"bin\":{\"pi\":\"dist/cli.cjs\"}}\n");
        // The stand-in reports what the unmodified upstream dispatcher actually
        // selected. No model, credentials or Pi installation is required.
        write(package / "dist/cli.cjs",
            "#!/usr/bin/env node\n"
            "console.log(JSON.stringify({version:'" + version + "',args:process.argv.slice(2),"
            "managedRoot:process.env.PI_MANAGED_INSTALL_ROOT,path:process.env.PATH}));\n");
        fs::create_directories(modules / ".bin");
        fs::create_symlink(package / "dist/cli.cjs", modules / ".bin/pi");
    }

    void activate(std::string_view version)
    {
        write(root / "current-version.tmp", std::string(version) + "\n");
        fs::rename(root / "current-version.tmp", root / "current-version");
    }

    void wrapper(std::string_view source, std::string_view destination)
    {
        write(bin / destination, read(fixtures / source));
    }

    void select(std::string_view name)
    {
        const auto selected = user_bin / "pi";
        std::error_code error;
        fs::remove(selected, error);
        fs::create_symlink(bin / name, selected);
    }

    addon::Json_value ambient() const
    {
        auto result = addon::Json_value::object();
        for (const auto& [name, value] : environment) {
            result.set(name, addon::Json_value(value));
        }
        return result;
    }

    addon::Launcher_request discover(std::string platform) const
    {
        addon::Discover_request request;
        request.ambient_environment = ambient();
        request.default_home_fallback = addon::path_to_utf8(home);
        request.platform_id = platform;
        const auto result = addon::decode_discover_result(pi::discover(request), platform, nullptr);
        check(result && result->size() == 1, "the official managed installation is discovered");
        const auto& candidate = result->front();
        check(candidate.script_body.find("/releases/") == std::string::npos,
            "a saved launcher never embeds the selected release directory");
        addon::Launcher_request launcher{candidate.script_body, candidate.runner_key, candidate.environment_text};
        addon::Recognize_request recognize{launcher, ambient(), std::move(platform)};
        const auto recognized = addon::decode_recognize_result(pi::recognize(recognize), nullptr);
        check(recognized && recognized->recognized, "the saved launcher is recognized again");
        return launcher;
    }

    std::optional<pi::Launcher> resolve(const addon::Launcher_request& launcher, std::string_view platform) const
    {
        return pi::resolve_launcher(launcher, environment, platform, addon::path_to_utf8(home));
    }

    addon::Json_value run(const pi::Launcher& launcher) const
    {
        Context context;
        const auto output = addon::probe(launcher.argv, environment, addon::path_to_utf8(home), 5000, context);
        check(output.has_value(), "the resolved upstream dispatcher runs successfully");
        const auto result = addon::parse_json(output->output, {}, nullptr);
        check(result && result->is_object(), "the selected release reports its actual invocation");
        check(json::text(*result, "managedRoot") == addon::path_to_utf8(root),
            "the upstream launcher preserves Pi's managed-update environment");
        return *result;
    }

    void preparation(const addon::Launcher_request& launcher, const pi::Launcher& resolved,
        const std::string& platform) const
    {
        addon::Prepare_request request;
        request.launcher = launcher;
        request.working_directory = addon::path_to_utf8(home);
        request.declared_assets = {"assets/logonomic-pi.mjs"};
        request.launch_environment = addon::Launch_environment{platform, ambient(), std::nullopt};
        request.launcher_argv = launcher.runner_key == addon::k_runner_cmd
            ? std::vector<std::string>{"cmd.exe", "/c", launcher.script_body}
            : addon::literal_argv(launcher.script_body);
        const auto patch = pi::prepare(request);
        check(patch.disposition && patch.disposition->prepared && patch.disposition->lifecycle_events,
            "the managed launcher admits the existing interactive integration");
        auto expected = resolved.argv;
        expected.insert(expected.begin() + (std::ptrdiff_t)resolved.argument_prefix_size,
            {"--extension", "{{asset:assets/logonomic-pi.mjs}}"});
        check(patch.argv && *patch.argv == expected,
            "preparation inserts the extension after the stable dispatch prefix and preserves arguments");
        addon::Contract_failure failure;
        check(addon::validate_launch_patch(patch, request.declared_assets, request.declared_assets, &failure),
            "the SDK accepts the managed launch patch: " + failure.reason_code);
    }
};

void posix_contract(const fs::path& fixtures, const fs::path& node)
{
    Installation install(fixtures, node);
    install.wrapper("pi", "pi");
    install.select("pi");
    auto saved = install.discover("linux-x86_64");
    check(saved.runner_key == addon::k_runner_direct, "POSIX keeps the executable upstream dispatcher");
    saved.script_body += " --model \"model with spaces\" \"prompt & literal\"";
    auto resolved = install.resolve(saved, "linux-x86_64");
    check(resolved && resolved->argument_prefix_size == 1 &&
        resolved->argv.front() == addon::path_to_utf8(install.bin / "pi"),
        "the dispatcher remains the executable before Pi's arguments");
    install.preparation(saved, *resolved, "linux-x86_64");
    auto observed = install.run(*resolved);
    check(json::text(observed, "version") == "1.5.2", "the initial selected release runs");
    check(json::member(observed, "args") == addon::Json_value(addon::Json_value::Array{
        addon::Json_value("--model"), addon::Json_value("model with spaces"), addon::Json_value("prompt & literal")}),
        "argument boundaries survive the source wrapper");

    const auto standalone = install.home / "data/pi-node/current/bin/node";
    fs::create_directories(standalone.parent_path());
    fs::create_symlink(node, standalone);
    observed = install.run(*resolved);
    check(json::text(observed, "path").starts_with(addon::path_to_utf8(standalone.parent_path()) + ":"),
        "the source dispatcher keeps installer-owned Node first on child PATH");

    install.release("1.5.3");
    install.activate("1.5.3");
    fs::remove_all(install.root / "releases/1.5.2");
    auto updated = install.resolve(saved, "linux-x86_64");
    check(updated && updated->version == "1.5.3" && updated->argv == resolved->argv,
        "a saved launcher follows an update without changing its argv");
    check(json::text(install.run(*updated), "version") == "1.5.3",
        "the saved upstream dispatcher runs the new release after old-release pruning");

    write(install.root / "managed-install.json", "\xEF\xBB\xBF" + read(install.root / "managed-install.json"));
    check(install.resolve(saved, "linux-x86_64").has_value(), "a PowerShell BOM does not invalidate Pi's marker");
    write(install.root / "current-version", "../outside\n");
    check(!install.resolve(saved, "linux-x86_64"), "an invalid active release cannot claim Pi identity");
    install.activate("9.9.9");
    check(!install.resolve(saved, "linux-x86_64"), "a missing active release is not inferred from stale metadata");
    install.activate("1.5.3");
    write(install.root / "managed-install.json", "{\"kind\":\"unrelated-install\"}\n");
    check(!install.resolve(saved, "linux-x86_64"), "unrelated adjacent metadata is not Pi proof");
}

void windows_dispatch_contract(const fs::path& fixtures, const fs::path& node)
{
    Installation install(fixtures, node);
    install.wrapper("pi-launcher.js", "pi-launcher.js");
    install.wrapper("pi.cmd", "pi.cmd");
    install.wrapper("pi.ps1", "pi.ps1");
    install.select("pi.cmd");
    auto saved = install.discover("windows-x86_64");
    check(saved.runner_key == addon::k_runner_cmd && saved.script_body.find("pi.cmd") != std::string::npos,
        "Windows saves the stable cmd wrapper for future runtime decisions");
    auto resolved = install.resolve(saved, "windows-x86_64");
    check(resolved && resolved->argument_prefix_size == 2 &&
        resolved->argv[1] == addon::path_to_utf8(install.bin / "pi-launcher.js"),
        "preparation targets the upstream stable Node dispatcher");
    install.preparation(saved, *resolved, "windows-x86_64");
    check(json::text(install.run(*resolved), "version") == "1.5.2", "the actual Windows JS dispatcher selects its release");

    install.release("1.5.3");
    install.activate("1.5.3");
    fs::remove_all(install.root / "releases/1.5.2");
    auto updated = install.resolve(saved, "windows-x86_64");
    check(updated && updated->version == "1.5.3" && updated->argv == resolved->argv &&
        json::text(install.run(*updated), "version") == "1.5.3",
        "the saved Windows wrapper follows the new release after pruning");

    const auto standalone = install.home / "local/pi-node/current/node.exe";
    fs::create_directories(standalone.parent_path());
    fs::create_symlink(node, standalone);
    check(install.resolve(saved, "windows-x86_64")->argv.front() == addon::path_to_utf8(node),
        "the PATH-only wrapper does not adopt an unrelated standalone Node");
    install.wrapper("pi.cmd.standalone", "pi.cmd");
    resolved = install.resolve(saved, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(standalone),
        "the installer-recorded standalone Node choice is preserved");
    check(json::text(install.run(*resolved), "version") == "1.5.3", "standalone Node runs the stable dispatcher");
    fs::remove(standalone);
    resolved = install.resolve(saved, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(node),
        "the same saved launcher follows upstream PATH fallback when standalone Node is removed");

    addon::Launcher_request explicit_node{
        quote(node) + " " + quote(install.bin / "pi-launcher.js") + " --model \"selected model\"", "direct", {}};
    resolved = install.resolve(explicit_node, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(node),
        "an explicit Node dispatcher invocation preserves the selected runtime");
    check(json::member(install.run(*resolved), "args") == addon::Json_value(addon::Json_value::Array{
        addon::Json_value("--model"), addon::Json_value("selected model")}),
        "the Windows JS dispatcher preserves argument boundaries");

    install.select("pi.ps1");
    const auto powershell = install.discover("windows-x86_64");
    check(powershell.runner_key == addon::k_runner_powershell && install.resolve(powershell, "windows-x86_64"),
        "the official PowerShell wrapper preserves its runner and is recognized");
    install.wrapper("pi.ps1.standalone", "pi.ps1");
    check(install.resolve(powershell, "windows-x86_64").has_value(), "PowerShell's standalone removal fallback is supported");

    write(install.bin / "pi.cmd", "@ECHO custom behavior\n" + read(install.bin / "pi.cmd"));
    check(!install.resolve(saved, "windows-x86_64"), "a custom wrapper is never replaced by guessed native argv");
}

void windows_command_search_contract(const fs::path& fixtures, const fs::path& node, const fs::path& self)
{
    Installation install(fixtures, node);
    install.wrapper("pi-launcher.js", "pi-launcher.js");
    install.wrapper("pi.cmd", "pi.cmd");
    install.wrapper("pi.ps1", "pi.ps1");
    install.select("pi.cmd");
    addon::set_environment(install.environment, "PATHEXT", ".exe;.cmd");
    const auto saved = install.discover("windows-x86_64");

    // Microsoft's NeedCurrentDirectoryForExePath contract specifies that CMD
    // searches .;%PATH% unless NoDefaultCurrentDirectoryInExePath exists.
    // A different native executable proves selection without running Windows
    // or the stand-in executable on this platform.
    const auto cwd_node = install.home / "node.exe";
    fs::copy_file(self, cwd_node);
    auto resolved = install.resolve(saved, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(cwd_node),
        "CMD selects the working-directory node.exe before the different PATH Node");
    install.preparation(saved, *resolved, "windows-x86_64");
    check(!pi::resolve_launcher(saved, install.environment, "windows-x86_64", {}),
        "unknown cwd cannot produce a guessed CMD runtime replacement");

    const auto switch_name = std::string("NoDefaultCurrentDirectoryInExePath");
    for (const auto value : {"", "0", "1"}) {
        addon::set_environment(install.environment, switch_name, value);
        resolved = install.resolve(saved, "windows-x86_64");
        check(resolved && resolved->argv.front() == addon::path_to_utf8(node),
            "CMD suppresses implicit cwd search by switch presence, including an empty or zero value");
    }
    std::erase_if(install.environment, [&](const auto& entry) { return entry.first == switch_name; });
    install.environment.emplace_back("nODEFAULTcurrentDIRECTORYinEXEpath", "");
    resolved = install.resolve(saved, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(node),
        "Windows switch names are matched case-insensitively");
    install.environment.pop_back();

    const addon::Launcher_request powershell{quote(install.bin / "pi.ps1"), "direct", {}};
    resolved = install.resolve(powershell, "windows-x86_64");
    check(resolved && resolved->argv.front() == addon::path_to_utf8(node),
        "CMD's implicit current-directory search is not applied to PowerShell");

    addon::set_environment(install.environment, "PATHEXT", ".cmd;.exe");
    write(install.home / "node.cmd", "@ECHO custom node wrapper\n");
    check(!install.resolve(saved, "windows-x86_64"),
        "a selected cwd script is not silently replaced with a different native PATH runtime");
}

void frozen_environment_contract(const fs::path& source)
{
    for (const auto sidecar : {"launcher-discovery@1.json", "launch-preparation@3.json"}) {
        const auto document = addon::parse_json(read(source / "package/capabilities" / sidecar), {}, nullptr);
        check(document.has_value(), "the launcher capability sidecar is valid JSON");
        const auto declaration = addon::decode_launcher_discovery_declaration(json::member(*document, "payload"), nullptr);
        check(declaration.has_value(), "the launcher environment allowlist is valid");
        const auto frozen = addon::ambient_view({{"nODEFAULTcurrentDIRECTORYinEXEpath", ""}},
            *declaration, "windows-x86_64");
        const auto* present = frozen.find("NoDefaultCurrentDirectoryInExePath");
        check(present && present->is_string() && present->as_string().empty(),
            "the host exposes switch presence and empty values to both launcher capabilities");
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 3, "usage: pi_managed_launcher_tests <fixtures-directory> <node-executable>");
        const auto fixtures = fs::canonical(argv[1]);
        const auto node = fs::canonical(argv[2]);
        posix_contract(fixtures, node);
        windows_dispatch_contract(fixtures, node);
        windows_command_search_contract(fixtures, node, fs::canonical(argv[0]));
        frozen_environment_contract(fixtures.parent_path().parent_path().parent_path());
        std::cout << assertions << " managed-launcher assertions passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
