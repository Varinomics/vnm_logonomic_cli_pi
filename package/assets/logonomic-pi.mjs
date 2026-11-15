import { spawn } from "node:child_process";
import { randomBytes, randomUUID } from "node:crypto";
import { join, resolve as resolve_path } from "node:path";

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

    function emit(name, ctx, extra = {})
    {
        const event_id = randomUUID();
        const input = {
            name,
            body: {
                session_id: ctx.sessionManager.getSessionId(),
                session_file: ctx.sessionManager.getSessionFile() ?? "",
                cwd: ctx.cwd,
                event_id,
                turn_id,
                attention_id,
                ...extra,
            },
            idempotency_key: event_id,
            transition_nonce: activation_nonce,
        };
        // UI prompt hooks are not awaited by Pi. Preserve their order with the awaited lifecycle hooks.
        const delivery = pending.then(async () => {
            const result = await invoke_helper(helper, ["event"], input);
            if (!result.committed || !result.accepted) {
                throw new Error(`Logonomic lifecycle event was rejected: ${result.reason ?? name}`);
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
        await emit("agent_start", ctx);
    }

    pi.on("session_start", (event, ctx) => {
        const session_id = ctx.sessionManager.getSessionId();
        // Rebinding the same Pi runtime can repeat session_start without a shutdown in between.
        if (session_start?.session_id === session_id) {
            return session_start.delivery;
        }
        activation_nonce = randomBytes(16).toString("hex");
        const delivery = emit("session_start", ctx, { reason: event.reason });
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

    pi.on("message_end", (event) => {
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
    });

    pi.on("agent_settled", async (event, ctx) => {
        const delivery = emit("agent_settled", ctx, {
            aborted: event.aborted,
            outcome: event.aborted ? "interrupted" : outcome,
        });
        turn_id = "";
        outcome = "none";
        await delivery;
    });

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
