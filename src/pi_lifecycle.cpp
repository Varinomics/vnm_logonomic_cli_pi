#include "pi_lifecycle.h"

#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/lifecycle_events.h"

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
            "attention_id", "reason", "aborted", "outcome"}) ||
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

} // namespace

std::optional<cli_addon::Classify_event_result> classify_event(
    const cli_addon::Classify_event_request& request,
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
    return result;
}

} // namespace logonomic::pi
