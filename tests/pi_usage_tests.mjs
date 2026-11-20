import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { appendFile, mkdir, mkdtemp, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";

const [simulator, provider] = process.argv.slice(2);
const root = await realpath(await mkdtemp(join(tmpdir(), "logonomic-pi-receipts-")));
const managed = join(root, "managed");
const native = join(root, "native");
const workspace = join(root, "workspace");
const entry = join(root, "pi-package", "dist", "cli.js");
const platform = { win32: "windows-x86_64", darwin: "macos-arm64", linux: "linux-x86_64" }[process.platform];
const env = { PI_CODING_AGENT_DIR: native, PATH: dirname(process.execPath) };
const time = "2026-11-18T08:00:00.000Z";
let sequence = 0;

async function invoke(capability_id, version, operation, payload, success = true) {
    const file = join(root, "host.json");
    await writeFile(file, JSON.stringify({ script_version: 1, steps: [
        { op: "invoke", id: "accounting", capability_id, version, operation, payload, deadline_ms: 10000 },
        { op: "wait_result", id: "accounting", timeout_ms: 11000 }, { op: "shutdown", grace_ms: 2000 },
    ] }));
    const trace = execFileSync(simulator, [provider, "--script", file], { encoding: "utf8", timeout: 20000, maxBuffer: 12 * 1024 * 1024 });
    const result = trace.split("\n").filter(line => line.startsWith("< ")).map(line => JSON.parse(line.slice(2))).find(v => v.type === "result");
    assert.ok(result, trace);
    if (success) assert.equal(result.status, "success", JSON.stringify(result));
    else assert.notEqual(result.status, "success", JSON.stringify(result));
    return success ? result.payload : result;
}

function launcher(args = []) {
    const argv = [process.execPath, entry, ...args];
    return { launcher: { script_body: argv.map(word => '"' + word + '"').join(" "), runner_key: "direct", environment_text: "" },
        launcher_argv: argv, working_directory: workspace, declared_assets: [],
        launch_environment: { platform, effective_environment: env }, profile_storage: { version: 1, root: managed } };
}
async function request(accepted = null, options = {}) {
    const candidate = join(managed, "candidate-" + (++sequence));
    await mkdir(candidate, { recursive: true });
    return { ...launcher(), mode: "incremental", snapshot_id: null, cursor: null,
        cache_selection: { version: 1, candidate_root: candidate, accepted }, ...options };
}
async function scan(payload, expected = true) {
    return invoke("usage-scan", 3, "scan", payload, expected);
}
function header(id, parentSession) {
    return { type: "session", version: 3, id, parentSession, cwd: workspace, timestamp: time };
}
function assistant(id, total) {
    return { type: "message", id, parentId: null, timestamp: time, message: {
        role: "assistant", content: [{ type: "text", text: "fixture" }], provider: "vendor", model: "alias",
        responseModel: "served", responseId: "reused", timestamp: 1000, stopReason: "stop",
        usage: { input: total - 6, output: 2, cacheRead: 3, cacheWrite: 1, totalTokens: total },
    } };
}
function jsonl(rows) { return rows.map(row => JSON.stringify(row) + "\n").join(""); }
async function journal(path, records) {
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, jsonl(records));
}
try {
    await mkdir(dirname(entry), { recursive: true });
    await writeFile(join(dirname(dirname(entry)), "package.json"), JSON.stringify({
        name: "@earendil-works/pi-coding-agent", version: "1.5.2", bin: { pi: "dist/cli.js" },
    }));
    await writeFile(entry, "#!/usr/bin/env node\n");
    await mkdir(workspace);
    const original = join(native, "sessions", "--project--", "original.jsonl");
    const fork = join(dirname(original), "fork.jsonl");
    const first = assistant("a", 12);
    const changed = assistant("b", 23);
    const aux = { type: "usage", id: "c", parentId: "b", timestamp: time, kind: "cache_warm", provider: "vendor", model: "cache",
        usage: { input: 2, cacheRead: 3, output: 1, cacheWrite: 0, totalTokens: 6 } };
    await journal(original, [header("original"), first, changed, aux]);
    await journal(join(dirname(original), "legacy.jsonl"), [{ ...header("old"), version: 1 }, first]);
    await journal(join(dirname(original), "another-logger.jsonl"), [{ type: "diagnostic", id: "log" }]);
    const tool = { type: "message", id: "tool", parentId: "a", timestamp: time, message: { role: "toolResult", usage: { totalTokens: 9 } } };
    const summary = { type: "compaction", id: "summary", parentId: "tool", timestamp: time, usage: { totalTokens: 3 }, tokensBefore: 999 };
    await journal(fork, [header("fork", original), { ...first, parentId: null }, tool, summary, assistant("d", 44)]);
    const initial = await scan(await request());
    assert.ok(initial.complete && initial.incomplete_historical_coverage);
    assert.equal(initial.records.length, 6);
    assert.equal(initial.records.reduce((total, row) => total + row.total_tokens, 0), 97);
    const receipt = initial.records.find(row => row.total_tokens === 12);
    assert.equal(receipt.model, "vendor/served");
    assert.deepEqual([receipt.input.value, receipt.output.value, receipt.cache_read.value, receipt.cache_creation.value], [6, 2, 3, 1]);
    assert.equal(receipt.capabilities["usage-temporal"].portions[0].instant, time);
    assert.ok(receipt.capabilities["usage-cross-source-event"]);
    const aggregate = initial.records.find(row => row.total_tokens === 9);
    assert.ok(!aggregate.model && !aggregate.capabilities["usage-cross-source-event"]);
    for (const row of initial.records.filter(row => row.total_tokens === 9 || row.total_tokens === 3)) {
        const temporal = row.capabilities["usage-temporal"];
        assert.deepEqual(temporal.portions, []);
        assert.equal(temporal.unavailable_tokens, row.total_tokens);
        assert.equal(temporal.coverage_reason, "missing-evidence");
    }
    assert.equal(aggregate.input.coverage.support, "unknown");
    assert.equal(aggregate.input.value, undefined);
    const facts = await invoke("usage-session", 1, "describe", { records: initial.records.map(({record_id, session_key}) => ({record_id, session_key})) });
    assert.deepEqual(facts.records.map(row => row.session.provider_session_id).sort(), ["fork", "fork", "fork", "original", "original", "original"]);
    assert.ok(facts.records.every(row => row.session.excludes_turn_in_flight === false));
    const account = async (environment) => {
        const { accounts: [selected] } = await invoke("accounts", 2, "discover", {
            launcher: launcher().launcher, launch_environment: environment,
            normalization_directory: workspace, default_home_fallback: root,
        });
        return { version: 2, adapter_account_key: selected.adapter_account_key, profile_path: selected.profile_path };
    };
    const second = join(root, "second-profile");
    const second_env = { platform, effective_environment: { ...env, PI_CODING_AGENT_DIR: second } };
    const first_account = await account(launcher().launch_environment);
    const second_account = await account(second_env);
    await journal(join(second, "sessions", "--elsewhere--", "copy.jsonl"), [header("original"), first]);
    const selected = await scan(await request(null, { account_context: first_account }));
    const other = await scan(await request(null, { account_context: second_account, launch_environment: second_env }));
    assert.deepEqual(selected.account_context, first_account);
    assert.equal(other.scope.source_account_key, second_account.adapter_account_key);
    assert.equal(other.records.length, 1);
    assert.deepEqual(other.records[0].capabilities["usage-cross-source-event"], receipt.capabilities["usage-cross-source-event"]);
    await scan(await request(null, { launch_environment: second_env, account_context: first_account }), false);
    await scan(await request(selected.cache_candidate, { launch_environment: second_env, account_context: second_account }), false);
    await rm(original);
    const orphan = await scan(await request());
    assert.ok(orphan.records.length === 4 && orphan.records.every(row => !row.session_key));
    await journal(original, [header("original"), first, changed, aux]);
    await appendFile(fork, jsonl([assistant("new-in-fork", 37)]));
    const reconciled = await scan(await request(orphan.cache_candidate));
    assert.equal(reconciled.records.length, 7);
    for (const previous of orphan.records) {
        const now = reconciled.records.find(row => row.record_id === previous.record_id);
        assert.equal(now.session_key, previous.session_key);
        assert.equal(now.source_path, previous.source_path);
    }
    assert.equal(reconciled.records.find(row => row.total_tokens === 37).session_key, "pi-session:fork");
    assert.equal(reconciled.records.find(row => row.total_tokens === 23).session_key, "pi-session:original");
    await rm(original);
    await rm(fork);
    const retained = await scan(await request(initial.cache_candidate));
    assert.deepEqual(retained.records.map(row => row.record_id), initial.records.map(row => row.record_id));
    assert.equal((await scan(await request(null, { mode: "reset" }))).records.length, 0);

    const external = join(root, "custom");
    const bulk = Array.from({ length: 263 }, (_, i) => assistant("entry" + i, 10 + i));
    await journal(join(external, "custom.jsonl"), [header("bulk"), ...bulk]);
    await mkdir(join(workspace, ".pi"));
    await writeFile(join(workspace, ".pi", "settings.json"), JSON.stringify({ sessionDir: external }));
    const paging = await request();
    const page = await scan(paging);
    assert.ok(!page.complete && page.records.length === 256);
    const continuation = { ...paging, snapshot_id: page.snapshot_id, cursor: page.next_cursor };
    await appendFile(join(external, "custom.jsonl"), jsonl([assistant("arrived-later", 30)]));
    const final = await scan(continuation);
    assert.deepEqual(await scan(continuation), final);
    assert.equal(final.records.length, 7);
    assert.ok(final.complete && page.records.at(-1).record_id < final.records[0].record_id);
    await scan({ ...continuation, mode: "repair" }, false);
    const foreign = await request(page.cache_candidate);
    const wrong = { ...foreign, launch_environment: { platform, effective_environment: { ...env, PI_CODING_AGENT_DIR: join(root, "other") } } };
    await scan(wrong, false);
    await writeFile(join(page.cache_candidate.root, "page-0.json"), "[]\n");
    await scan(await request(page.cache_candidate), false);

    await rm(join(workspace, ".pi", "settings.json"));
    const authored = { ...launcher(["--session-dir", external]) };
    const custom_request = await request(null, authored);
    const custom = await scan(custom_request);
    const rest = await scan({ ...custom_request, snapshot_id: custom.snapshot_id, cursor: custom.next_cursor });
    assert.equal(custom.records.length + rest.records.length, 264);
    const exact = join(root, "explicit.external");
    await journal(exact, [header("exact"), assistant("exact-call", 17)]);
    assert.equal((await scan(await request(null, launcher(["--session", exact])))).records[0].total_tokens, 17);
    const missing = { ...assistant("missing-category", 34) };
    delete missing.message.usage.cacheRead;
    await journal(exact, [header("exact"), missing]);
    assert.equal((await scan(await request(null, launcher(["--session", exact])))).records[0].cache_read.coverage.covered_records, 0);
    const placeholder = assistant("absent-usage", 6);
    placeholder.message.api = "openai-completions";
    placeholder.message.usage = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
        cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
    const normalized = structuredClone(placeholder);
    normalized.id = "empty-or-zero";
    normalized.message.usage.reasoning = 0;
    normalized.message.usage.cost.totalFromProvider = false;
    const partial = assistant("unmetered", 12);
    delete partial.message.usage.totalTokens;
    delete partial.message.usage.cacheRead;
    const fallback = assistant("known-components", 27);
    fallback.message.usage.totalTokens = 0;
    await journal(exact, [header("exact"), placeholder, normalized, partial, fallback, missing]);
    const measured = await scan(await request(null, launcher(["--session", exact])));
    assert.ok(measured.incomplete_historical_coverage);
    assert.deepEqual(measured.records.map(row => row.total_tokens).sort(), [27, 34]);
    console.log("Pi packaged usage verified across native receipt types, forks, selected caches and snapshot pages.");
}
finally {
    await rm(root, { recursive: true, force: true });
}
