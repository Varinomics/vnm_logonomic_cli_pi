import { execFile } from "node:child_process";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

const [cli_argument, test_binary] = process.argv.slice(2);
if (!cli_argument || !test_binary) {
    throw new Error("Usage: node tests/pi_profile_smoke.mjs <Pi bundled CLI> <pi_profile_tests>");
}
const cli = resolve(cli_argument);
const { getAgentDir } = await import(pathToFileURL(resolve(dirname(cli), "../config.js")).href);
const { resolvePath } = await import(pathToFileURL(resolve(dirname(cli), "../utils/paths.js")).href);
const directory = await mkdtemp(join(tmpdir(), "logonomic-pi-profile-oracle-"));
const original_cwd = process.cwd();
const home_name = process.platform === "win32" ? "USERPROFILE" : "HOME";
const original_home = process.env[home_name];
const original_profile = process.env.PI_CODING_AGENT_DIR;
const cases = [];

function capture(name, values)
{
    delete process.env[home_name];
    delete process.env.PI_CODING_AGENT_DIR;
    Object.assign(process.env, values);
    cases.push({ name, environment: values, cwd: directory, expected: resolvePath(getAgentDir()) });
}

try {
    process.chdir(directory);
    const home = join(directory, "native home");
    const base = { [home_name]: home };
    capture("default profile", base);
    capture("empty profile override", { ...base, PI_CODING_AGENT_DIR: "" });
    capture("absolute override", { ...base, PI_CODING_AGENT_DIR: join(directory, "external profile λ") });
    capture("relative override and dot segments", { ...base, PI_CODING_AGENT_DIR: "relative/../ profile λ " });
    capture("tilde profile", { ...base, PI_CODING_AGENT_DIR: "~" });
    capture("tilde directory", { ...base, PI_CODING_AGENT_DIR: "~/team" });
    capture("tilde repeated separators", { ...base, PI_CODING_AGENT_DIR: "~//team" });
    capture("local file URL", { ...base, PI_CODING_AGENT_DIR: pathToFileURL(join(directory, "profile λ #1")).href });
    capture("file URL with dot segments", { ...base, PI_CODING_AGENT_DIR: `${pathToFileURL(directory).href}/one/../profile` });
    if (process.platform !== "win32") {
        capture("localhost file URL", { ...base, PI_CODING_AGENT_DIR: `file://localhost${directory}/profile` });
        capture("explicit empty native home", { [home_name]: "" });
        capture("relative native home", { [home_name]: "relative-home" });
        capture("empty native home with tilde", { [home_name]: "", PI_CODING_AGENT_DIR: "~/team" });
    }
    const fixture = join(directory, "native-paths.json");
    await writeFile(fixture, JSON.stringify(cases));
    const { stdout } = await promisify(execFile)(resolve(test_binary), ["--native-paths", fixture], { timeout: 20_000 });
    process.stdout.write(stdout);
    console.log(`Verified ${cases.length} profile paths against installed Pi; no credentials or model calls.`);
}
finally {
    process.chdir(original_cwd);
    if (original_home === undefined) delete process.env[home_name];
    else process.env[home_name] = original_home;
    if (original_profile === undefined) delete process.env.PI_CODING_AGENT_DIR;
    else process.env.PI_CODING_AGENT_DIR = original_profile;
    await rm(directory, { recursive: true, force: true });
}
