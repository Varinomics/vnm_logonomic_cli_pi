#include "pi_lifecycle.h"
#include "pi_sessions.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/context_state.h"
#include "logonomic_cli_addon/execution_context.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/lifecycle_events.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace logonomic::pi {
namespace {

namespace addon = cli_addon;
namespace json  = cli_addon::json;

bool valid_body(const addon::Json_value& body)
{
    if (!json::only_keys(body, {
            "session_id", "session_file", "cwd", "event_id", "turn_id",
            "attention_id", "reason", "aborted", "outcome", "context_updates",
            "context_failure", "execution_context"}) ||
        !json::bounded_string(json::member(body, "session_id"), 256) ||
        !json::bounded_string(json::member(body, "event_id"), 256))
    {
        return false;
    }
    for (const auto key : {"turn_id", "attention_id", "reason", "outcome"}) {
        const auto* value = body.find(key);
        if (value && !json::bounded_string(*value, 160, true)) {
            return false;
        }
    }
    for (const auto key : {"session_file", "cwd"}) {
        const auto* value = body.find(key);
        if (value && !json::bounded_string(*value, 4096, true)) {
            return false;
        }
    }
    const auto* aborted = body.find("aborted");
    return !aborted || aborted->is_boolean();
}

std::optional<addon::Classify_event_result> invalid(addon::Contract_failure* failure)
{
    if (failure) {
        *failure = {"invalid_pi_event", "Pi event lacks its required native lifecycle evidence."};
    }
    return std::nullopt;
}

void context_failure(
    addon::Classify_event_result& result, const std::string& session, const std::string& detail)
{
    result.context_updates.clear();
    result.context_failure = addon::Context_failure{session, {"observation_invalid", detail}};
}

void classify_context(
    const addon::Classify_event_request& request,
    const std::function<bool()>&        cancelled,
    addon::Classify_event_result&       result)
{
    const auto& body = request.event.body;
    const auto session = json::text(body, "session_id");
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{session, *request.context_failure};
        return;
    }
    if (request.context_state_version != addon::k_context_state_capability_version) {
        return;
    }
    if (const auto* updates = body.find("context_updates")) {
        if (!updates->is_array() || updates->as_array().size() != 1 ||
            !json::only_keys(updates->as_array().front(), {
                "provider_session_id", "event_id", "used_tokens", "context_window", "usage_unavailable"}))
        {
            context_failure(result, session, "Pi context usage has an invalid shape.");
            return;
        }
    }
    addon::decode_context_updates(body, result.context_updates, result.context_failure);
    if (result.context_failure) {
        if (result.context_failure->provider_session_id != session) {
            context_failure(result, session, "Pi context failure does not identify its native source.");
        }
        return;
    }
    if (result.context_updates.empty()) {
        return;
    }
    auto& update = result.context_updates.front();
    if (update.provider_session_id != session || update.event_id != json::text(body, "event_id") ||
        (!update.usage_unavailable && (!update.used_tokens || !update.context_window)))
    {
        context_failure(result, session, "Pi context usage lacks matching native evidence.");
        return;
    }
    if (request.context_snapshot_revision) {
        addon::Contract_failure failure;
        std::optional<Native_session> snapshot;
        try {
            snapshot = read_native_session(
                addon::path_from_utf8(json::text(body, "session_file")), session, cancelled, &failure);
        }
        catch (const std::exception&) {
            // Failed history proof cannot discard independent native occupancy or lifecycle evidence.
        }
        update.compaction_count_revision = request.context_snapshot_revision;
        // A fresh unreadable history proves only a zero lower bound, never a complete zero count.
        // The host retains prior counts and hides them until completeness is established again.
        update.compaction_count = snapshot
            ? (std::uint64_t)std::count_if(snapshot->entries.begin(), snapshot->entries.end(),
                  [](const auto& entry) { return json::text(entry, "type") == "compaction"; })
            : 0;
        update.compaction_count_complete = snapshot && !snapshot->incomplete_tail_omitted;
    }
}

void classify_execution(
    const addon::Classify_event_request& request, addon::Classify_event_result& result)
{
    result.execution_failure = request.execution_failure;
    if (request.execution_context_version != addon::k_execution_context_version || result.execution_failure) {
        return;
    }
    addon::decode_execution_updates(request.event.body, result.execution_updates, result.execution_failure);
    if (result.execution_failure || result.execution_updates.empty()) {
        return;
    }
    const auto& update = result.execution_updates.front();
    if (result.execution_updates.size() != 1 ||
        update.provider_session_id != json::text(request.event.body, "session_id") ||
        update.transition_nonce != request.transition_nonce)
    {
        result.execution_updates.clear();
        result.execution_failure = addon::Contract_failure{
            "observation_invalid", "Pi execution context lacks matching native attribution."};
    }
}

void classify_telemetry(
    const addon::Classify_event_request& request,
    const std::function<bool()>&        cancelled,
    addon::Classify_event_result&       result)
{
    classify_context(request, cancelled, result);
    classify_execution(request, result);
}

} // namespace

std::optional<cli_addon::Classify_event_result> classify_event(
    const cli_addon::Classify_event_request& request,
    const std::function<bool()>& cancelled,
    cli_addon::Contract_failure* failure)
{
    const auto& body = request.event.body;
    const auto& name = request.event.name;
    if (!valid_body(body)) {
        return invalid(failure);
    }
    addon::Classify_event_result result;
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = json::text(body, "session_id");
    observation.provider_event_id   = json::text(body, "event_id");
    observation.provider_turn_id    = json::text(body, "turn_id");
    const auto reason = json::text(body, "reason");

    if (name == "session_start") {
        if (reason.empty()) {
            return invalid(failure);
        }
        result.classification = addon::Classification::TRANSITION;
        result.transition = reason == "resume" || reason == "reload"
            ? addon::Transition_kind::RESUME
            : addon::Transition_kind::START;
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source     = reason;
        observation.provider_turn_id.clear();
        const auto session_file = json::text(body, "session_file");
        if (!session_file.empty()) {
            observation.resume_record = json::object({
                {"provider_session_id", addon::Json_value(observation.provider_session_id)},
                {"session_file",        addon::Json_value(session_file)},
            });
        }
    }
    else {
        // The activation nonce prevents delayed callbacks from A being applied
        // after switching A -> B -> A, even when the native session id matches.
        const auto& transition = request.ledger.current_transition;
        if (!transition || transition->provider_session_id != observation.provider_session_id ||
            transition->nonce != request.transition_nonce)
        {
            return result;
        }
        if (name == "context_snapshot") {
            classify_telemetry(request, cancelled, result);
            return result;
        }
        result.classification = addon::Classification::EVENT;
        if (name == "agent_start") {
            if (observation.provider_turn_id.empty()) {
                return invalid(failure);
            }
            observation.phase_transition = addon::Phase_transition::WORKING;
            observation.phase_evidence   = addon::Evidence::CONFIRMED;
        }
        else
        if (name == "agent_settled") {
            const auto* aborted = body.find("aborted");
            const auto outcome  = json::text(body, "outcome");
            if (observation.provider_turn_id.empty() || !aborted || outcome.empty()) {
                return invalid(failure);
            }
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence   = addon::Evidence::CONFIRMED;
            if (aborted->as_boolean() || outcome == "interrupted") {
                observation.outcome_transition = addon::Outcome_transition::INTERRUPTED;
            }
            else
            if (outcome == "failed") {
                observation.outcome_transition = addon::Outcome_transition::FAILED;
            }
            else
            if (outcome == "succeeded") {
                observation.outcome_transition = addon::Outcome_transition::SUCCEEDED;
            }
            else
            if (outcome != "none") {
                return invalid(failure);
            }
            if (observation.outcome_transition != addon::Outcome_transition::NONE) {
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
        else
        if (name == "ui_prompt_start" || name == "ui_prompt_end") {
            observation.attention_request_id = json::text(body, "attention_id");
            if (observation.attention_request_id.empty()) {
                return invalid(failure);
            }
            observation.attention_transition = name == "ui_prompt_start"
                ? addon::Attention_transition::REQUESTED
                : addon::Attention_transition::RESOLVED;
            observation.attention_evidence = addon::Evidence::CONFIRMED;
        }
        else
        if (name == "session_shutdown" && reason == "quit") {
            observation.session_transition = addon::Session_transition::ENDED;
            observation.session_end_reason = reason;
            observation.provider_turn_id.clear();
        }
        else {
            return addon::Classify_event_result{};
        }
    }
    // Apply the public codec's semantic bounds before returning evidence to the host.
    if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation),
        failure, addon::Lifecycle_observation_policy::HELPER_INGRESS))
    {
        return std::nullopt;
    }
    result.observations.push_back(std::move(observation));
    classify_telemetry(request, cancelled, result);
    return result;
}

} // namespace logonomic::pi
