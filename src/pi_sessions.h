#pragma once

#include "logonomic_cli_addon/conversation_export.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::pi {

struct Native_session
{
    std::string id;
    std::string cwd;
    std::vector<cli_addon::Json_value> entries;
    // Root-to-leaf indices for the last persisted entry, as restored by Pi.
    std::vector<std::size_t> branch;
    bool incomplete_tail_omitted = false;
};

// Read the complete, immutable prefix of this exact native session. The
// entries retain every persisted branch; branch selects Pi's restored leaf.
std::optional<Native_session> read_native_session(
    const std::filesystem::path& path,
    std::string_view expected_id,
    const std::function<bool()>& cancelled,
    cli_addon::Contract_failure* failure);

cli_addon::Resume_inspect_result inspect_resume(
    const cli_addon::Resume_inspect_request& request,
    const std::function<bool()>& cancelled);

std::optional<cli_addon::Resume_prepare_result> prepare_resume(
    const cli_addon::Resume_prepare_request& request,
    const std::filesystem::path& package_root,
    const std::function<bool()>& cancelled,
    cli_addon::Contract_failure* failure);

std::optional<cli_addon::Conversation_export_result> export_conversation(
    const cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    cli_addon::Contract_failure* failure);

} // namespace logonomic::pi
