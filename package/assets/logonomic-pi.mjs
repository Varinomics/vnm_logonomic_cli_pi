import { spawn } from "node:child_process";
import { randomBytes, randomUUID } from "node:crypto";
import { isAbsolute, join, resolve as resolve_path } from "node:path";

const max_input_bytes  = 256 * 1024;
const max_output_bytes = 36 * 1024;
const helper_timeout_ms = 30_000;

function invoke_helper(helper, args, input, signal)
{
    const bytes = JSON.stringify(input);
    if (Buffer.byteLength(bytes) > max_input_bytes) {
        return Promise.reject(new Error("Logonomic request exceeds the helper input limit."));
    }
    if (signal?.aborted) {
        return Promise.reject(new Error("Logonomic request was cancelled."));
    }

    return new Promise((resolve, reject) => {
        const child = spawn(helper, args, {
            shell: false,
            windowsHide: true,
            stdio: ["pipe", "pipe", "pipe"],
        });
        const output = [];
        let output_bytes = 0;
        let failure;
        let stopped = false;

        const stop = (message) => {
            if (stopped) {
                return;
            }
            stopped = true;
            failure ??= new Error(message);
            // EOF asks the SDK helper to terminate and reap its vnmctl child.
            // Retain the helper until cleanup finishes; killing only its PID abandons that child.
            child.stdin.end();
        };
        const cancelled = () => stop("Logonomic request was cancelled.");
        const timer = setTimeout(() => stop("Logonomic helper did not finish in time."), helper_timeout_ms);

        signal?.addEventListener("abort", cancelled, { once: true });
        if (signal?.aborted) {
            cancelled();
        }
        child.on("error", (error) => { failure ??= error; });
        child.stdin.on("error", (error) => { failure ??= error; });
        child.stdout.on("data", (chunk) => {
            output_bytes += chunk.length;
            if (output_bytes > max_output_bytes) {
                stop("Logonomic helper response exceeds its output limit.");
                return;
            }
            output.push(chunk);
        });
        // Pi owns stdout and the terminal. The helper reports failures in its JSON result.
        child.stderr.resume();
        child.on("close", (code) => {
            clearTimeout(timer);
            signal?.removeEventListener("abort", cancelled);
            if (failure) {
                reject(failure);
                return;
            }
            let result;
            try {
                result = JSON.parse(Buffer.concat(output).toString("utf8"));
            }
            catch {
                reject(new Error("Logonomic helper returned an invalid response."));
                return;
            }
            if (!result || typeof result !== "object" || Array.isArray(result)) {
                reject(new Error("Logonomic helper returned an invalid response."));
                return;
            }
            if (code !== 0) {
                const reason = typeof result.reason === "string" ? result.reason : "request failed";
                reject(new Error(`Logonomic: ${reason}`));
                return;
            }
            resolve(result);
        });
        if (!stopped) {
            child.stdin.write(`${bytes}\n`);
        }
    });
}

function tool_result(text)
{
    return { content: [{ type: "text", text }], details: {} };
}

function native_cwd(ctx)
{
    try {
        const cwd = ctx.cwd;
        return typeof cwd === "string" && Buffer.byteLength(cwd) <= 4096 && !/[\u0000-\u001f\u007f]/.test(cwd)
            ? cwd : undefined;
    }
    catch {
        return undefined;
    }
}

function context_snapshot(ctx, session_id, event_id)
{
    const unavailable = {
        provider_session_id: session_id,
        event_id,
        usage_unavailable: true,
    };
    try {
        const usage = ctx.getContextUsage();
        if (!usage || usage.tokens === null) {
            return { context_updates: [unavailable] };
        }
        if (!Number.isSafeInteger(usage.tokens) || usage.tokens < 0 ||
            !Number.isSafeInteger(usage.contextWindow) || usage.contextWindow <= 0)
        {
            throw new Error("invalid context measurement");
        }
        return { context_updates: [{
            provider_session_id: session_id,
            event_id,
            used_tokens: usage.tokens,
            context_window: usage.contextWindow,
        }] };
    }
    catch {
        // Optional native telemetry cannot prevent delivery of its adjacent lifecycle fact.
        return { context_failure: {
            provider_session_id: session_id,
            reason_code: "observation_invalid",
            detail: "Pi could not provide its current context usage.",
        } };
    }
}

function model_label(value, limit)
{
    return typeof value === "string" && value.length > 0 && Buffer.byteLength(value) <= limit &&
        !/[\u0000-\u001f\u007f\uD800-\uDFFF]/u.test(value) ? value : undefined;
}

function model_sample(source, api, provider, id, effort)
{
    // A virtual selection routes each request separately. Only its physical response identifies the model.
    const model = api !== "pi-virtual" && model_label(id, 512);
    if (!model) {
        return { source, state: "unknown", provider: "", model: "" };
    }
    return {
        source, state: "confirmed", provider: model_label(provider, 512), model,
        reasoning_effort: model_label(effort, 128),
    };
}

function selected_model(pi, ctx)
{
    let effort;
    try {
        effort = pi.getThinkingLevel();
    }
    catch {
        // Losing the optional thinking level does not invalidate an independently observed model.
    }
    try {
        const model = ctx.model;
        return model_sample("pi.ctx.model", model?.api, model?.provider, model?.id, effort);
    }
    catch {
        return model_sample("pi.ctx.model");
    }
}

function available_models(ctx, row_budget)
{
    const empty = (state) => ({ state, models: [], total_count: 0, truncated: false });
    try {
        const registry = ctx.modelRegistry;
        if (typeof registry?.getAvailable !== "function") {
            return empty("unavailable");
        }
        if (registry.getError?.()) {
            return empty("unknown");
        }
        // The public RPC get_available_models reads this same live profile snapshot.
        // Do not refresh credentials or request a model to establish availability.
        const available = registry.getAvailable();
        if (!Array.isArray(available)) {
            return empty("unknown");
        }
        const snapshot = empty("available");
        const seen = new Set();
        for (const model of available) {
            const provider = model_label(model?.provider, 512);
            const id = model_label(model?.id, 512);
            if (!provider || !id || seen.has(provider + "\0" + id)) {
                return empty("unknown");
            }
            seen.add(provider + "\0" + id);
            if (snapshot.truncated) {
                continue;
            }
            const row = { provider, model: id, display_name: model_label(model.name, 256),
                reasoning: typeof model.reasoning === "boolean" ? model.reasoning : undefined };
            const size = Buffer.byteLength(JSON.stringify(row)) + 1;
            if (snapshot.models.length === 128 || size > row_budget) {
                snapshot.truncated = true;
            }
            else {
                snapshot.models.push(row);
                row_budget -= size;
            }
        }
        snapshot.total_count = seen.size;
        return snapshot;
    }
    catch {
        return empty("unknown");
    }
}

export default function logonomic_pi(pi)
{
    const helpers_dir = process.env.LOGONOMIC_PROVIDER_HELPERS_DIR;
    if (!helpers_dir) {
        throw new Error("The Logonomic Pi extension requires a Logonomic-managed terminal.");
    }
    const helper = join(helpers_dir, process.platform === "win32" ? "pi_helper.exe" : "pi_helper");
    let pending = Promise.resolve();
    let session_start;
    let activation_nonce = "";
    let turn_id = "";
    let attention_id = "";
    let outcome = "none";
    let source_sequence = 0n;
    let model_sequence = 0n;
    let catalog;

    function emit(name, ctx, extra = {}, snapshot_source, model)
    {
        const event_id = randomUUID();
        const session_id = ctx.sessionManager.getSessionId();
        const cwd = native_cwd(ctx);
        const input = {
            name,
            body: {
                session_id,
                session_file: ctx.sessionManager.getSessionFile() ?? "",
                cwd,
                event_id,
                turn_id,
                attention_id,
                ...extra,
            },
            idempotency_key: event_id,
            transition_nonce: activation_nonce,
        };
        if (model) {
            input.body.model_state = {
                version: 1,
                updates: [{
                    provider_session_id: session_id,
                    transition_nonce: activation_nonce,
                    sequence: (model_sequence++).toString(),
                    agent_id: "",
                    ...model,
                }],
            };
        }
        if (snapshot_source) {
            Object.assign(input.body, context_snapshot(ctx, session_id, event_id));
            const confirmed = typeof cwd === "string" && isAbsolute(cwd);
            input.body.execution_context = {
                version: 1,
                updates: [{
                    provider_session_id: session_id,
                    transition_nonce: activation_nonce,
                    sequence: (source_sequence++).toString(),
                    state: confirmed ? "confirmed" : "unknown",
                    environment: confirmed ? "local" : "",
                    cwd: confirmed ? cwd : "",
                    source: `pi.${snapshot_source}`,
                }],
            };
        }
        let sample;
        const owner = catalog;
        if (name === "session_start" || name === "agent_start" || snapshot_source === "model_select") {
            // Keep the SDK's 28 KiB catalog and 48 KiB event bounds, including
            // the native paths already present and the catalog activation fields.
            const row_budget = Math.min(24 * 1024, 46 * 1024 - Buffer.byteLength(JSON.stringify(input.body)));
            if (owner && row_budget >= 0) {
                const value = available_models(ctx, row_budget);
                sample = { value, signature: JSON.stringify(value), sequence: (owner.sequence++).toString() };
            }
        }
        // UI prompt hooks are not awaited by Pi. Preserve their order with the awaited lifecycle hooks.
        const delivery = pending.then(async () => {
            if (sample && sample.signature !== owner.delivered) {
                input.body.model_catalog = { version: 1, snapshot: {
                    provider_session_id: session_id, transition_nonce: input.transition_nonce, sequence: sample.sequence,
                    source: "pi.modelRegistry.getAvailable", ...sample.value,
                } };
            }
            const result = await invoke_helper(helper, ["event"], input);
            if (!result.committed || !result.accepted) {
                throw new Error(`Logonomic lifecycle event was rejected: ${result.reason ?? name}`);
            }
            if (input.body.model_catalog) {
                owner.delivered = sample.signature;
            }
        });
        pending = delivery.catch(() => {});
        return delivery;
    }

    async function begin_work(ctx)
    {
        if (turn_id) {
            return;
        }
        turn_id = randomUUID();
        outcome = "none";
        await emit("agent_start", ctx, {}, undefined, selected_model(pi, ctx));
    }

    pi.on("session_start", (event, ctx) => {
        const session_id = ctx.sessionManager.getSessionId();
        // Rebinding the same Pi runtime can repeat session_start without a shutdown in between.
        if (session_start?.session_id === session_id) {
            return session_start.delivery;
        }
        activation_nonce = randomBytes(16).toString("hex");
        source_sequence = 0n;
        model_sequence = 0n;
        catalog = { sequence: 0n, delivered: undefined };
        const delivery = emit("session_start", ctx, { reason: event.reason }, "session_start", selected_model(pi, ctx));
        session_start = { session_id, delivery };
        return delivery;
    });

    pi.on("before_agent_start", async (event, ctx) => {
        await begin_work(ctx);
        await pending;
        const session_id = ctx.sessionManager.getSessionId();
        const transition_nonce = activation_nonce;
        const instruction = await invoke_helper(helper, ["control", "title-instruction"], {
            source: "pi-extension",
            hook_event_name: "before_agent_start",
            session_id,
            transition_nonce,
        }, ctx.signal);
        if (!instruction.completed) {
            throw new Error(`Logonomic title instruction could not be obtained: ${instruction.reason ?? "request failed"}`);
        }
        // The host may refuse context for a retired activation without vetoing the native Pi turn.
        if (!instruction.accepted || transition_nonce !== activation_nonce || session_start?.session_id !== session_id) {
            return;
        }
        if (instruction.instruction_required) {
            if (typeof instruction.instruction !== "string" || !instruction.instruction) {
                throw new Error("Logonomic returned an empty title instruction.");
            }
            return {
                message: {
                    customType: "logonomic-title-instruction",
                    content: instruction.instruction,
                    display: false,
                },
            };
        }
    });

    pi.on("agent_start", async (event, ctx) => {
        outcome = "none";
        await begin_work(ctx);
    });

    pi.on("message_end", (event, ctx) => {
        if (event.message.role !== "assistant") {
            return;
        }
        switch (event.message.stopReason) {
            case "error":   outcome = "failed";      break;
            case "aborted": outcome = "interrupted"; break;
            case "stop":
            case "length":  outcome = "succeeded";   break;
            default:        outcome = "none";        break;
        }
        if (outcome !== "failed" && outcome !== "interrupted" && turn_id &&
            session_start?.session_id === ctx.sessionManager.getSessionId())
        {
            const { api, provider, model, thinkingLevel } = event.message;
            return emit("model_snapshot", ctx, {}, undefined,
                model_sample("pi.message_end", api, provider, model, thinkingLevel));
        }
    });

    pi.on("agent_settled", async (event, ctx) => {
        const delivery = emit("agent_settled", ctx, {
            aborted: event.aborted,
            outcome: event.aborted ? "interrupted" : outcome,
        }, "agent_settled");
        turn_id = "";
        outcome = "none";
        await delivery;
    });

    for (const name of ["model_select", "thinking_level_select", "session_compact", "session_compact_failed"]) {
        pi.on(name, (event, ctx) => {
            if (session_start?.session_id !== ctx.sessionManager.getSessionId()) {
                return;
            }
            if (name === "thinking_level_select") {
                return emit("model_snapshot", ctx, {}, undefined, selected_model(pi, ctx));
            }
            return emit("context_snapshot", ctx, {}, name, name === "model_select" ? selected_model(pi, ctx) : undefined);
        });
    }

    pi.on("ui_prompt_start", (event, ctx) => {
        attention_id = randomUUID();
        return emit("ui_prompt_start", ctx, { reason: event.reason });
    });

    pi.on("ui_prompt_end", (event, ctx) => {
        const delivery = emit("ui_prompt_end", ctx);
        attention_id = "";
        return delivery;
    });

    pi.on("session_shutdown", async (event, ctx) => {
        // Reload and session replacement rebind the extension; they do not close the terminal.
        if (event.reason === "quit") {
            await emit("session_shutdown", ctx, { reason: event.reason });
        }
        await pending;
        session_start = undefined;
        turn_id = "";
        attention_id = "";
        outcome = "none";
    });

    pi.registerTool({
        name: "logonomic_set_title",
        label: "Set Logonomic title",
        description: "Set the title of this Logonomic terminal to a short description of the current task.",
        parameters: {
            type: "object",
            properties: { title: { type: "string", minLength: 1 } },
            required: ["title"],
            additionalProperties: false,
        },
        async execute(tool_call_id, params, signal) {
            const result = await invoke_helper(helper, ["control", "set-title"], params, signal);
            if (result.ok !== true) {
                throw new Error(`Logonomic could not set the terminal title: ${result.reason ?? "request failed"}`);
            }
            return tool_result("Terminal title updated.");
        },
    });

    pi.registerTool({
        name: "logonomic_show_changed_files",
        label: "Show changed files",
        description: "Open changed files in Logonomic's diff viewer when the user asks to review changes. " +
            "Paths are absolute or relative to Pi's current directory.",
        parameters: {
            type: "object",
            properties: {
                paths: { type: "array", items: { type: "string", minLength: 1 }, minItems: 1 },
            },
            required: ["paths"],
            additionalProperties: false,
        },
        async execute(tool_call_id, params, signal, on_update, ctx) {
            const paths = params.paths.map((path) => resolve_path(ctx.cwd, path));
            const result = await invoke_helper(helper, ["control", "show-changed-files"], { paths }, signal);
            if (result.ok !== true) {
                throw new Error(`Logonomic could not show changed files: ${result.reason ?? "request failed"}`);
            }
            return tool_result("Changed files opened in Logonomic.");
        },
    });
}
