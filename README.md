# Pi for Logonomic

Standalone Logonomic CLI add-on for the [Pi coding agent](https://github.com/earendil-works/pi).
It connects Pi's native sessions and tools to Logonomic's terminal integration.

Package identity: `logonomic.cli.pi`.

The adapter uses the public Logonomic CLI add-on SDK. Pi remains responsible for
its model configuration, credentials, tools, and native conversation storage.
The package integrates an existing Pi installation; it does not distribute Pi.

## Install and use

1. Install and configure Pi following its
   [upstream instructions](https://github.com/earendil-works/pi).
   The official managed installers and npm installations are supported.
   Standalone bundles must include Pi package metadata. A saved managed
   launcher follows Pi's active release after `pi update`.
   The interactive integration requires Pi **1.1.0 or newer**, whose native
   settled event includes cancellation evidence. Compatible newer versions have
   no version ceiling. Use the Node.js version required by your Pi installation.
2. Build this package as described below, then export it to a fresh directory:

   ```sh
   cmake --install ../pi-build --config Release --prefix ../pi-export
   ```

3. With Logonomic closed, place the complete exported `logonomic.cli.pi`
   directory in the profile's package root:
   `<profile local-data root>/cli-addons/packages/`. Preserve executable
   permissions and the complete package contents. A developer may instead point
   the profile's `[cli_addons] development_root` setting at a directory containing
   the package. See [package delivery](docs/package_delivery.md).
4. Start Logonomic and select the discovered Pi launcher. For a project-local
   npm install, include its `node_modules/.bin` directory in Logonomic's `PATH`
   before scanning, or save the absolute path to the Pi launcher. Logonomic
   identifies a saved launcher's provider and account before a project is
   selected, so a hand-written project-relative command runs as an ordinary
   shell command. The add-on attaches its managed extension to eligible
   interactive launches. Your Pi settings, other extensions, model choices and
   native session files remain under Pi's control.

Pi receives `logonomic_set_title` and `logonomic_show_changed_files` as native
tools. The first sets the terminal's task title; the second opens selected files
in Logonomic's diff viewer when you ask Pi to show its changes. Relative file
paths are resolved against Pi's current directory. Logonomic's title guidance
is supplied through Pi's model-context hook.

## Integration scope

| Facility | Behavior |
| --- | --- |
| Launcher discovery and recognition | Identifies official managed installations, npm installations and standalone bundles with Pi package metadata. Managed launchers retain Pi's active-release and runtime selection. |
| Interactive preparation | Adds the managed extension to supported interactive invocations; preserves the authored arguments. |
| Session and work state | Reports native session selection, work start, settled state, completion, failure and interruption. |
| Root model and reasoning level | Reports Pi's physical model and thinking level at session start, work start and selection changes. For virtual models the routed identity becomes known when a native assistant response supplies its physical model and effective thinking level. |
| Available models | Reports the live profile's configured registry at session start, work start and model selection, including the native total when the list is truncated. Pi can further restrict its picker using scoped models. |
| Native prompts | Pairs Pi's prompt start/end events so Logonomic can request and resolve attention. |
| Terminal tools | Supplies title guidance, terminal-title updates and changed-file presentation. |
| Logonomic-managed resume | Captures the exact native session file, inspects availability and content, and resumes that file with the selected Pi launcher and model settings. |
| Conversation export | Exports the persisted native branch as plain text, Markdown or highlighted conversation text, with full and compact projections. |
| Context occupancy and compactions | Reports Pi's current context estimate and counts installed compactions from freshly read native history. Unknown usage and incomplete history remain explicitly unavailable. |
| Execution directory | Reports Pi's native default tool directory for the exact selected session activation. |
| Profile storage and accounts | Identifies the exact launcher-selected Pi profile, reports its filesystem availability, and preserves Logonomic's account and managed-storage scope. |
| Authentication, quota and usage | No Logonomic capability is advertised. Pi owns its provider configuration. |
| Delegated tasks and idle wake | No capability is advertised. |

Help, version, print, export, RPC and package-management invocations are ordinary
Pi commands rather than integrated interactive sessions. Launches that disable
extensions or cannot be identified retain their ordinary launch behavior without
claiming lifecycle support.

Context occupancy comes from Pi's native context API, not cumulative token
billing. After compaction, Pi may report usage as unknown until a later assistant
response provides fresh evidence. Compaction counts include all persisted
branches of that native session; a lost or partial history withdraws completeness
without claiming that the count became zero. Context and directory observations
are optional: a failed query cannot suppress an otherwise valid work or session
event.

### Native profiles

An account identifies a Pi source profile directory. It does not identify a
remote provider login or attest who produced historical conversations. The
profile comes from the launcher's frozen `PI_CODING_AGENT_DIR` override, or from
the native home directory followed by `.pi/agent`, following
[Pi's profile configuration](https://github.com/earendil-works/pi/blob/main/packages/coding-agent/src/config.ts).
Discovery includes that selected profile even when it is outside the default
location. Its display name is the profile folder name; availability describes
the directory, independently of authentication or quota.

Windows profiles follow their containing directory's case rules. A saved
account whose old key could identify two profiles requires its original
profile path so Logonomic can verify which one it belongs to.

Relative profile paths use the host-supplied launch directory. Tilde expansion
uses the frozen native home, and ordinary local file URLs are supported. A path
the adapter cannot interpret exactly, or missing home evidence when it is
needed, produces an explicit unavailable result. No ambient adapter home is
substituted. Missing directories are unavailable; inaccessible directories
remain unknown. Discovery does not create a profile or read credentials.

When preparation replaces a supported shell command with direct Pi arguments,
it preserves the leading environment assignments explicitly. Their exact
host-evaluated values, including empty values, reach interactive and resumed
Pi processes; unrelated values from the frozen snapshot are not added to the
terminal environment.

Account-selected launch, resume and export verify the requested profile against
their frozen environment. An incompatible current launcher cannot substitute
its profile for a remembered account. Logonomic's allocated managed-storage
root is acknowledged separately and never replaces Pi's native profile path.

### Native history

Resume and export bind the native session ID to the absolute session filename
reported by Pi. They verify the file's header and parent tree before using it;
they never choose a recent file or a partial ID match. A missing file is absent.
Inaccessible, malformed, incomplete or unsupported history remains unknown.
Memory-only sessions and older Logonomic records without a native file locator
cannot be resumed or exported through this add-on. A new Pi session may have a
filename before Pi saves its first conversation message.

The reader supports Pi's version 2 and 3 session tree formats. Inspection is
read-only; it does not perform Pi's migration of older linear history. A complete
file containing only initialization metadata is reported as empty independently
of whether Pi can open it. Content inspection considers every retained branch;
selecting a metadata-only branch does not hide a conversation kept elsewhere in
the same file. Resume checks the exact file again when preparing the
launch, retains authored model, provider, tool and extension options, and removes
old session selectors and positional prompts before supplying `--session` with
the exact filename.

Export follows the ancestry of the last persisted entry, which is the branch Pi
restores when opening the file. An in-memory `/tree` selection that has not yet
appended an entry is not recoverable from disk. Full export preserves the
original participant transcript and includes tool activity, reasoning and
summaries on that branch; append-only context edits do not rewrite the original
conversation. Compact export retains user messages, final replies and assistant
text with unknown phase, plus explicit encrypted-content placeholders. Native
per-block text signatures identify commentary and final answers; the enclosing
response's stop reason does not override those phases. Highlighted export
preserves each known phase. Image
attachments are labeled; binary data and referenced files are not copied.
An unfinished final JSONL record may be omitted from an export, with that fact
reported in the result. Cancellation or invalid complete records publish no file.

## Build

Use CMake 3.25 or newer and a C++20 compiler. Linux package assembly also requires
Python 3.11 or newer and `patchelf`. Node.js runs the native extension fixtures. The SDK
dependency tracks Logonomic `master`; the commands below use a local SDK checkout
so its source location is explicit:

```sh
cmake -S . -B ../pi-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/logonomic/sdk/logonomic_cli_addon_sdk
cmake --build ../pi-build --config Release --parallel 2
ctest --test-dir ../pi-build -C Release --output-on-failure
```

Omit `LOGONOMIC_CLI_ADDON_SDK_SOURCE` to let CMake fetch the SDK from the Logonomic
repository using your configured Git access. With a multi-configuration
generator, select `Release` when building, testing and exporting.

The `pi_package` target assembles the indexed package at
`<build>/package/logonomic.cli.pi`; a multi-configuration generator adds the
configuration directory before `logonomic.cli.pi`. The SDK validator and
conformance tests check the delivered package, and the provider tests exercise
the launcher and native extension contracts.

The package recipe supports Windows/Linux x86-64 and macOS ARM64 toolchains.
Each platform requires its own native qualification; Linux evidence does not
qualify Windows or macOS process paths. The optional native Pi smoke fixture
uses an isolated profile and a local fake model, with no provider credentials
or paid model calls:

```sh
node tests/pi_runtime_smoke.mjs /absolute/path/to/pi/dist/bundle/cli.js
```

The standalone command uses a POSIX executable helper. On Windows, set
`PI_TEST_PI_CLI` at configure time and run the registered CTest entries so the
native test helper is available. These tests exercise Pi's actual loader and
event API against an isolated host fixture.

Set `PI_TEST_PI_CLI` to the installed bundled CLI entrypoint when configuring
CMake to include the native persistence and resume smoke test. It creates history
with Pi's actual `SessionManager`, prepares a resume through the packaged adapter,
and runs the packaged SDK launch bridge. It appends RPC mode only to inspect the
restored session through Pi's public transport, with an isolated profile and no
model requests; desktop terminal behavior still requires native host qualification.

See [package delivery](docs/package_delivery.md) for export, offline payload
staging and package-root details, and the
[Logonomic CLI add-on SDK](https://github.com/Varinomics/logonomic/tree/master/sdk/logonomic_cli_addon_sdk)
for the host contracts.
