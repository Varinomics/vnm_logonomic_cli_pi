#include "pi_usage.h"
#include "pi_profile.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json_utilities.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"
#include "logonomic_cli_addon/usage_session.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace logonomic::pi {
namespace {
namespace addon = cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::string_view k_format = "pi-usage-receipts-v1";
constexpr std::string_view k_session_prefix = "pi-session:";

void require(bool condition, const std::string& reason)
{
    if (!condition) throw std::runtime_error(reason.empty() ? "pi_usage_unavailable" : reason);
}

std::string digest(std::string_view bytes)
{
    return addon::to_hex(addon::Sha256::digest(bytes));
}

std::string fingerprint(addon::Usage_scan_request request)
{
    request.cursor.reset();
    request.snapshot_id.reset();
    return digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(request)));
}

bool receipt_id(std::string_view key)
{
    return key.starts_with("pi-event:") && addon::sha256_from_hex(key.substr(9)).has_value();
}

void write_json(const fs::path& path, const Json& value)
{
    std::ofstream stream(path, std::ios::binary);
    stream << addon::serialize_json(value) << '\n';
    stream.close();
    require(static_cast<bool>(stream), "pi_usage_cache_write_failed");
}

Json read_json(const fs::path& path)
{
    constexpr std::uint64_t maximum = addon::limits::k_max_frame_bytes * 2;
    const auto size = fs::file_size(path);
    require(size <= maximum, "pi_usage_cache_file_invalid");
    std::ifstream stream(path, std::ios::binary);
    std::string raw(static_cast<std::size_t>(size), '\0');
    stream.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    require(static_cast<bool>(stream) && stream.peek() == std::char_traits<char>::eof(), "pi_usage_cache_file_invalid");
    addon::Json_parse_limits limits;
    limits.max_bytes = maximum;
    limits.max_members = 65536;
    const auto value = addon::parse_json(raw, limits, nullptr);
    require(value.has_value(), "pi_usage_cache_file_invalid");
    return *value;
}

Json serialize_receipt(const Usage_receipt& receipt)
{
    return json::object({
        {"accounting", addon::encode_usage_record(receipt.record)}, {"instant", Json(receipt.instant)},
        {"atomic", Json(receipt.atomic)}, {"fingerprint", Json(receipt.evidence_digest)},
    });
}

Usage_receipt parse_receipt(const Json& value)
{
    addon::Contract_failure failure;
    const auto record = addon::decode_usage_record(json::member(value, "accounting"), &failure);
    const auto& atomic = json::member(value, "atomic");
    Usage_receipt result;
    result.instant = json::text(value, "instant");
    result.evidence_digest = json::text(value, "fingerprint");
    require(record && receipt_id(record->record_id) && atomic.is_boolean() &&
        addon::sha256_from_hex(result.evidence_digest) && result.instant.size() == 24 &&
        result.instant.ends_with('Z') && addon::parse_native_usage_timestamp(result.instant) &&
        record->completed_date == result.instant.substr(0, 10) &&
        record->semantics == addon::Usage_counter_semantics::DELTA_EVENT && record->aggregate_evidence == "complete" &&
        record->capabilities.as_object().empty(), "pi_usage_cache_receipt_invalid");
    result.record = *record;
    result.record.aggregate_evidence = "complete";
    result.atomic = atomic.as_boolean();
    return result;
}

std::string page_filename(std::uint64_t number)
{
    return "page-" + std::to_string(number) + ".json";
}

struct Cache
{
    addon::Usage_cache_reference reference;
    addon::Usage_cache_manifest manifest;
    addon::Observation_scope scope;
    std::uint64_t pages = 0;
};

Cache open_cache(const addon::Usage_cache_reference& reference, const addon::Usage_scan_request& request,
    const Native_profile& profile, bool continuation, const Usage_progress& cancelled)
{
    addon::Contract_failure failure;
    const auto manifest = addon::validate_usage_cache_manifest(reference, request.context.profile_storage->root,
        request.context.account_context, cancelled, &failure);
    require(manifest && manifest->format == k_format, "pi_usage_cache_invalid");
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(reference.root));
    const bool valid = addon::validate_usage_cache_file(reference, *manifest, "source.json", cancelled, &failure);
    require(valid, failure.reason_code);
    auto meta = read_json(root / "source.json");
    const auto& pages = json::member(meta, "pages");
    const auto scope = addon::decode_observation_scope(json::member(meta, "scope"), &failure);
    const auto account_key = request.context.account_context ? std::optional(request.context.account_context->adapter_account_key) : std::nullopt;
    require(scope && scope->source_account_key == account_key && json::text(meta, "profile_key") == profile.account_key &&
        pages.is_integer() && pages.as_integer() > 0 &&
        static_cast<std::uint64_t>(pages.as_integer()) == manifest->files.size() - 1, "pi_usage_cache_scope_invalid");
    if (continuation) require(json::text(meta, "request") == fingerprint(request), "pi_usage_snapshot_context_changed");
    return {reference, *manifest, *scope, static_cast<std::uint64_t>(pages.as_integer())};
}

Json read_page(const Cache& cache, std::uint64_t index, const Usage_progress& cancelled)
{
    addon::Contract_failure failure;
    const auto name = page_filename(index);
    const bool valid = addon::validate_usage_cache_file(cache.reference, cache.manifest, name, cancelled, &failure);
    require(valid, failure.reason_code);
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(cache.reference.root));
    auto page = read_json(root / name);
    require(page.is_array() && page.as_array().size() <= addon::k_max_usage_page_records, "pi_usage_cache_page_invalid");
    return page;
}

addon::Usage_record observation(const Usage_receipt& receipt, const addon::Usage_scan_page& page)
{
    auto result = receipt.record;
    if (result.session_key) {
        if (result.session_key->size() + k_session_prefix.size() <= 256) {
            result.session_key = std::string(k_session_prefix) + *result.session_key;
        }
        else result.session_key.reset();
    }
    addon::Usage_temporal temporal;
    temporal.parent = {result.record_id, result.total_tokens, page.snapshot_id, page.scope.source_account_key};
    if (receipt.atomic) {
        temporal.portions.push_back({"instant", receipt.instant, {}, {}, result.total_tokens});
    }
    else {
        // Pi can combine several calls before saving a tool result or summary.
        temporal.unavailable_tokens = result.total_tokens;
        temporal.coverage_reason = "missing-evidence";
    }
    result.temporal = std::move(temporal);
    if (receipt.atomic) result.cross_source_event = addon::Usage_cross_source_event{"pi.native-usage-entry", digest(result.record_id + receipt.evidence_digest)};
    return result;
}

addon::Usage_scan_page base_page(const addon::Usage_scan_request& request, const addon::Usage_cache_reference& reference,
    const addon::Observation_scope& scope)
{
    addon::Usage_scan_page page;
    page.scope = scope;
    page.snapshot_id = reference.snapshot_id;
    page.cache_candidate = reference;
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    page.incomplete_historical_coverage = true;
    return page;
}

void merge_accepted(Usage_receipts& current, const Cache& accepted, const Usage_progress& cancelled)
{
    for (std::uint64_t index = 0; index < accepted.pages; ++index) {
        auto page = read_page(accepted, index, cancelled);
        for (const auto& raw : page.as_array()) {
            require(!cancelled(), "cancelled");
            auto previous = parse_receipt(raw);
            const auto [item, inserted] = current.try_emplace(previous.record.record_id, previous);
            if (inserted) continue;
            auto& observed = item->second;
            require(observed.evidence_digest == previous.evidence_digest && previous.instant == observed.instant &&
                previous.atomic == observed.atomic, "pi_usage_receipt_changed");
            require(!previous.record.session_key || !observed.record.session_key ||
                previous.record.session_key == observed.record.session_key, "pi_usage_event_owner_conflict");
            // The host treats even an initially unknown session as part of the
            // immutable receipt identity. New evidence can qualify later receipts.
            observed.record.session_key = previous.record.session_key;
            observed.record.source_path = previous.record.source_path;
        }
    }
}

Cache create(const addon::Usage_scan_request& request, const Native_profile& profile, const Usage_progress& cancelled)
{
    const auto& selection = *request.cache_selection;
    const auto& profile_root = request.context.profile_storage->root;
    addon::Contract_failure failure;
    const bool allocated = addon::validate_usage_cache_allocation(selection.candidate_root, profile_root, &failure);
    require(allocated, failure.reason_code);
    auto receipts = collect_native_usage(request, profile, cancelled);
    if (selection.accepted) merge_accepted(receipts, open_cache(*selection.accepted, request, profile, false, cancelled), cancelled);
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
    const auto snapshot = addon::random_nonce_hex();
    addon::Observation_scope scope;
    scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (request.context.account_context) scope.source_account_key = request.context.account_context->adapter_account_key;
    const addon::Usage_cache_reference provisional{selection.candidate_root, snapshot, std::string(64, '0')};
    auto page = base_page(request, provisional, scope);
    Json serialized = Json::array();
    std::size_t bytes = 0;
    std::uint64_t index = 0;
    for (const auto& [id, receipt] : receipts) {
        (void)id;
        require(!cancelled(), "cancelled");
        auto record = observation(receipt, page);
        if (!addon::append_usage_page_record(page, record, bytes, 3, 256)) {
            require(!page.records.empty(), "pi_usage_record_too_large");
            write_json(root / page_filename(index++), serialized);
            serialized = Json::array();
            page.records.clear();
            bytes = 0;
            require(addon::append_usage_page_record(page, record, bytes, 3, 256), "pi_usage_record_too_large");
        }
        serialized.push_back(serialize_receipt(receipt));
    }
    write_json(root / page_filename(index++), serialized);
    write_json(root / "source.json", json::object({
        {"scope", addon::encode_observation_scope(scope)}, {"profile_key", Json(profile.account_key)},
        {"request", Json(fingerprint(request))}, {"pages", Json(static_cast<std::int64_t>(index))},
    }));
    auto reference = addon::seal_usage_cache_candidate(selection.candidate_root, profile_root, request.context.account_context,
        snapshot, std::string(k_format), cancelled, &failure);
    require(reference.has_value(), failure.reason_code);
    return open_cache(*reference, request, profile, true, cancelled);
}
} // namespace

void handle_usage_scan(cli_addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    auto last = std::chrono::steady_clock::now();
    std::uint64_t examined = 0;
    const auto cancelled = [&] {
        ++examined;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            context.report_progress(examined, std::nullopt, {});
            last = now;
        }
        return context.cancel_requested();
    };
    try {
        const auto profile = resolve_native_profile(*request->context.launch_environment, request->context.working_directory, {}, &failure);
        require(profile.has_value(), failure.reason_code);
        const bool matched = validate_account_profile(request->context.account_context, request->context.launch_environment,
            request->context.working_directory, &failure);
        require(matched, failure.reason_code);
        std::uint64_t index = 0;
        Cache cache;
        if (request->snapshot_id) {
            const auto cursor = std::string_view(*request->cursor);
            const auto separator = cursor.find('.');
            require(separator == 64 && addon::sha256_from_hex(cursor.substr(0, separator)), "pi_usage_cursor_invalid");
            const auto number = cursor.substr(separator + 1);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
            require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && index != 0 &&
                number == std::to_string(index), "pi_usage_cursor_invalid");
            const addon::Usage_cache_reference ref{request->cache_selection->candidate_root, *request->snapshot_id,
                std::string(cursor.substr(0, separator))};
            cache = open_cache(ref, *request, *profile, true, cancelled);
        }
        else cache = create(*request, *profile, cancelled);
        require(index < cache.pages, "pi_usage_cursor_invalid");
        auto page = base_page(*request, cache.reference, cache.scope);
        auto entries = read_page(cache, index, cancelled);
        for (const auto& raw : entries.as_array()) {
            require(!cancelled(), "cancelled");
            page.records.push_back(observation(parse_receipt(raw), page));
        }
        page.complete = index + 1 == cache.pages;
        if (!page.complete) page.next_cursor = cache.reference.manifest_sha256_hex + "." + std::to_string(index + 1);
        context.succeed(addon::encode_usage_scan_result(page));
    }
    catch (const fs::filesystem_error&) {
        context.fail(addon::Result_status::UNAVAILABLE, "pi_usage_storage_unavailable");
    }
    catch (const std::runtime_error& error) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : error.what());
    }
}

void handle_usage_session(cli_addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_session_describe_operation) { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto input = addon::decode_usage_session_request(context.request().payload, &failure);
    if (!input) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    std::vector<addon::Usage_session_description> descriptions;
    for (const auto& item : *input) {
        std::optional<addon::Usage_session_fact> fact;
        if (receipt_id(item.record_id) && item.session_key && item.session_key->starts_with(k_session_prefix) &&
            item.session_key->size() > k_session_prefix.size()) {
            // Pi persists individual calls while tools can still be running in the same turn.
            fact = addon::Usage_session_fact{item.session_key->substr(k_session_prefix.size()), false};
        }
        descriptions.push_back({item, std::move(fact)});
    }
    context.succeed(addon::encode_usage_session_result(descriptions));
}
} // namespace logonomic::pi
