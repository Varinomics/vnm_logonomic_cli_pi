import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { appendFile, copyFile, mkdir, mkdtemp, readFile, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";

const [npm, simulator, provider] = process.argv.slice(2);
const root = await mkdtemp(join(tmpdir(), "pi-npm-launchers-"));
const platform = "windows-x86_64";
const asset = "assets/logonomic-pi.mjs";
const npm_options = ["--offline", "--ignore-scripts", "--no-audit", "--no-fund", "--no-update-notifier",
    "--userconfig", join(root, "npmrc"), "--cache", join(root, "cache")];

function launcher(script_body, runner_key) {
    return { script_body, runner_key, environment_text: "" };
}

async function invoke(capability_id, version, operation, payload) {
    const script = join(root, "request.json");
    await writeFile(script, JSON.stringify({ script_version: 1, steps: [
        { op: "invoke", id: "probe", capability_id, version, operation, payload, deadline_ms: 10000 },
        { op: "wait_result", id: "probe", timeout_ms: 10000 },
        { op: "shutdown", grace_ms: 2000 },
    ] }));
    const trace = execFileSync(simulator, [provider, "--script", script], { encoding: "utf8", timeout: 20000 });
    const result = trace.split("\n").filter(line => line.startsWith("< ")).map(line => JSON.parse(line.slice(2)))
        .find(frame => frame.type === "result");
    assert.equal(result?.status, "success", trace);
    return result.payload;
}

async function check_install(global, archive) {
    const prefix = join(root, global ? "global prefix" : "local prefix");
    const name = "@earendil-works/pi-coding-agent";
    const bin = global ? prefix : join(prefix, "node_modules", ".bin");
    await mkdir(prefix);
    execFileSync(process.execPath, [npm, "install", "--prefix", prefix, ...(global ? ["--global"] : []),
        ...npm_options, archive], { cwd: root, timeout: 30000, stdio: "pipe" });

    const environment = { PATH: bin + ";" + dirname(process.execPath), PATHEXT: ".COM;.EXE;.BAT;.CMD", USERPROFILE: root };
    const discover = () => invoke("launcher-discovery", 1, "discover",
        { platform, ambient_environment: environment, default_home_fallback: root });
    const recognize = saved => invoke("launcher-discovery", 1, "recognize",
        { platform, ambient_environment: environment, launcher: saved });
    const prepare = saved => invoke("launch-preparation", 3, "prepare", {
        launcher: saved, launcher_argv: saved.runner_key === "cmd" ? ["cmd.exe", "/c", saved.script_body] :
            ["powershell.exe", "-NoLogo", "-Command", saved.script_body],
        working_directory: root, title_instruction: "", declared_assets: [asset],
        launch_environment: { platform, effective_environment: environment },
    });
    const candidates = (await discover()).candidates;
    assert.equal(candidates.length, 1);
    assert.equal((await recognize(launcher(candidates[0].script_body, candidates[0].runner_key))).recognized, true);
    const entry = await realpath(join(prefix, "node_modules", name, "dist", "cli.js"));
    const command = launcher('pi --model "two words"', "cmd");
    const powershell = launcher('pi --model "two words"', "powershell");
    const stem = join(bin, "pi");
    const local_stem = ".\\" + relative(root, stem);
    for (const saved of [command, powershell, launcher(`"${stem}" --model "two words"`, "cmd"),
        launcher(`& '${stem}' --model "two words"`, "powershell")]) {
        assert.equal((await recognize(saved)).recognized, true, saved.script_body);
    }
    for (const saved of [command, powershell, launcher(`"${local_stem}" --model "two words"`, "cmd"),
        launcher(`& '${local_stem}' --model "two words"`, "powershell")]) {
        const patch = await prepare(saved);
        assert.ok(patch.argv, saved.script_body);
        assert.equal(await realpath(patch.argv[0]), await realpath(process.execPath));
        assert.equal(await realpath(patch.argv[1]), entry);
        assert.deepEqual(patch.argv.slice(2), ["--extension", "{{asset:" + asset + "}}", "--model", "two words"]);
    }
    const adjacent = join(bin, "node.exe");
    await copyFile(process.execPath, adjacent);
    assert.equal(await realpath((await prepare(command)).argv[0]), await realpath(adjacent));

    const ps1 = join(bin, "pi.ps1");
    const cmd = join(bin, "pi.cmd");
    const generated = await readFile(cmd);
    await appendFile(ps1, "\nWrite-Output 'custom action'\n");
    assert.equal((await recognize(powershell)).recognized, false);
    assert.equal((await prepare(launcher(`& '${local_stem}'`, "powershell"))).argv, undefined);
    assert.equal((await recognize(command)).recognized, true);
    await writeFile(cmd, Buffer.concat([Buffer.from("@echo custom action\r\n"), generated]));
    assert.equal((await recognize(command)).recognized, false);
    assert.equal((await discover()).candidates.length, 0);
}

try {
    // npm generates the actual scripts. An offline package with Pi's public bin
    // contract makes the local and global layouts reproducible without credentials.
    const fixture = join(root, "source");
    await mkdir(join(fixture, "dist"), { recursive: true });
    await writeFile(join(fixture, "package.json"), JSON.stringify({
        name: "@earendil-works/pi-coding-agent", version: "1.5.2", bin: { pi: "dist/cli.js" },
    }));
    await writeFile(join(fixture, "dist", "cli.js"), "#!/usr/bin/env node\n");
    await writeFile(join(root, "npmrc"), "");
    const archive = JSON.parse(execFileSync(process.execPath,
        [npm, "pack", ...npm_options, "--json", "--pack-destination", root, fixture],
        { cwd: root, encoding: "utf8", timeout: 30000 }))[0].filename;
    await check_install(false, join(root, archive));
    await check_install(true, join(root, archive));
} finally {
    await rm(root, { recursive: true, force: true });
}
