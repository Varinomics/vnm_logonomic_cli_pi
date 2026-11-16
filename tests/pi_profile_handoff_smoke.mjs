import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { create_sdk_fixture } from "./pi_sdk_fixture.mjs";

const [cli_argument, package_argument, test_binary] = process.argv.slice(2);
if (!cli_argument || !package_argument || !test_binary || process.platform === "win32") {
    throw new Error("Usage: node tests/pi_profile_handoff_smoke.mjs <Pi bundled CLI> <package> <pi_profile_tests> (POSIX)");
}
const cli = resolve(cli_argument);
const package_root = resolve(package_argument);
const { SessionManager } = await import(pathToFileURL(resolve(dirname(cli), "../core/session-manager.js")).href);
const config_url = pathToFileURL(resolve(dirname(cli), "../config.js")).href;
const fixture = await create_sdk_fixture(join(package_root, "helpers/pi_helper"));
const bash_quote = (value) => `'${value.replaceAll("'", "'\\''")}'`;

async function run_native(executable, arguments_, environment)
{
    return new Promise((resolve_result, reject) => {
        const child = execFile(executable, [...arguments_, "--mode", "rpc"], {
            cwd: fixture.directory, env: environment, timeout: 30_000, maxBuffer: 2 * 1024 * 1024,
        }, (error, stdout, stderr) => {
            if (error) reject(new Error(`Native Pi profile handoff failed: ${stderr}`, { cause: error }));
            else resolve_result(stdout);
        });
        child.stdin.end(`${JSON.stringify({ id: "state", type: "get_state" })}\n`);
    });
}

try {
    const home = join(fixture.directory, "isolated-home");
    const selected_profile = join(fixture.directory, "team profile");
    const default_profile = join(home, ".pi", "agent");
    const report_file = join(fixture.directory, "native-profile.json");
    const reporter = join(fixture.directory, "profile-reporter.mjs");
    const sentinel = "literal $value ; λ";
    const ambient_xdg = join(fixture.directory, "ambient-xdg");
    // Both dummy profiles can start the same local model without contacting it.
    // A lost leading assignment must reach Pi and reveal its wrong native home.
    for (const profile of [selected_profile, default_profile]) {
        await mkdir(profile, { recursive: true });
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
    }
    await writeFile(reporter, `import { writeFileSync } from "node:fs";
import { getAgentDir } from ${JSON.stringify(config_url)};
export default function(pi) {
    pi.on("session_start", (_event, ctx) => {
        writeFileSync(${JSON.stringify(report_file)}, JSON.stringify({
            profile: getAgentDir(), session_id: ctx.sessionManager.getSessionId(),
            sentinel: process.env.PI_HANDOFF_SENTINEL ?? null,
            empty: process.env.PI_HANDOFF_EMPTY ?? null,
            xdg: process.env.XDG_CONFIG_HOME ?? null,
        }));
    });
}
`);
    const native = SessionManager.create(fixture.directory, join(selected_profile, "sessions", "resume-fixture"));
    native.appendMessage({ role: "user", content: "Native profile handoff fixture", timestamp: Date.now() });
    const words = [process.execPath, cli, "--offline", "--provider", "logonomic-fixture", "--model", "fixture-model",
        "--thinking", "off", "--extension", reporter];
    const script = `PI_CODING_AGENT_DIR=${bash_quote(selected_profile)} PI_HANDOFF_SENTINEL='overridden' PI_HANDOFF_SENTINEL=${bash_quote(sentinel)} ` +
        `PI_HANDOFF_EMPTY='' ${words.map(bash_quote).join(" ")}`;
    const request_file = join(fixture.directory, "requests.json");
    const proposal_file = join(fixture.directory, "proposals.json");
    const frozen = {
        HOME: home, PATH: process.env.PATH, XDG_CONFIG_HOME: join(fixture.directory, "frozen-unrelated-xdg"),
        PI_CODING_AGENT_DIR: selected_profile, PI_HANDOFF_SENTINEL: sentinel, PI_HANDOFF_EMPTY: "",
    };
    await writeFile(request_file, JSON.stringify({
        launch: {
            launcher: { script_body: script, runner_key: "bash", environment_text: "" },
            launcher_argv: ["/bin/bash", "-c", script], working_directory: fixture.directory,
            title_instruction: "", declared_assets: ["assets/logonomic-pi.mjs"],
            launch_environment: { platform: process.platform === "darwin" ? "macos-arm64" : "linux-x86_64", effective_environment: frozen },
        },
        resume_record: { provider_session_id: native.getSessionId(), session_file: native.getSessionFile() },
    }));
    await promisify(execFile)(resolve(test_binary), ["--native-handoff", package_root, request_file, proposal_file], { timeout: 30_000 });
    const proposals = JSON.parse(await readFile(proposal_file, "utf8"));
    assert.equal(proposals.selected_profile, selected_profile);
    const observations = [];
    for (const mode of ["interactive", "resume"]) {
        const proposal = proposals[mode];
        // Mirror the host's terminal overlay: frozen evidence is NOT the base
        // environment. Only the explicit set/unset patch reaches the process.
        const environment = { HOME: home, PATH: process.env.PATH, LANG: "C.UTF-8", XDG_CONFIG_HOME: ambient_xdg, ...fixture.env };
        assert.equal(environment.PI_CODING_AGENT_DIR, undefined);
        for (const name of proposal.environment.unset) delete environment[name];
        Object.assign(environment, proposal.environment.set);
        const executable = mode === "interactive" ? proposal.argv[0] : join(package_root, proposal.process.executable);
        const arguments_ = mode === "interactive" ? proposal.argv.slice(1) : proposal.process.argv;
        const stdout = await run_native(executable, arguments_, environment);
        const state = stdout.split("\n").filter(Boolean).map((line) => JSON.parse(line)).find((record) => record.id === "state");
        assert.equal(state?.success, true, stdout);
        const observation = JSON.parse(await readFile(report_file, "utf8"));
        if (mode === "resume") assert.equal(observation.session_id, native.getSessionId());
        observations.push({ mode, ...observation });
    }
    assert.deepEqual(observations.map(({ mode, profile }) => ({ mode, profile })),
        ["interactive", "resume"].map((mode) => ({ mode, profile: selected_profile })),
        "Both the prepared interactive process and SDK resume bridge must launch Pi in the acknowledged native profile.");
    for (const observation of observations) {
        assert.equal(observation.sentinel, sentinel, `${observation.mode}: frozen literal assignment survives without shell expansion`);
        assert.equal(observation.empty, "", `${observation.mode}: an explicit empty assignment remains present`);
        assert.equal(observation.xdg, ambient_xdg, `${observation.mode}: unrelated frozen values are not imported into the terminal`);
        assert.deepEqual(Object.keys(proposals[observation.mode].environment.set).sort(),
            ["PI_CODING_AGENT_DIR", "PI_HANDOFF_EMPTY", "PI_HANDOFF_SENTINEL"]);
    }
    console.log("Passed native Pi interactive and SDK bridge profile handoff; exact leading assignments survive and unrelated frozen values remain unprojected.");
}
finally {
    await fixture.dispose();
}
