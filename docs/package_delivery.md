# Package delivery

`logonomic.cli.pi` is a native Logonomic CLI add-on package. Its manifest,
adapter, helper, capability declarations, extension asset and integrity index
travel together. The package integrates a separately installed Pi CLI.

## Export and activate

Build the package, then export it through its CMake install rule:

```sh
cmake --install ../pi-build --config Release --prefix ../pi-export
```

This creates `../pi-export/logonomic.cli.pi`. See [SDK package export](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#exporting-a-package) for destination and retry behavior.

Logonomic reads complete packages from these generic roots, in precedence order:

| Root | Location |
| --- | --- |
| Development | The directory selected by `development_root` in the profile's `[cli_addons]` settings section. |
| Per user | `<profile local-data root>/cli-addons/packages`. |
| Installation | `cli_addons` beside the Logonomic executable, unless the profile overrides `installation_root`. |

For example, a developer can point an isolated profile at a single-configuration
build's package directory:

```ini
[cli_addons]
development_root=/home/user/build/pi/package
```

The selected directory contains `logonomic.cli.pi` as an immediate child.
Logonomic freezes its package registry during startup. Close Logonomic before
placing or replacing the complete package in a root, then start it to consume
the new generation. A broken higher-precedence copy makes that package
unavailable; it does not select a lower-precedence copy.

For deployment, the installer owns atomic replacement of a package directory.
Do not overwrite the files of a package that another process may be ingesting.
The CMake export and SDK staging tools create artifacts; they do not update a
running application's package registry.

The root locations and startup behavior are defined by Logonomic's
[package architecture](https://github.com/Varinomics/logonomic/blob/master/docs/cli_addon_architecture.md)
and [root settings](https://github.com/Varinomics/logonomic/blob/master/src/logonomic_cli_addon_roots.h).

## Assemble an offline payload

Use the SDK staging tool when assembling a payload containing one or more
already-built packages. It copies the exact indexed bytes and records their
generation digests.

First validate the exported directory:

```sh
../pi-build/sdk/logonomic_cli_addon_validate /absolute/path/to/pi-export/logonomic.cli.pi
```

Create an artifact-list JSON file with `artifact_set_version` set to `1` and a
`packages` array. Each entry contains the absolute `directory` and the exact
`generation_digest` printed by that validator. The complete input format is in
the SDK's [artifact staging contract](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/artifact_staging.md).

With `artifacts.json` containing that list, stage and verify:

```sh
../pi-build/sdk/logonomic_cli_addon_stage \
  --output /absolute/path/to/payload/cli_addons \
  --input artifacts.json \
  --evidence /absolute/path/to/payload/cli-addons.evidence.json
../pi-build/sdk/logonomic_cli_addon_stage \
  --verify /absolute/path/to/payload/cli_addons \
  --evidence /absolute/path/to/payload/cli-addons.evidence.json
```

The output directory and evidence file must be absent, with their parent
directories already created. For a multi-configuration generator, use the
selected configuration's SDK tool paths; Windows executable names end in
`.exe`. This offline operation does not add the package to an official bundle
or activate it in a live profile.
