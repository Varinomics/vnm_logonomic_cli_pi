import { watch } from "node:fs";
import { copyFile, mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

export const executable = (name) => process.platform === "win32" ? name + ".exe" : name;

export async function write_script(path, script)
{
    if (process.platform === "win32") {
        await copyFile(process.env.PI_TEST_SCRIPT_RUNNER, executable(path));
        await writeFile(path + ".mjs", script);
    }
    else {
        await writeFile(path, script, { mode: 0o755 });
    }
}

export async function create_fixture({ title_refused = false, title_delay_ms = 0 } = {})
{
    const directory = await mkdtemp(join(tmpdir(), "logonomic-pi-test-"));
    const helpers = join(directory, "helpers");
    const log = join(directory, "helper.jsonl");
    await mkdir(helpers);
    await writeFile(log, "");
    const script = `#!${process.execPath}
import { appendFileSync } from "node:fs";
process.stdin.setEncoding("utf8");
process.stdin.on("end", () => {
    appendFileSync(${JSON.stringify(log)}, JSON.stringify({args:["cancelled"], input:{}}) + "\\n");
    process.exit(1);
});
const input = await new Promise(resolve => {
    let bytes = "";
    function read(chunk) {
        bytes += chunk;
        const newline = bytes.indexOf("\\n");
        if (newline !== -1) {
            process.stdin.off("data", read);
            resolve(JSON.parse(bytes.slice(0, newline)));
        }
    }
    process.stdin.on("data", read);
});
const args = process.argv.slice(2);
appendFileSync(${JSON.stringify(log)}, JSON.stringify({args, input}) + "\\n");
if (input.title === "slow-title") await new Promise(resolve => setTimeout(resolve, 60000));
function output(value, code = 0) {
    process.stdout.end(value, () => process.exit(code));
}
if (input.title === "large-response") {
    output("x".repeat(40000));
}
else if (input.title === "reject-title" || input.body?.reason === "reject-event") {
    output(JSON.stringify({ok:false, committed:true, accepted:false, reason:"fixture refusal"}), 1);
}
else if (args[0] === "event") {
    output(JSON.stringify({committed:true, accepted:true, reason:""}));
}
else if (args[1] === "title-instruction") {
    await new Promise(resolve => setTimeout(resolve, ${title_delay_ms}));
    output(JSON.stringify({completed:true, accepted:${!title_refused}, instruction_required:true,
        instruction:"Set the Logonomic title with logonomic_set_title for this task.", reason:""}));
}
else output(JSON.stringify({ok:true}));
`;
    await write_script(join(helpers, "pi_helper"), script);

    return {
        directory,
        helpers,
        ...observe_records(log),
        async dispose() {
            // Windows can hold a just-retired executable open while the process image is released.
            await rm(directory, { recursive: true, force: true, maxRetries: 12, retryDelay: 100 });
        },
    };
}

export function observe_records(log)
{
    async function records()
    {
        const bytes = await readFile(log, "utf8");
        return bytes.split("\n").filter(Boolean).map((line) => JSON.parse(line));
    }

    async function wait_for_record(predicate)
    {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => finish(new Error("Fixture helper record did not arrive.")), 10_000);
            const watcher = watch(log, inspect);
            let finished = false;
            function finish(error, record)
            {
                if (finished) {
                    return;
                }
                finished = true;
                watcher.close();
                clearTimeout(timer);
                if (error) {
                    reject(error);
                }
                else {
                    resolve(record);
                }
            }
            async function inspect()
            {
                try {
                    const record = (await records()).find(predicate);
                    if (record) {
                        finish(undefined, record);
                    }
                }
                catch (error) {
                    finish(error);
                }
            }
            void inspect();
        });
    }

    return {
        records,
        wait_for_record,
    };
}
