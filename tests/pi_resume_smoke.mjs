import assert from "node:assert/strict";
import { execFile, spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { create_sdk_fixture } from "./pi_sdk_fixture.mjs";
import { executable } from "./pi_test_support.mjs";

const [cli_argument, package_argument, test_binary] = process.argv.slice(2);
if (!cli_argument || !package_argument || !test_binary) {
    throw new Error("Usage: node tests/pi_resume_smoke.mjs <Pi bundled CLI> <package> <pi_session_tests>");
}
const cli = resolve(cli_argument);
const package_root = resolve(package_argument);
const { SessionManager } = await import(pathToFileURL(resolve(dirname(cli), "../core/session-manager.js")).href);
const fixture = await create_sdk_fixture(join(package_root, "helpers", executable("pi_helper")));
let child;
let closed;
let stderr = "";
const replies = new EventEmitter();
const pending = new Map();

function command(type)
{
    const id = `resume-${pending.size + 1}-${type}`;
    return new Promise((resolve_reply, reject) => {
        const timer = setTimeout(() => finish(new Error(`Native Pi ${type} timed out: ${stderr}`)), 20_000);
        function finish(error, value)
        {
            clearTimeout(timer);
            replies.off(id, receive);
            pending.delete(id);
            if (error) {
                reject(error);
            }
            else {
                resolve_reply(value);
            }
        }
        function receive(record)
        {
            finish(record.success ? undefined : new Error(JSON.stringify(record)), record.data);
        }
        pending.set(id, finish);
        replies.on(id, receive);
        child.stdin.write(`${JSON.stringify({ id, type })}\n`);
    });
}

try {
    const profile = join(fixture.directory, "profile");
    const session_directory = join(fixture.directory, "native sessions ");
    await mkdir(profile);
    await writeFile(join(profile, "settings.json"), JSON.stringify({
        retry: { enabled: false }, compaction: { enabled: false },
        enableInstallTelemetry: false, enableAnalytics: false,
    }));
    await writeFile(join(profile, "models.json"), JSON.stringify({ providers: {
        "logonomic-fixture": {
            baseUrl: "http://127.0.0.1:1/v1", api: "openai-completions", apiKey: "unused-local-fixture",
            models: [{ id: "fixture-model", contextWindow: 32_000, maxTokens: 1_024 }],
        },
    } }));

    // The installed native writer is the format oracle, including branched
    // ancestry. No model call or hand-authored JSONL creates this source.
    const native = SessionManager.create(fixture.directory, session_directory);
    const user = native.appendMessage({ role: "user", content: "Native retained question λ", timestamp: Date.now() });
    const answer = (text) => ({
        role: "assistant", content: [{ type: "text", text }], api: "openai-completions",
        provider: "logonomic-fixture", model: "fixture-model", stopReason: "stop", timestamp: Date.now(),
        usage: { input: 1, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 2,
            cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } },
    });
    native.appendMessage(answer("Abandoned native answer"));
    native.branch(user);
    native.appendMessage(answer("Selected native answer ✓"));
    const session_file = native.getSessionFile();
    const session_id = native.getSessionId();
    const proposal_file = join(fixture.directory, "resume-proposal.json");
    await promisify(execFile)(resolve(test_binary), [
        "--native-resume", package_root, process.execPath, cli, session_file, session_id,
        fixture.directory, proposal_file,
    ], { timeout: 30_000 });
    const proposal = JSON.parse(await readFile(proposal_file, "utf8"));
    assert.equal(proposal.process.executable, "helpers/" + executable("pi_launch_bridge"));
    assert.deepEqual(proposal.process.argv.slice(0, 2), [process.execPath, cli]);

    // The production proposal and stock SDK bridge are used unchanged.
    // RPC is appended only to observe the native restored session through a
    // deterministic public transport; this does not qualify the desktop TUI.
    child = spawn(join(package_root, proposal.process.executable), [...proposal.process.argv, "--mode", "rpc"], {
        cwd: fixture.directory,
        env: { PATH: process.env.PATH, HOME: fixture.directory, LANG: "C.UTF-8",
            ...(process.platform === "win32" && { USERPROFILE: fixture.directory, SystemRoot: process.env.SystemRoot,
                TMP: fixture.directory, TEMP: fixture.directory }),
            PI_CODING_AGENT_DIR: profile, ...fixture.env },
        stdio: ["pipe", "pipe", "pipe"],
    });
    closed = once(child, "close");
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => { stderr += chunk; });
    let remaining = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
        remaining += chunk;
        let newline;
        while ((newline = remaining.indexOf("\n")) !== -1) {
            const line = remaining.slice(0, newline);
            remaining = remaining.slice(newline + 1);
            const value = JSON.parse(line);
            if (value.type === "response") {
                replies.emit(value.id, value);
            }
        }
    });
    child.on("error", (error) => {
        for (const finish of [...pending.values()]) {
            finish(error);
        }
    });
    const state = await command("get_state");
    assert.equal(state.sessionId, session_id);
    assert.equal(state.sessionFile, session_file);
    assert.equal(state.model.id, "fixture-model");
    const messages = JSON.stringify((await command("get_messages")).messages);
    assert.match(messages, /Native retained question λ/);
    assert.match(messages, /Selected native answer ✓/);
    assert.doesNotMatch(messages, /Abandoned native answer|original prompt/);
    child.stdin.end();
    assert.equal((await closed)[0], 0, stderr);
    const lifecycle = (await fixture.records()).map((record) => record.input);
    const start = lifecycle.find((event) => event.name === "session_start");
    assert.ok(start, "The real packaged extension and SDK helper observe the resumed native session.");
    assert.equal(start.body.session_id, session_id);
    assert.equal(start.body.session_file, session_file);
    assert.deepEqual(JSON.parse(await readFile(join(fixture.directory, "ledger.json"), "utf8")).pending, []);
    console.log("Passed native Pi persistence → SDK resume preparation → packaged bridge → native restore smoke.");
}
finally {
    if (child && child.exitCode === null) {
        child.kill("SIGTERM");
        await closed;
    }
    await fixture.dispose();
}
