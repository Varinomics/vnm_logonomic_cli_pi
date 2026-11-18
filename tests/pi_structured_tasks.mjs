import assert from "node:assert/strict";
import { execFile, execFileSync, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { promisify } from "node:util";

const [simulator, package_root, native_cli] = process.argv.slice(2).map(value => resolve(value));
const directory = await mkdtemp(join(tmpdir(), "pi-structured-"));
const profile = join(directory, "selected profile");
const cli = native_cli ?? join(directory, "pi-package", "cli.js");
const windows = process.platform === "win32";
const platform = windows ? "windows-x86_64" : process.platform === "darwin" ? "macos-arm64" : "linux-x86_64";
const prompt = " \nPreserve \"quotes\", λ, and the trailing newline.\n ";
const answer = "Finished in the selected workspace. ✓";
const b64 = value => Buffer.from(value).toString("base64");
const chunks = buffer => Array.from({ length: Math.ceil(buffer.length / 65536) },
    (_, index) => buffer.subarray(index * 65536, (index + 1) * 65536).toString("base64"));
const decode = parts => Buffer.concat(parts.map(part => Buffer.from(part, "base64"))).toString();

async function invoke(operation, payload, cap = "structured-session", version = 3, status = "success")
{
    const script = join(directory, "request.json");
    await writeFile(script, JSON.stringify({ script_version: 1, steps: [
        { op: "invoke", id: "test", capability_id: cap, version, operation, payload, deadline_ms: 10000 },
        { op: "wait_result", id: "test", timeout_ms: 10000 },
        { op: "shutdown", grace_ms: 2000 },
    ] }));
    const trace = execFileSync(simulator, [package_root, "--script", script], {
        encoding: "utf8", timeout: 20000, maxBuffer: 12 * 1024 * 1024,
    });
    const response = trace.split("\n").filter(line => line.startsWith("< ")).map(line => JSON.parse(line.slice(2)))
        .find(frame => frame.type === "result");
    assert.equal(response?.status, status, trace.slice(-2048));
    return response.payload;
}

function inspect_request(state, bytes, final = false, exit_code = 0, timed_out = false)
{
    return { purpose: "implementation", model_state: { version: 1 }, inspection: {
        protocol_state_base64_chunks: state, stdout_base64_chunks: chunks(bytes), stderr_base64_chunks: [],
        final, exit_code, timed_out,
    } };
}

const quote = word => "'" + word.replaceAll("'", "'\\''") + "'";
const env = {
    PATH: dirname(process.execPath), [windows ? "USERPROFILE" : "HOME"]: directory,
    PI_CODING_AGENT_DIR: profile,
    ...(windows && { SystemRoot: process.env.SystemRoot, TEMP: directory, TMP: directory }),
};
const words = [process.execPath, cli, "--offline", "--no-extensions", "--no-session", "--no-tools",
    "--provider", "logonomic-fixture", "--model", "fixture-model", "--thinking", "off"];
function saved_launcher(arguments_)
{
    const script = arguments_.map(quote).join(" ");
    return {
        launcher: { script_body: script, runner_key: "bash", environment_text: "" },
        launcher_argv: [windows ? "bash.exe" : "/bin/bash", process.platform === "linux" ? "-c" : "-lc", script],
    };
}
const context = {
    ...saved_launcher(words),
    working_directory: directory, declared_assets: [], launch_environment: { platform, effective_environment: env },
    purpose: "implementation", prompt_base64: b64(prompt),
};
const records = (...values) => Buffer.from(values.map(value => JSON.stringify(value) + "\n").join(""));
const state_response = (id, messageCount) => ({
    type: "response", command: "get_state", id, success: true, data: {
        sessionId: "test-session", isStreaming: false, isCompacting: false, pendingMessageCount: 0, messageCount,
    },
});

async function contract(prepared)
{
    const starting = prepared.inspection.protocol_state_base64_chunks;
    const inspect = (state, ...events) => invoke("inspect", inspect_request(state, records(...events)));
    const submitted = await inspect(starting, state_response("initial", 0));
    assert.deepEqual(JSON.parse(decode(submitted.stdin_base64_chunks)), { id: "task", type: "prompt", message: prompt });
    const response = { type: "response", id: "task", command: "prompt", success: true, data: { disposition: "started" } };
    assert.ok((await inspect(starting, state_response("initial", 0), response)).failure_reason);
    const running = (await inspect(submitted.protocol_state_base64_chunks, response)).protocol_state_base64_chunks;
    const user = { type: "message_end", message: { role: "user", content: [{ type: "text", text: prompt }] } };
    assert.equal((await inspect(running, { ...user, message: { ...user.message, content: [{ type: "text", text: prompt.trim() }] } })).failure_reason,
        "pi_prompt_mismatch");
    assert.ok((await inspect(starting, state_response("initial", 1))).failure_reason);
    assert.ok((await inspect(submitted.protocol_state_base64_chunks, { ...response, data: { disposition: "queued" } })).failure_reason);
    assert.equal((await inspect(running, { type: "extension_ui_request", method: "confirm" })).failure_reason, "hook_approval_required");
    assert.ok((await invoke("inspect", inspect_request(running, Buffer.alloc(0), true))).failure_reason);
    const bytes = records(user);
    const split = bytes.indexOf(Buffer.from("λ")) + 1;
    const partial = await invoke("inspect", inspect_request(running, bytes.subarray(0, split)));
    const committed = await invoke("inspect", inspect_request(partial.protocol_state_base64_chunks, bytes.subarray(split)));
    const assistant = { role: "assistant", api: "openai-completions", provider: "serving", model: "actual", stopReason: "stop",
        content: [{ type: "text", text: answer }] };
    for (const change of [{ api: "pi-virtual" }, { stopReason: "error" }, { stopReason: "aborted" }]) {
        const unserved = await inspect(committed.protocol_state_base64_chunks, {
            type: "message_end", message: { ...assistant, ...change },
        });
        assert.equal(unserved.model_state, undefined);
    }
    const native = records({ type: "message_end", message: assistant }, { type: "agent_end", messages: [assistant], willRetry: false },
        { type: "agent_settled", aborted: false });
    const settled_request = inspect_request(committed.protocol_state_base64_chunks, native);
    const settled = await invoke("inspect", settled_request);
    assert.deepEqual(settled.model_state.observation, { provider: "serving", model: "actual", source: "pi.message_end" });
    assert.equal(settled.complete, false);
    assert.equal(JSON.parse(decode(settled.stdin_base64_chunks)).id, "finished");
    const timed_out = await invoke("inspect", inspect_request(committed.protocol_state_base64_chunks, native, true, -1, true));
    assert.equal(timed_out.failure_reason, "pi_task_timed_out");
    assert.deepEqual(timed_out.model_state, settled.model_state);
    delete settled_request.model_state;
    assert.equal((await invoke("inspect", settled_request)).model_state, undefined);
    const closed = await inspect(settled.protocol_state_base64_chunks, state_response("finished", 2));
    assert.equal(closed.close_stdin, true);
    const finish = (input, code = 0) => invoke("inspect", inspect_request(closed.protocol_state_base64_chunks, input, true, code));
    assert.equal(decode((await finish(Buffer.alloc(0))).result_base64_chunks), answer);
    assert.ok((await finish(Buffer.from('{"incomplete"'))).failure_reason);
    assert.ok((await finish(Buffer.alloc(0), 7)).failure_reason);
    // Each native read receipt fits one RPC message, but agent_end repeats
    // every receipt in a single record. A tool turn may span many SDK frames.
    const receipts = Array.from({ length: 43 }, (_, index) => ({
        role: "toolResult", toolCallId: "read-" + index, toolName: "read",
        content: [{ type: "text", text: 'bytes "}],{" \\'.repeat(3900) }],
    }));
    const repeated = records({ type: "agent_end", messages: [...receipts, assistant], willRetry: false });
    assert.ok(repeated.length > 2 * 1024 * 1024);
    let state = committed.protocol_state_base64_chunks;
    for (let offset = 0; offset < repeated.length; offset += 320000) {
        const part = await invoke("inspect", inspect_request(state, repeated.subarray(offset, offset + 320000)));
        assert.equal(part.complete, false, part.failure_reason);
        state = part.protocol_state_base64_chunks;
    }
    assert.ok((await inspect(state, { type: "agent_settled", aborted: false })).stdin_base64_chunks);
}

async function native(prepared)
{
    let mode;
    let active_prompt;
    let child;
    let http_error;
    let requests = 0;
    let held_closed;
    const held_marker = join(directory, "held-request");
    const host_command = process.env.PI_TASK_HOST_COMMAND ? JSON.parse(process.env.PI_TASK_HOST_COMMAND) : undefined;
    const server = createServer(async (request, response) => {
        try {
            let bytes = "";
            for await (const chunk of request) bytes += chunk;
            const input = JSON.parse(bytes);
            assert.equal(input.model, "fixture-model");
            const asked = input.messages.find(message => message.role === "user");
            assert.deepEqual(asked.content, [{ type: "text", text: active_prompt }]);
            assert.deepEqual(input.tools?.map(tool => tool.function.name) ?? [], mode === "write" ? ["write"] : []);
            assert.match(JSON.stringify(input.messages[0]), /pi-structured-/i);
            ++requests;
            if (mode === "error") {
                response.writeHead(400, { "Content-Type": "application/json" });
                response.end(JSON.stringify({ error: { message: "Local fixture model failure", type: "invalid_request_error" } }));
                return;
            }
            response.writeHead(200, { "Content-Type": "text/event-stream" });
            if (mode === "cancel") {
                held_closed = once(response, "close");
                response.flushHeaders();
                if (host_command) {
                    await writeFile(held_marker, "");
                } else {
                    // Exercise native shutdown on withdrawal of the SDK bridge's input.
                    child.stdin.end();
                }
                return;
            }
            const use_tool = mode === "write" && input.messages.at(-1).role !== "tool";
            const delta = use_tool ? { role: "assistant", tool_calls: [{ index: 0, id: "fixture-write", type: "function",
                function: { name: "write", arguments: JSON.stringify({ path: "task-output.txt", content: "native write ✓" }) } }] }
                : { role: "assistant", content: answer };
            for (const [part, finish_reason] of [[delta, null], [{}, use_tool ? "tool_calls" : "stop"]]) {
                response.write("data: " + JSON.stringify({ id: "fixture", object: "chat.completion.chunk", created: 1,
                    model: "fixture-model", choices: [{ index: 0, delta: part, finish_reason }] }) + "\n\n");
            }
            response.end("data: [DONE]\n\n");
        } catch (error) {
            http_error = error;
            response.destroy();
        }
    });
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    try {
        await writeFile(join(profile, "settings.json"), JSON.stringify({ retry: { enabled: false }, compaction: { enabled: false },
            enableInstallTelemetry: false, enableAnalytics: false }));
        await writeFile(join(profile, "models.json"), JSON.stringify({ providers: { "logonomic-fixture": {
            baseUrl: "http://127.0.0.1:" + server.address().port + "/v1", api: "openai-completions", apiKey: "local-fixture",
            models: [{ id: "fixture-model", contextWindow: 32000, maxTokens: 1024 }],
        } } }));
        const classifier = join(directory, "commands.mjs");
        const command_ran = join(directory, "unexpected-command");
        await writeFile(classifier, 'import { writeFileSync } from "node:fs"; export default pi => pi.registerCommand("fixture-command", ' +
            '{ description: "Local test command", handler: () => writeFileSync(' + JSON.stringify(command_ran) + ', "ran") });');
        for (mode of (host_command ? ["success", "error", "cancel"] : ["success", "error", "cancel", "write", "command"])) {
            active_prompt = mode === "write" ? "/tmp/example.cpp needs a wording change." :
                mode === "command" ? "/fixture-command should never execute" : prompt;
            if (host_command) {
                const scenario = join(directory, mode + ".json");
                await writeFile(scenario, JSON.stringify({
                    mode, package: package_root, launcher_argv: words, cwd: directory, prompt,
                    environment: env, account_context: context.account_context, expected_result: answer,
                    ...(mode === "cancel" && { held_request_marker: held_marker }),
                }));
                await promisify(execFile)(host_command[0], host_command.slice(1), {
                    env: { ...process.env, LOGONOMIC_TEST_NATIVE_STRUCTURED_TASK: scenario },
                    timeout: 75000, windowsHide: true,
                });
                if (http_error) throw http_error;
                continue;
            }
            let plan = prepared;
            if (mode === "write" || mode === "command") {
                const selected = mode === "write" ? words.flatMap(word => word === "--no-tools" ? ["--tools", "write"] : [word])
                    : [...words, "--extension", classifier];
                plan = await invoke("prepare", { ...context, ...saved_launcher(selected), prompt_base64: b64(active_prompt) });
            }
            const proposal = plan.process;
            child = spawn(join(package_root, proposal.executable), proposal.argv, { cwd: directory, windowsHide: true,
                env: Object.fromEntries(proposal.environment_names.map(name => [name, env[name]])), stdio: ["pipe", "pipe", "pipe"] });
            let stderr = "";
            child.stderr.on("data", bytes => { stderr += bytes; });
            const exit = once(child, "close");
            let state = plan.inspection.protocol_state_base64_chunks;
            let result;
            let observation;
            const deadline = setTimeout(() => child.stdin.destroy(), 30000);
            try {
                child.stdin.write(proposal.stdin_payload);
                for await (const bytes of child.stdout) {
                    if (result?.complete) continue;
                    const request = inspect_request(state, bytes);
                    result = await invoke("inspect", request);
                    if (result.model_state) {
                        observation = result.model_state.observation;
                        if (mode === "success" && process.env.PI_TASK_EVIDENCE_DIRECTORY) {
                            await writeFile(join(process.env.PI_TASK_EVIDENCE_DIRECTORY, "native-pi-inspect.json"), JSON.stringify(request));
                        }
                    }
                    if (result.stdin_base64_chunks && !child.stdin.writableEnded) child.stdin.write(decode(result.stdin_base64_chunks));
                    if (result.close_stdin || result.complete) child.stdin.end();
                    state = result.protocol_state_base64_chunks;
                }
                const [code] = await exit;
                if (!result?.complete) result = await invoke("inspect", inspect_request(state, Buffer.alloc(0), true, code ?? -1));
                if (http_error) throw http_error;
                if (mode === "success" || mode === "write") {
                    assert.equal(result.failure_reason, undefined, stderr);
                    assert.equal(decode(result.result_base64_chunks), answer);
                    assert.deepEqual(observation, { provider: "logonomic-fixture", model: "fixture-model", source: "pi.message_end" });
                    if (mode === "write") assert.equal(await readFile(join(directory, "task-output.txt"), "utf8"), "native write ✓");
                } else {
                    assert.ok(result.failure_reason, mode);
                    assert.equal(result.result_base64_chunks, undefined);
                    if (mode === "error") {
                        assert.equal(result.failure_reason, "pi_model_error");
                        assert.equal(observation, undefined);
                    }
                    if (mode === "command") {
                        assert.equal(result.failure_reason, "pi_command_prompt");
                        await assert.rejects(readFile(command_ran), { code: "ENOENT" });
                    }
                }
            } finally {
                clearTimeout(deadline);
                child.stdin.destroy();
                await exit;
            }
        }
        assert.equal(requests, host_command ? 3 : 5);
        assert.ok(held_closed);
        await held_closed;
    } finally {
        server.closeAllConnections();
        await new Promise(resolve_close => server.close(resolve_close));
    }
}

try {
    await mkdir(profile);
    if (!native_cli) {
        await mkdir(dirname(cli));
        await writeFile(cli, "");
        await writeFile(join(dirname(cli), "package.json"), JSON.stringify({ name: "@earendil-works/pi-coding-agent",
            version: "1.5.2", bin: { pi: "cli.js" } }));
    }
    const account = (await invoke("discover", { launcher: context.launcher, launch_environment: context.launch_environment,
        default_home_fallback: directory, normalization_directory: directory }, "accounts", 2)).accounts[0];
    assert.ok(account);
    context.account_context = { version: 2, adapter_account_key: account.adapter_account_key, profile_path: account.profile_path };
    const prepared = await invoke("prepare", context);
    assert.deepEqual(prepared.account_context, context.account_context);
    assert.equal(prepared.process.start, "attached");
    assert.deepEqual(prepared.process.argv.slice(2), [...words.slice(2), "--mode", "rpc"]);
    assert.equal(prepared.inspection.keep_stdin_open, true);
    if (native_cli) {
        await native(prepared);
    } else {
        await contract(prepared);
        for (const change of [{ purpose: "read_only_review" }, { prompt_base64: b64("") },
            { account_context: { ...context.account_context, profile_path: directory } },
            saved_launcher([...words, "--resume"]), saved_launcher([...words, "--thinking", "unsupported"])]) {
            await invoke("prepare", { ...context, ...change }, "structured-session", 3, "unavailable");
        }
        for (const [version, status] of [["1.0.9", "unavailable"], ["1.1.0-rc.1", "unavailable"], ["1.1.0", "success"]]) {
            await writeFile(join(dirname(cli), "package.json"), JSON.stringify({ name: "@earendil-works/pi-coding-agent",
                version, bin: { pi: "cli.js" } }));
            await invoke("prepare", context, "structured-session", 3, status);
        }
    }
} finally {
    await rm(directory, { recursive: true, force: true });
}
