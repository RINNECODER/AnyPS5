# Engine releases and update channels

The fork publishes signed engine builds for MacPS on three channels. MacPS installs from
the channel picked in its Settings (default `alpha`), verifies everything below before it
touches the installed engine, and always keeps the previous version for a one-click
rollback.

| Channel | What lands there | How |
|---|---|---|
| `alpha` | Every push to `main` whose four required macOS checks passed | `.github/workflows/release-alpha.yml` → `tools/release/alpha.py` |
| `beta` | An alpha that is at least 24 h old, passed the required checks, MacPS acceptance and the local PPSA04203 smoke (boot stage not below the current beta), with no open `regression` issue against it | hourly `.github/workflows/release-promote.yml` → `promote.py beta` |
| `stable` | A beta the maintainer approved | `promote.py stable-candidate` opens an issue; the maintainer adds `release:approved` |

## What gets published

Everything is public on RINNECODER/AnyPS5 and downloads need no authentication.

**Build releases.** Each build is an immutable GitHub prerelease tagged `engine-<YYYY.MM.DD>.<n>`, where `n` counts up per UTC day. It has three assets:

- `anyps5-engine-<version>-macos-arm64.zip`: the engine package. The zip root is the package directory itself (`manifest.json`, `SHA256SUMS`, `bin/`, `lib/`, `fixtures/`), with Unix modes preserved.
- `release.json`: the build record (schema 1). It holds the version, commit, build time, the zip's URL, SHA-256 and size, `package_manifest_sha256` and short notes.
- `release.json.sig`: the signature of `release.json`.

A build that reaches `stable` stops being a prerelease.

**Channel feeds.** The rolling `channels` release holds `engine-alpha.json`, `engine-beta.json` and `engine-stable.json`, each with a `.sig`. The `macps-*.json` feeds for the app live there too. A feed is:

```json
{"schema": 1, "product": "engine", "channel": "alpha", "sequence": 42, "updated_at": "...", "release": {"...": "the release.json object"}}
```

`sequence` increases strictly per product and channel. The tooling refuses to point a feed at a build that is not newer than the one it already names. Feed URL: `https://github.com/RINNECODER/AnyPS5/releases/download/channels/engine-<channel>.json`.

## Signing

Signatures are Ed25519 over the exact bytes of the `.json` file. The `.sig` file holds the base64 of the 64-byte signature, followed by a newline.

Public key (base64, raw 32 bytes):

```
8CH+bArwdc+38zujG9xU/pD1W7tRoi5UAeGSschuCpc=
```

The private key exists only on the maintainer's Mac, as a mode-0600 file outside the repository. Self-hosted runner jobs and local scripts read it from there. It is never committed, printed, uploaded or stored as a GitHub secret.

The signer, `tools/release/signer.swift`, uses CryptoKit. Every signature is re-checked before upload by the independent RFC 8032 verifier in `verify.py`, and the signer refuses to run with a key whose public half doesn't match the key above.

## How a package is built

`tools/release/build_package.py` runs the existing `tools/prepare_diagnostic_engine.py --profile native` unchanged. That one run covers:

- a fresh clone and build at the exact commit;
- the full declared native CTest inventory;
- packaging through `tools/diagnostic_package.py`;
- relocated fixture runs;
- MacPS's production `EnginePackage.accept`, built from a fresh clone of the MacPS repository, with negative controls.

`build_package.py` then adds three steps of its own:

1. It rejects any package byte that contains the build user's home path or user name, because the binaries are public.
2. It writes a deterministic zip.
3. It unzips the zip with `ditto` and runs `EnginePackage.accept` again on the round-tripped bytes, pinned to the manifest SHA-256.

## Verifying a channel yourself

`verify.py` is one self-contained, stdlib-only file. Copy it anywhere and run it:

```bash
python3 tools/release/verify.py https://github.com/RINNECODER/AnyPS5/releases/download/channels/engine-alpha.json \
    [--min-sequence N] [--no-download] [--extract new-dir]
```

It checks, in order:

1. The feed signature.
2. The schema, product, channel and minimum sequence.
3. The build's own signed `release.json`, which must equal the feed's `release` object.
4. The zip's size and SHA-256.
5. With `--extract`, the extracted manifest SHA-256.

To run MacPS's acceptance on a downloaded zip or an extracted package:

```bash
python3 tools/release/macps_accept.py anyps5-engine-<version>-macos-arm64.zip --macps-repo <MacPS checkout>
```

## Promotion rules

**Beta: `promote.py beta`, run hourly.** It takes the newest verified alpha that is newer than the current beta and at least 24 h old. Every one of these gates must pass:

1. The four required macOS checks succeeded on that commit.
2. MacPS accepts the downloaded zip.
3. The PPSA04203 smoke reaches a boot stage at least as far as the current beta's.
4. No open `regression` issue is filed against it.

If a gate fails, the run reports the reason and waits for the next hour.

**The smoke.** The title doesn't boot yet, so the smoke records the furthest boot stage reached rather than requiring a pass. The stages, in order, are: `no_diagnostics`, `launch_rejected`, `load_rejected`, `dependency_init_failed`, `entered_main`, `guest_exit`.

- The candidate and the current beta run side by side on the runner, under the shared `title-session` lease.
- The title dump's location comes only from the runner `.env` (`ANYPS5_E2E_TITLE_DIR`). Its paths and hashes are never committed or logged.

**Regression issues.** A `regression` issue counts against every version up to and including the oldest version it mentions, in the form `YYYY.MM.DD.n`. An issue that mentions no version blocks every promotion. Close the issue, or edit it to name the right version, to unblock.

**Stable candidate: `promote.py stable-candidate`.** Once a beta has been on the beta channel for 48 h with no blocking regression, the promoter opens one issue:

- Title: `Promote engine-<version> to stable`.
- Label: `release:stable-candidate`.
- The issue body carries a `<!-- release-candidate: engine <version> -->` marker.

When a newer beta arrives, the promoter closes the older candidate issues it opened.

**Stable: `promote.py stable`.** This runs on the `issues: labeled` event and also hourly, so an approval is never lost. It promotes only an open issue that meets all of these conditions:

- It is labelled both `release:stable-candidate` and `release:approved`.
- It was opened by the promoter or the maintainer, and its title and marker match.
- The most recent `release:approved` label event on it was made by **RINNECODER**.
- Its version reached beta, is newer than the current stable, and has no blocking regression.

The promoter then writes `engine-stable.json`, marks the build as a full release, and closes the issue with a comment.

> **Agents must never add the `release:approved` label.** Only the maintainer approves stable. Agents may open
> candidate issues, file `regression` issues, and report evidence; they never approve.

## Rolling back

Rollback is local: in MacPS Settings, choose the previous engine. MacPS always keeps `current` and `previous` installed. A feed is never downgraded, because clients reject a lower `sequence` and would also reject a replayed old feed.

To get a channel off a bad build:

1. Fix forward: merge a revert to `main`, and the next alpha supersedes the bad one.
2. Open a `regression` issue that names the bad version. This holds beta and stable promotion until it is closed.
3. If a bad build already reached beta or stable, promote a newer fixed build. As an emergency, you can re-sign a feed that points at an older, known-good build under a new, higher sequence. Do this deliberately and by hand: the tooling refuses it on purpose.

Do not delete build releases that a feed points to.

## Runner setup

The jobs run only on the self-hosted `anyps5-metal` runners. Every job keeps the same-repository guard, so fork code never runs on the Mac.

Each runner's `.env` provides:

| Variable | Purpose |
|---|---|
| `ANYPS5_MACPS_REPO` | MacPS git checkout. It is cloned, never modified, to build the `EnginePackage.accept` helper. |
| `ANYPS5_E2E_TITLE_DIR` | Local title dump used by the smoke. |
| `ANYPS5_UNOBTRUSIVE_WINDOWS=1` | Test window behaviour. |
| `ANYPS5_RELEASE_KEY` | Optional. Overrides the default signing key location. |

PRs that touch release tooling run the `Release tooling preflight` job. It runs `tools/tests/test_release_channels.py` and checks that the runner can derive the public key and read both paths.
