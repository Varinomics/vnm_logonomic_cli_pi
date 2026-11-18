#include "logonomic_cli_addon/launcher_utilities.h"

#include <process.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

// Windows CreateProcess cannot start the Node shebangs used by the native
// helper fixtures. Pass the original arguments and inherited pipes to Node.
int wmain(int argc, wchar_t** argv)
{
    namespace addon = logonomic::cli_addon;
    const auto node = addon::wide(PI_TEST_NODE);
    const auto script = std::filesystem::path(argv[0]).replace_extension(L".mjs");
    std::vector<std::wstring> words{addon::quote_argument(node), addon::quote_argument(script.native())};
    for (int index = 1; index < argc; ++index) {
        words.push_back(addon::quote_argument(argv[index]));
    }
    std::vector<const wchar_t*> command;
    for (const auto& word : words) {
        command.push_back(word.c_str());
    }
    command.push_back(nullptr);
    // SDK cancellation terminates this fixture executable. The interpreter
    // implements that same fixture and must retire with it.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess()))
    {
        return 1;
    }
    const auto result = _wspawnv(_P_WAIT, node.c_str(), command.data());
    // The OS closes our non-inheritable job handle on normal or forced exit.
    return result < 0 ? 1 : (int)result;
}
