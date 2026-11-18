import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { appendFileSync, readFileSync, writeFileSync } from "node:fs";
import { copyFile, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { create_fixture, executable, observe_records, write_script } from "./pi_test_support.mjs";

const instruction = "Set the Logonomic title with logonomic_set_title for this task.";

function environment(directory)
{
    return {
        LOGONOMIC_VNMCTL_PATH: join(directory, executable("vnmctl-fixture")),
        LOGONOMIC_SESSION_CAPABILITY: "1".repeat(64),
        LOGONOMIC_PROVIDER_PACKAGE_ID: "logonomic.cli.pi",
        LOGONOMIC_PROVIDER_GENERATION_DIGEST: "2".repeat(64),
        LOGONOMIC_PROVIDER_GENERATION_NONCE: "3".repeat(32),
        LOGONOMIC_PROVIDER_HELPERS_DIR: join(directory, "helpers"),
    };
}

function keys(value, expected)
{
    assert.ok(value && typeof value === "object" && !Array.isArray(value));
    assert.deepEqual(Object.keys(value).sort(), [...expected].sort());
}

function classify(directory, input, ledger)
{
    const config = JSON.parse(readFileSync(join(directory, "classifier.json"), "utf8"));
    if (!config) {
        return;
    }
    ledger.revision += 1;
    const script = join(directory, "classifier-script.json");
    const activation = ledger.activation;
    writeFileSync(script, JSON.stringify({ script_version: 1, steps: [
        { op: "invoke", id: "native-event", capability_id: "helper-ingress", version: 2,
            operation: "classify_event", deadline_ms: 3000, payload: {
                event: input.event,
                ledger: { current_epoch: ledger.epoch, current_transition: activation ? {
                    kind: ledger.kind,
                    provider_session_id: activation.provider_session_id,
                    nonce: activation.transition_nonce,
                } : null },
                transition_nonce: input.transition_nonce,
                context_state_version: 2,
                context_snapshot_revision: ledger.revision,
                execution_context: { version: 1 },
                model_state: { version: 1 },
                model_catalog: { version: 1 },
            } },
        { op: "wait_result", id: "native-event", timeout_ms: 4000 },
        { op: "shutdown", grace_ms: 2000 },
    ] }));
    const transcript = execFileSync(config.simulator, [config.package_directory, "--script", script], {
        encoding: "utf8", timeout: 10_000, maxBuffer: 2 * 1024 * 1024,
    });
    const frames = transcript.split("\n").filter((line) => line.startsWith("< "))
        .map((line) => JSON.parse(line.slice(2)));
    const response = frames.find((frame) => frame.type === "result");
    assert.equal(response?.status, "success", transcript);
    const result = response.payload;
    if (input.event.body.context_updates) {
        assert.equal(result.context_updates?.length, 1, transcript);
        const update = result.context_updates[0];
        assert.equal(update.provider_session_id, input.event.body.session_id);
        assert.equal(update.compaction_count_revision, ledger.revision);
        assert.equal(update.used_tokens, input.event.body.context_updates[0].used_tokens);
        assert.equal(update.context_window, input.event.body.context_updates[0].context_window);
        assert.equal(Boolean(update.usage_unavailable), Boolean(input.event.body.context_updates[0].usage_unavailable));
    }
    if (input.event.body.execution_context) {
        assert.deepEqual(result.execution_context, input.event.body.execution_context);
    }
    if (input.event.body.model_state) {
        assert.deepEqual(result.model_state, input.event.body.model_state);
    }
    if (input.event.body.model_catalog) {
        assert.deepEqual(result.model_catalog, input.event.body.model_catalog);
    }
    appendFileSync(join(directory, "classifier.jsonl"), `${JSON.stringify({
        event: input.event, revision: ledger.revision, result, transcript,
    })}\n`);
    return result;
}

// This is an isolated vnmctl protocol oracle, not the Logonomic desktop host.
// Response shapes and commands come from SDK helper_ingress_2.md,
// model_context_delivery_2.md, and Helper_client's public control methods.
export async function run_vnmctl(directory)
{
    for (const [name, value] of Object.entries(environment(directory))) {
        assert.equal(process.env[name], value, `The SDK wrapper must inherit its bound ${name}.`);
    }
    const args = process.argv.slice(2);
    let bytes = "";
    for await (const chunk of process.stdin) {
        bytes += chunk;
        assert.ok(Buffer.byteLength(bytes) <= 256 * 1024);
    }
    const input = bytes ? JSON.parse(bytes) : null;
    const log = join(directory, "vnmctl.jsonl");
    appendFileSync(log, `${JSON.stringify({ args, input, pid: process.pid })}\n`);
    const ledger_path = join(directory, "ledger.json");
    const ledger = JSON.parse(readFileSync(ledger_path, "utf8"));

    if (args[0] === "provider-event" && args[1] === "deliver") {
        assert.deepEqual(args, ["provider-event", "deliver", "--args-stdin"]);
        keys(input, ["event", "idempotency_key", "transition_nonce"]);
        keys(input.event, ["name", "body"]);
        assert.match(input.transition_nonce, /^[0-9a-f]{32}$/);
        assert.equal(input.idempotency_key, input.event.body.event_id);
        assert.equal(typeof input.event.body.session_id, "string");
        assert.ok(!ledger.pending.includes(input.idempotency_key), "Native deliveries must use unique event IDs.");
        const activation = {
            provider_session_id: input.event.body.session_id,
            transition_nonce: input.transition_nonce,
        };
        const result = classify(directory, input, ledger);
        if (input.event.name === "session_start") {
            ledger.epoch += 1;
            ledger.activation = activation;
            ledger.kind = result?.transition.kind ?? (input.event.body.reason === "resume" ||
                input.event.body.reason === "reload" ? "resume" : "start");
        }
        else {
            assert.deepEqual(activation, ledger.activation, "Events must retain the selected Pi activation.");
        }
        ledger.pending.push(input.idempotency_key);
        writeFileSync(ledger_path, JSON.stringify(ledger));
        process.stdout.write(JSON.stringify({ accepted: true, epoch: ledger.epoch }));
        return;
    }
    if (args[0] === "provider-event" && args[1] === "acknowledge") {
        assert.equal(args.length, 3);
        assert.equal(input, null);
        assert.ok(ledger.pending.includes(args[2]), "Acknowledge only a committed delivery.");
        ledger.pending = ledger.pending.filter((key) => key !== args[2]);
        writeFileSync(ledger_path, JSON.stringify(ledger));
        return;
    }
    if (args[0] === "request-title-instruction") {
        assert.deepEqual(args, ["request-title-instruction", "--args-stdin"]);
        keys(input, ["source", "hook_event_name", "changed_files_requested", "delivery"]);
        assert.equal(input.source, "pi-extension");
        assert.equal(input.hook_event_name, "before_agent_start");
        assert.equal(input.changed_files_requested, false);
        keys(input.delivery, ["version", "kind", "activation"]);
        assert.equal(input.delivery.version, 2);
        assert.equal(input.delivery.kind, "model-context");
        assert.deepEqual(input.delivery.activation, ledger.activation);
        process.stdout.write(JSON.stringify({ accepted: true, instruction_required: true, instruction }));
        return;
    }
    if (args[0] === "set-title") {
        assert.equal(args.length, 2);
        assert.equal(input, null);
        if (args[1] === "slow-title") {
            // The native abort must cancel and reap this SDK-owned wrapper before it returns.
            await new Promise((resolve) => setTimeout(resolve, 60_000));
            throw new Error("The native cancellation failed to stop its vnmctl child.");
        }
        assert.equal(args[1], "Pi integration fixture");
        return;
    }
    assert.deepEqual(args, ["call", "logonomic.diff", "show_changed_files", "--", join(directory, "source file.cpp")]);
    assert.equal(input, null);
}

export async function create_sdk_fixture(helper_binary, classifier)
{
    const fixture = await create_fixture();
    await copyFile(resolve(helper_binary), join(fixture.helpers, executable("pi_helper")));
    const env = environment(fixture.directory);
    const log = join(fixture.directory, "vnmctl.jsonl");
    await writeFile(log, "");
    await writeFile(join(fixture.directory, "ledger.json"), JSON.stringify({ epoch: 0, activation: null, pending: [], revision: 0 }));
    await writeFile(join(fixture.directory, "classifier.json"), JSON.stringify(classifier ?? null));
    await writeFile(join(fixture.directory, "classifier.jsonl"), "");
    await write_script(join(fixture.directory, "vnmctl-fixture"),
        `#!${process.execPath}\nimport { run_vnmctl } from ${JSON.stringify(import.meta.url)};\n` +
        `await run_vnmctl(${JSON.stringify(fixture.directory)});\n`);
    const wrapper = observe_records(log);
    return {
        ...fixture,
        env,
        wait_for_wrapper: wrapper.wait_for_record,
        async classifications() {
            return readFileSync(join(fixture.directory, "classifier.jsonl"), "utf8").split("\n").filter(Boolean)
                .map((line) => JSON.parse(line));
        },
        async records() {
            return (await wrapper.records()).filter((record) => record.args[1] === "deliver").map(({ input }) => ({
                args: ["event"],
                input: {
                    ...input.event,
                    idempotency_key: input.idempotency_key,
                    transition_nonce: input.transition_nonce,
                },
            }));
        },
        async verify() {
            const records = await wrapper.records();
            const deliveries = records.filter((record) => record.args[1] === "deliver");
            const acknowledgements = records.filter((record) => record.args[1] === "acknowledge");
            assert.deepEqual(acknowledgements.map((record) => record.args[2]),
                deliveries.map((record) => record.input.idempotency_key));
            assert.equal(records.filter((record) => record.args[0] === "request-title-instruction").length, classifier ? 5 : 4);
            assert.equal(records.filter((record) => record.args[0] === "set-title").length, 2);
            assert.equal(records.filter((record) => record.args[0] === "call").length, 1);
            assert.deepEqual(JSON.parse(readFileSync(join(fixture.directory, "ledger.json"), "utf8")).pending, []);
            return { deliveries: deliveries.length, acknowledgements: acknowledgements.length };
        },
        async save_evidence(directory) {
            await copyFile(log, join(directory, "pi-vnmctl.jsonl"));
            await copyFile(join(fixture.directory, "classifier.jsonl"), join(directory, "pi-classifier.jsonl"));
        },
    };
}
