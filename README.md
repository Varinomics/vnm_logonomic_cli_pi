# Pi for Logonomic

Standalone Logonomic CLI add-on for the [Pi coding agent](https://github.com/earendil-works/pi).
It connects Pi's native sessions and tools to Logonomic's terminal integration.

Package identity: `logonomic.cli.pi`.

The adapter uses the public Logonomic CLI add-on SDK. Pi remains responsible for
its model configuration, credentials, tools, and native conversation storage.
The package integrates an existing Pi installation; it does not distribute Pi.

## Install and use

1. Install and configure Pi using the npm alternative in its
   [upstream instructions](https://github.com/earendil-works/pi):

   ```sh
   npm install -g --ignore-scripts @earendil-works/pi-coding-agent
   ```

   Discovery currently supports npm installations and standalone bundles with
   package metadata. Pi's managed installer layout is not yet recognized.
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
4. Start Logonomic and select the discovered Pi launcher. The add-on attaches
   its managed extension to eligible interactive launches. Your Pi settings,
   other extensions, model choices and native session files remain under Pi's
   control.

Pi receives `logonomic_set_title` and `logonomic_show_changed_files` as native
tools. The first sets the terminal's task title; the second opens selected files
in Logonomic's diff viewer when you ask Pi to show its changes. Relative file
paths are resolved against Pi's current directory. Logonomic's title guidance
is supplied through Pi's model-context hook.

## Integration scope

| Facility | Behavior |
| --- | --- |
| Launcher discovery and recognition | Identifies npm installations and standalone bundles with Pi package metadata, then resolves their native entrypoints. |
| Interactive preparation | Adds the managed extension to supported interactive invocations; preserves the authored arguments. |
| Session and work state | Reports native session selection, work start, settled state, completion, failure and interruption. |
| Native prompts | Pairs Pi's prompt start/end events so Logonomic can request and resolve attention. |
| Terminal tools | Supplies title guidance, terminal-title updates and changed-file presentation. |
| Accounts, quota and usage | No Logonomic capability is advertised. Pi owns its provider configuration. |
| Delegated tasks, idle wake and Logonomic-managed resume | No capability is advertised. Pi's own new, resume and reload operations remain native operations whose lifecycle is observed. |

Help, version, print, export, RPC and package-management invocations are ordinary
Pi commands rather than integrated interactive sessions. Launches that disable
extensions or cannot be identified retain their ordinary launch behavior without
claiming lifecycle support.

## Build

Use CMake 3.25 or newer and a C++20 compiler. Linux package assembly also requires
Python 3.11 or newer and `patchelf`. On Linux and macOS, Node.js also runs the
native extension fixtures. The SDK
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

That fixture uses a POSIX executable helper and runs on Linux or macOS. It
exercises the actual Pi loader and event API; its helper fixture does not
substitute for qualification inside the desktop application.

See [package delivery](docs/package_delivery.md) for export, offline payload
staging and package-root details, and the
[Logonomic CLI add-on SDK](https://github.com/Varinomics/logonomic/tree/master/sdk/logonomic_cli_addon_sdk)
for the host contracts.
