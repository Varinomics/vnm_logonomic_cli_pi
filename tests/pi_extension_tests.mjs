import assert from "node:assert/strict";
import { test } from "node:test";
import logonomic_pi from "../package/assets/logonomic-pi.mjs";
import { create_fixture } from "./pi_test_support.mjs";

// Oracles: Pi's extension event contract and the SDK's authenticated helper ingress contract.
function extension_host()
{
    const handlers = new Map();
    const tools = new Map();
    logonomic_pi({
        on(name, handler) { handlers.set(name, handler); },
        registerTool(tool) { tools.set(tool.name, tool); },
    });
    const ctx = {
        cwd: "/project with spaces",
        signal: undefined,
        sessionManager: {
            getSessionId() { return "pi-conversation-a"; },
            getSessionFile() { return "/sessions/conversation-a.jsonl"; },
        },
    };
    return { handlers, tools, ctx, emit(name, event = {}) { return handlers.get(name)?.(event, ctx); } };
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
            "session_start", "agent_start", "ui_prompt_start", "ui_prompt_end", "agent_settled", "session_shutdown",
        ]);
        assert.equal(events[4].input.body.outcome, "succeeded");
        assert.equal(events[4].input.body.turn_id, events[1].input.body.turn_id);
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
            input: { paths: ["/project with spaces/source with spaces.cpp", "/project with spaces/-dash.txt"] },
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
