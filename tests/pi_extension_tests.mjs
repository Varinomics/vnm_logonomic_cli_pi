import assert from "node:assert/strict";
import { resolve } from "node:path";
import { test } from "node:test";
import logonomic_pi from "../package/assets/logonomic-pi.mjs";
import { create_fixture } from "./pi_test_support.mjs";

// Oracles: Pi's extension event contract and the SDK's authenticated helper ingress contract.
function extension_host()
{
    const handlers = new Map();
    const tools = new Map();
    let thinking = "off";
    logonomic_pi({
        on(name, handler) { handlers.set(name, handler); },
        registerTool(tool) { tools.set(tool.name, tool); },
        getThinkingLevel() { return thinking; },
    });
    const ctx = {
        cwd: "/project with spaces",
        signal: undefined,
        getContextUsage() { return { tokens: 24, contextWindow: 100, percent: 24 }; },
        sessionManager: {
            getSessionId() { return "pi-conversation-a"; },
            getSessionFile() { return "/sessions/conversation-a.jsonl"; },
        },
    };
    return {
        handlers, tools, ctx,
        set_thinking(level) { thinking = level; },
        emit(name, event = {}) { return handlers.get(name)?.(event, ctx); },
    };
}

async function in_fixture(run, options)
{
    const fixture = await create_fixture(options);
    const previous = process.env.LOGONOMIC_PROVIDER_HELPERS_DIR;
    process.env.LOGONOMIC_PROVIDER_HELPERS_DIR = fixture.helpers;
    try {
        await run(fixture);
    }
    finally {
        if (previous === undefined) {
            delete process.env.LOGONOMIC_PROVIDER_HELPERS_DIR;
        }
        else {
            process.env.LOGONOMIC_PROVIDER_HELPERS_DIR = previous;
        }
        await fixture.dispose();
    }
}

test("native hooks preserve one turn through retries and pair asynchronous prompts", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        const context = await host.emit("before_agent_start", { messageType: "user" });
        assert.equal(context.message.display, false);
        assert.match(context.message.content, /logonomic_set_title/);
        await host.emit("agent_start");
        await host.emit("message_end", { message: { role: "assistant", stopReason: "error" } });
        await host.emit("agent_end");
        assert.equal((await fixture.records()).filter((record) => record.input.name === "agent_settled").length, 0);
        await host.emit("agent_start");
        const opening = host.emit("ui_prompt_start", { reason: "ui_prompt" });
        const closing = host.emit("ui_prompt_end");
        await Promise.all([opening, closing]);
        await host.emit("message_end", { message: { role: "assistant", stopReason: "stop" } });
        await host.emit("agent_settled", { aborted: false });
        await host.emit("session_shutdown", { reason: "quit" });

        const events = (await fixture.records()).filter((record) => record.args[0] === "event");
        assert.deepEqual(events.map((record) => record.input.name), [
            "session_start", "agent_start", "ui_prompt_start", "ui_prompt_end", "model_snapshot", "agent_settled",
            "session_shutdown",
        ]);
        assert.equal(events[5].input.body.outcome, "succeeded");
        assert.equal(events[5].input.body.turn_id, events[1].input.body.turn_id);
        assert.ok(events[2].input.body.attention_id);
        assert.equal(events[2].input.body.attention_id, events[3].input.body.attention_id);
        assert.equal(new Set(events.map((record) => record.input.transition_nonce)).size, 1);
        assert.match(events[0].input.transition_nonce, /^[0-9a-f]{32}$/);
        assert.equal(new Set(events.map((record) => record.input.idempotency_key)).size, events.length);
        for (const { input } of events) {
            assert.equal(input.idempotency_key, input.body.event_id);
            assert.equal(input.body.session_id, "pi-conversation-a");
            assert.equal(input.body.cwd, "/project with spaces");
        }
    });
});

test("root model observations preserve source order, identity loss and independently observed effort", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        host.ctx.model = { id: "coder-1", provider: "lab", api: "openai-completions" };
        const start = host.emit("session_start", { reason: "startup" });
        host.set_thinking("high");
        const level = host.emit("thinking_level_select");
        host.ctx.model = { ...host.ctx.model, id: "router", api: "pi-virtual" };
        const virtual = host.emit("model_select");
        host.ctx.model = { ...host.ctx.model, id: "coder-2", api: "openai-completions" };
        host.set_thinking(undefined);
        const physical = host.emit("model_select");
        await Promise.all([start, level, virtual, physical]);
        const updates = (await fixture.records()).map((record) => record.input.body.model_state.updates[0]);
        assert.deepEqual(updates.map(({ sequence, model, reasoning_effort }) => [sequence, model, reasoning_effort]), [
            ["0", "coder-1", "off"], ["1", "coder-1", "high"], ["2", "", undefined], ["3", "coder-2", undefined],
        ]);
        assert.equal(updates[2].state, "unknown");
        assert.equal(updates[2].provider, "");
        assert.ok(updates.every((update) => update.provider_session_id === "pi-conversation-a" &&
            update.agent_id === "" && update.transition_nonce === updates[0].transition_nonce));
    });
});

test("native snapshots capture current occupancy and cwd before queued delivery", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        const start = host.emit("session_start", { reason: "startup" });
        host.ctx.cwd = "/project with spaces/nested";
        host.ctx.getContextUsage = () => ({ tokens: 50, contextWindow: 200, percent: 25 });
        const selected = host.emit("model_select");
        host.ctx.cwd = "/later";
        host.ctx.getContextUsage = () => ({ tokens: null, contextWindow: 200, percent: null });
        const compacted = host.emit("session_compact", { compactionEntry: { id: "repeated-summary-id" } });
        await Promise.all([start, selected, compacted]);
        const events = (await fixture.records()).map((record) => record.input);
        assert.deepEqual(events.map((event) => event.name), ["session_start", "context_snapshot", "context_snapshot"]);
        assert.deepEqual(events.map((event) => event.body.execution_context.updates[0].sequence), ["0", "1", "2"]);
        assert.deepEqual(events.map((event) => event.body.execution_context.updates[0].cwd), [
            "/project with spaces", "/project with spaces/nested", "/later",
        ]);
        assert.deepEqual(events.map((event) => event.body.context_updates[0].used_tokens), [24, 50, undefined]);
        assert.equal(events[1].body.context_updates[0].context_window, 200);
        assert.equal(events[2].body.context_updates[0].usage_unavailable, true);
        for (const event of events) {
            const usage = event.body.context_updates[0];
            const execution = event.body.execution_context.updates[0];
            assert.equal(usage.provider_session_id, event.body.session_id);
            assert.equal(usage.event_id, event.body.event_id);
            assert.equal(execution.provider_session_id, event.body.session_id);
            assert.equal(execution.transition_nonce, event.transition_nonce);
            assert.equal(execution.state, "confirmed");
            assert.equal(execution.environment, "local");
            assert.equal(usage.compacted, undefined);
            assert.equal(usage.compaction_count, undefined);
        }
    });
});

test("optional native observation failures cannot prevent lifecycle delivery", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        host.ctx.getContextUsage = () => { throw new Error("native context unavailable"); };
        Object.defineProperty(host.ctx, "cwd", { get() { throw new Error("native cwd unavailable"); } });
        await host.emit("session_start", { reason: "startup" });
        await host.emit("agent_start");
        await host.emit("message_end", { message: { role: "assistant", stopReason: "error" } });
        await host.emit("agent_settled", { aborted: false });
        const events = (await fixture.records()).map((record) => record.input);
        assert.deepEqual(events.map((event) => event.name), ["session_start", "agent_start", "agent_settled"]);
        assert.equal(events[2].body.outcome, "failed");
        for (const event of [events[0], events[2]]) {
            assert.equal(event.body.context_updates, undefined);
            assert.equal(event.body.context_failure.provider_session_id, event.body.session_id);
            assert.equal(event.body.execution_context.updates[0].state, "unknown");
            assert.equal(event.body.execution_context.updates[0].cwd, "");
        }
    });
});

test("failed compaction refreshes native usage without claiming installed compaction", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        await host.emit("session_compact_failed", { reason: "aborted", message: "Cancelled" });
        await host.emit("session_shutdown", { reason: "reload" });
        await host.emit("session_start", { reason: "reload" });
        const events = (await fixture.records()).map((record) => record.input);
        assert.equal(events[1].body.context_updates[0].used_tokens, 24);
        assert.equal(events[1].body.context_updates[0].compacted, undefined);
        assert.equal(events[1].body.execution_context.updates[0].source, "pi.session_compact_failed");
        assert.notEqual(events[2].transition_nonce, events[0].transition_nonce);
        assert.equal(events[2].body.execution_context.updates[0].sequence, "0");
    });
});

test("native failures, cancellation and missing completion evidence stay distinct", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        for (const [stop_reason, aborted, expected] of [
            ["error", false, "failed"],
            ["stop", true, "interrupted"],
            ["toolUse", false, "none"],
        ]) {
            await host.emit("agent_start");
            await host.emit("message_end", { message: { role: "assistant", stopReason: stop_reason } });
            await host.emit("agent_settled", { aborted });
            const settled = (await fixture.records()).filter((record) => record.input.name === "agent_settled");
            assert.equal(settled.at(-1).input.body.outcome, expected);
        }
        const events = (await fixture.records()).filter((record) => record.input.name === "agent_start");
        assert.equal(new Set(events.map((record) => record.input.body.turn_id)).size, 3);
    });
});

test("reload rebinds activation without ending the terminal session", async () => {
    await in_fixture(async (fixture) => {
        const old_host = extension_host();
        await old_host.emit("session_start", { reason: "startup" });
        await old_host.emit("session_shutdown", { reason: "reload" });
        const new_host = extension_host();
        await new_host.emit("session_start", { reason: "reload" });
        await new_host.emit("session_start", { reason: "reload" });
        await new_host.emit("session_shutdown", { reason: "new" });
        const next_host = extension_host();
        next_host.ctx.sessionManager.getSessionId = () => "pi-conversation-b";
        await next_host.emit("session_start", { reason: "new" });
        const events = await fixture.records();
        assert.deepEqual(events.map((record) => record.input.name), ["session_start", "session_start", "session_start"]);
        assert.equal(new Set(events.map((record) => record.input.transition_nonce)).size, 3);
        assert.equal(events[2].input.body.session_id, "pi-conversation-b");
    });
});

test("native tools forward arguments without a shell and surface helper refusal", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        const title = host.tools.get("logonomic_set_title");
        const files = host.tools.get("logonomic_show_changed_files");
        const result = await title.execute("title-call", { title: "Fix π; $(unchanged)" });
        assert.equal(result.content[0].type, "text");
        assert.deepEqual(result.details, {});
        const paths = ["source with spaces.cpp", "-dash.txt"];
        await files.execute("files-call", { paths }, undefined, undefined, host.ctx);
        const records = await fixture.records();
        assert.deepEqual(records[0], { args: ["control", "set-title"], input: { title: "Fix π; $(unchanged)" } });
        assert.deepEqual(records[1], {
            args: ["control", "show-changed-files"],
            input: { paths: paths.map((path) => resolve(host.ctx.cwd, path)) },
        });
        await assert.rejects(title.execute("refused-call", { title: "reject-title" }), /fixture refusal/);
        await assert.rejects(title.execute("large-call", { title: "large-response" }), /output limit/);
        await assert.rejects(title.execute("input-call", { title: "x".repeat(256 * 1024) }), /input limit/);
    });
});

test("cancelling a native tool closes input and waits for helper cleanup", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        const tool = host.tools.get("logonomic_set_title");
        const controller = new AbortController();
        const request = tool.execute("cancel-call", { title: "slow-title" }, controller.signal);
        const rejection = assert.rejects(request, /cancelled/);
        await fixture.wait_for_record((record) => record.input.title === "slow-title");
        controller.abort();
        await rejection;
        await assert.rejects(tool.execute("already-cancelled", { title: "unused" }, controller.signal), /cancelled/);
        const records = await fixture.records();
        assert.equal(records.length, 2);
        assert.deepEqual(records[1], { args: ["cancelled"], input: {} });
    });
});

test("a rejected lifecycle event is visible and does not poison later delivery", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        await assert.rejects(host.emit("ui_prompt_start", { reason: "reject-event" }), /fixture refusal/);
        await host.emit("session_shutdown", { reason: "quit" });
        assert.equal((await fixture.records()).length, 3);
    });
});

test("host context refusal leaves the native turn running without injecting an instruction", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        assert.equal(await host.emit("before_agent_start"), undefined);
        const events = (await fixture.records()).filter((record) => record.args[0] === "event");
        assert.deepEqual(events.map((record) => record.input.name), ["session_start", "agent_start"]);
    }, { title_refused: true });
});

test("a title instruction arriving after activation shutdown cannot enter the replacement context", async () => {
    await in_fixture(async (fixture) => {
        const host = extension_host();
        await host.emit("session_start", { reason: "startup" });
        const instruction = host.emit("before_agent_start");
        await fixture.wait_for_record((record) => record.args[1] === "title-instruction");
        await host.emit("session_shutdown", { reason: "reload" });
        assert.equal(await instruction, undefined);
    }, { title_delay_ms: 150 });
});
