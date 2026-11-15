import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { mkdir, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { create_fixture } from "./pi_test_support.mjs";
import { create_sdk_fixture } from "./pi_sdk_fixture.mjs";

// A real Pi runtime is the oracle for loader, JSON Schema tools, events and session replacement.
// Every model response is produced locally; no provider credentials or paid model calls are used.
const cli = process.argv[2];
const helper_binary = process.argv[3];
if (!cli) {
    throw new Error("Usage: node tests/pi_runtime_smoke.mjs /path/to/pi/dist/bundle/cli.js [compiled-pi_helper]");
}
if (process.platform === "win32") {
    throw new Error("This smoke fixture uses a POSIX executable helper; run it on Linux or macOS.");
}

const fixture = helper_binary ? await create_sdk_fixture(helper_binary) : await create_fixture();
const requests = [];
const model_events = new EventEmitter();
let mode = "tools";
const server = createServer(async (request, response) => {
    let bytes = "";
    for await (const chunk of request) {
        bytes += chunk;
    }
    const input = JSON.parse(bytes);
    requests.push(input);
    model_events.emit("request", { mode, input });
    if (mode === "hold") {
        response.writeHead(200, { "Content-Type": "text/event-stream" });
        response.flushHeaders();
        return;
    }
    if (mode === "error") {
        response.writeHead(400, { "Content-Type": "application/json" });
        response.end(JSON.stringify({ error: { message: "Deliberate local fixture error", type: "invalid_request_error" } }));
        return;
    }
    response.writeHead(200, { "Content-Type": "text/event-stream" });
    const use_tools = (mode === "tools" || mode === "cancel-tool") && input.messages.at(-1)?.role !== "tool";
    const delta = use_tools
        ? {
            role: "assistant",
            tool_calls: mode === "cancel-tool" ? [
                { index: 0, id: "call_cancel", type: "function", function: {
                    name: "logonomic_set_title", arguments: JSON.stringify({ title: "slow-title" }),
                } },
            ] : [
                { index: 0, id: "call_title", type: "function", function: {
                    name: "logonomic_set_title", arguments: JSON.stringify({ title: "Pi integration fixture" }),
                } },
                { index: 1, id: "call_files", type: "function", function: {
                    name: "logonomic_show_changed_files", arguments: JSON.stringify({ paths: ["source file.cpp"] }),
                } },
            ],
        }
        : { role: "assistant", content: "Local fixture completed." };
    for (const [chunk, finish_reason] of [[delta, null], [{}, use_tools ? "tool_calls" : "stop"]]) {
        response.write(`data: ${JSON.stringify({
            id: "fixture-completion", object: "chat.completion.chunk", created: 1, model: "fixture-model",
            choices: [{ index: 0, delta: chunk, finish_reason }],
            usage: { prompt_tokens: 12, completion_tokens: 6, total_tokens: 18 },
        })}\n\n`);
    }
    response.end("data: [DONE]\n\n");
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));

let child;
let stderr = "";
let stdout = "";
const records = [];
const events = new EventEmitter();
function wait_for(predicate, after = 0)
{
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => finish(new Error(`Pi response did not arrive. ${stderr}`)), 20_000);
        function finish(error, result)
        {
            clearTimeout(timer);
            events.off("record", inspect);
            if (error) {
                reject(error);
            }
            else {
                resolve(result);
            }
        }
        function inspect()
        {
            const result = records.slice(after).find(predicate);
            if (result) {
                finish(undefined, result);
            }
        }
        events.on("record", inspect);
        inspect();
    });
}

let command_number = 0;
async function command(type, fields = {})
{
    const id = `command-${++command_number}`;
    const response = wait_for((record) => record.type === "response" && record.id === id);
    child.stdin.write(`${JSON.stringify({ id, type, ...fields })}\n`);
    const result = await response;
    assert.equal(result.success, true, JSON.stringify(result));
    return result.data;
}

try {
    const profile = join(fixture.directory, "profile");
    const sessions = join(fixture.directory, "sessions");
    await mkdir(profile);
    await mkdir(sessions);
    await writeFile(join(profile, "settings.json"), JSON.stringify({
        retry: { enabled: false },
        compaction: { enabled: false },
        enableInstallTelemetry: false,
        enableAnalytics: false,
    }));
    await writeFile(join(profile, "models.json"), JSON.stringify({ providers: {
        "logonomic-fixture": {
            baseUrl: `http://127.0.0.1:${server.address().port}/v1`,
            api: "openai-completions",
            apiKey: "local-fixture",
            models: [{ id: "fixture-model", contextWindow: 32_000, maxTokens: 1_024 }],
        },
    } }));
    const commands_extension = join(fixture.directory, "fixture-commands.mjs");
    await writeFile(commands_extension, `export default function (pi) {
        pi.registerCommand("fixture-prompt", { description: "Exercise native prompt events", handler: async (args, ctx) => {
            await ctx.ui.confirm("Fixture prompt", "Continue the local fixture?");
        }});
        pi.registerCommand("fixture-reload", { description: "Exercise native reload", handler: async (args, ctx) => {
            await ctx.reload();
        }});
    }`);
    const extension = resolve(dirname(fileURLToPath(import.meta.url)), "../package/assets/logonomic-pi.mjs");
    child = spawn(process.execPath, [
        resolve(cli), "--offline", "--mode", "rpc", "--no-extensions",
        "--extension", extension, "--extension", commands_extension,
        "--provider", "logonomic-fixture", "--model", "fixture-model", "--thinking", "off",
        "--session-dir", sessions,
    ], {
        cwd: fixture.directory,
        env: { ...process.env, ...fixture.env, PI_CODING_AGENT_DIR: profile, LOGONOMIC_PROVIDER_HELPERS_DIR: fixture.helpers },
        stdio: ["pipe", "pipe", "pipe"],
    });
    const exited = once(child, "close");
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => { stderr += chunk; });
    child.stdout.setEncoding("utf8");
    let remaining = "";
    child.stdout.on("data", (chunk) => {
        stdout += chunk;
        remaining += chunk;
        let newline;
        while ((newline = remaining.indexOf("\n")) !== -1) {
            const line = remaining.slice(0, newline);
            remaining = remaining.slice(newline + 1);
            records.push(JSON.parse(line));
            events.emit("record");
        }
    });

    const initial = await command("get_state");
    const first_settled = wait_for((record) => record.type === "agent_settled");
    await command("prompt", { message: "Exercise both Logonomic tools using the local fixture." });
    await first_settled;
    assert.equal(requests.length, 2, "The fixture requires one tool request and one completion request.");
    assert.ok(requests[0].tools.some((tool) => tool.function.name === "logonomic_set_title"));
    assert.ok(requests[0].tools.some((tool) => tool.function.name === "logonomic_show_changed_files"));
    assert.match(JSON.stringify(requests[0].messages), /Set the Logonomic title with logonomic_set_title/);
    const tool_results = records.filter((record) => record.type === "tool_execution_end");
    assert.equal(tool_results.length, 2);
    assert.ok(tool_results.every((record) => record.isError === false));

    const prompt_request = wait_for((record) => record.type === "extension_ui_request" && record.method === "confirm");
    const prompt_command = command("prompt", { message: "/fixture-prompt" });
    const prompt = await prompt_request;
    child.stdin.write(`${JSON.stringify({ type: "extension_ui_response", id: prompt.id, confirmed: true })}\n`);
    await prompt_command;
    await command("prompt", { message: "/fixture-reload" });
    assert.equal((await command("get_state")).sessionId, initial.sessionId);
    await command("new_session");
    assert.notEqual((await command("get_state")).sessionId, initial.sessionId);
    await command("switch_session", { sessionPath: initial.sessionFile });
    assert.equal((await command("get_state")).sessionId, initial.sessionId);

    mode = "error";
    const error_settled = wait_for((record) => record.type === "agent_settled", records.length);
    await command("prompt", { message: "Exercise a deliberate local provider failure." });
    await error_settled;

    mode = "hold";
    const requested = once(model_events, "request");
    const aborted_settled = wait_for((record) => record.type === "agent_settled", records.length);
    await command("prompt", { message: "Exercise cancellation of the local fixture." });
    await requested;
    await command("abort");
    assert.equal((await aborted_settled).aborted, true);

    if (helper_binary) {
        mode = "cancel-tool";
        const wrapper_started = fixture.wait_for_wrapper((record) => record.args[1] === "slow-title");
        const tool_aborted = wait_for((record) => record.type === "agent_settled", records.length);
        await command("prompt", { message: "Cancel a native tool while the SDK wrapper is running." });
        const wrapper = await wrapper_started;
        await command("abort");
        assert.equal((await tool_aborted).aborted, true);
        assert.throws(() => process.kill(wrapper.pid, 0), { code: "ESRCH" },
            "The SDK helper must terminate and reap vnmctl before the native tool finishes.");
    }

    child.stdin.end();
    assert.equal((await exited)[0], 0, stderr);
    const helper_records = await fixture.records();
    const lifecycle = helper_records.filter((record) => record.args[0] === "event").map((record) => record.input);
    assert.deepEqual(lifecycle.filter((event) => event.name === "session_start").map((event) => event.body.reason), [
        "startup", "reload", "new", "resume",
    ]);
    assert.equal(new Set(lifecycle.filter((event) => event.name === "session_start")
        .map((event) => event.transition_nonce)).size, 4);
    assert.equal(lifecycle.filter((event) => event.name === "session_shutdown").length, 1);
    assert.deepEqual(lifecycle.filter((event) => event.name === "agent_settled").map((event) => event.body.outcome),
        helper_binary ? ["succeeded", "failed", "interrupted", "interrupted"] : ["succeeded", "failed", "interrupted"]);
    const attention = lifecycle.filter((event) => event.name.startsWith("ui_prompt_"));
    assert.deepEqual(attention.map((event) => event.name), ["ui_prompt_start", "ui_prompt_end"]);
    assert.ok(attention[0].body.attention_id);
    assert.equal(attention[0].body.attention_id, attention[1].body.attention_id);
    assert.equal(records.filter((record) => record.type === "extension_error").length, 0);
    process.stdout.write("PASS: real Pi loaded the extension, executed both tools, delivered hidden context, " +
        "paired prompt events, preserved reload/new/resume identity, and distinguished success/error/abort.\n");
    if (helper_binary) {
        const verified = await fixture.verify();
        process.stdout.write(`PASS: compiled pi_helper used the SDK wrapper contract for ${verified.deliveries} ` +
            `lifecycle deliveries and ${verified.acknowledgements} acknowledgements, fenced context, title/files controls, ` +
            "and cancellation with vnmctl cleanup.\n");
    }
}
finally {
    if (child && child.exitCode === null && child.signalCode === null) {
        child.kill("SIGKILL");
        await once(child, "close");
    }
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
    if (process.env.PI_TEST_EVIDENCE_DIR) {
        const directory = resolve(process.env.PI_TEST_EVIDENCE_DIR);
        await mkdir(directory, { recursive: true });
        await writeFile(join(directory, "pi-stdout.jsonl"), stdout);
        await writeFile(join(directory, "pi-stderr.log"), stderr);
        await fixture.save_evidence?.(directory);
    }
    await fixture.dispose();
}
