# Pi managed installer fixtures

These launchers are the generated upstream wrappers from
[install.sh](https://pi.dev/install.sh) and
[install.ps1](https://pi.dev/install.ps1), fetched on 2026-11-15.
The Windows placeholders are expanded with the installer's published package
and command names. Both documented runtime choices are represented: PATH Node,
and installer-selected standalone Node with PATH fallback if it is removed.
The scripts are kept independently of the resolver so tests execute the
upstream dispatch behavior. The installer itself is never executed by a test.

The source files have these SHA-256 digests:

| Source | SHA-256 |
| --- | --- |
| `install.sh` | `ff4ed7b1ea1c058f33249a44093863b50cf95224ab4ff654e722fcbbbdaa31df` |
| `install.ps1` | `3ba23d3451e17649495bdee14414ec1f3616ad7ae7246b4edd2e7166e7b3af51` |

Upstream code is MIT licensed; see [LICENSE](LICENSE).

The CMD runtime-selection cases use Microsoft's
[NeedCurrentDirectoryForExePath contract](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-needcurrentdirectoryforexepatha)
as the search-order oracle, including switch presence and the frozen working
directory. They compare distinct native executable files without executing CMD;
these portable contract checks do not qualify native Windows process behavior.
