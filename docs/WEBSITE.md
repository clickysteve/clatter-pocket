# The website

A static site for GitHub Pages, at <https://clatter-pocket.allmyfriendsaresynths.com/>, the custom
domain set in the repository's Pages settings; `clickysteve.github.io/clatter-pocket/` redirects to
it. No framework, no dependencies beyond Node 20, no backend, no tracking.

**The site must be used over HTTPS.** On plain HTTP a browser removes Web MIDI, `crypto.subtle` and
`AudioWorklet`, so neither the installer nor the demo can work. Each page moves itself to HTTPS.
All paths are relative, so the site works at a domain's root and under `/clatter-pocket/`.

```
make site        # build into build/site
make site-test   # the site's tests
make site-serve  # http://127.0.0.1:8770/clatter-pocket/, a base path, to catch paths from the root
```

| Path | |
|---|---|
| `site/index.html`, `styles.css`, `app.js` | the page. `<!-- @name -->` and `{{name}}` are filled by the build |
| `site/assets/controls.mjs` | the control guide's data and the panel drawing, used by the build and the browser |
| `site/release.json` | **the public release**: the only thing that says what may be installed |
| `releases/<version>/` | the installable packages. Copied to the site only if `site/release.json` names one |
| `site/installer/gate.mjs` | the rule, used by the build and the browser |
| `site/installer/index.html`, `installer.js` | the installer's interface |
| `site/installer/driver.js` | the update driver: **left out of the site while the gate is closed** |
| `tools/promote-release.mjs` | name a package as the installable release, after checking it |
| `tools/fetch-open-firmware.mjs` | get official Open from M-VAVE's release at deployment, by hash. Never committed |
| `tools/fm1-update-sim.mjs` | a simulated FM-1 at the Web MIDI level, for the installer's tests |
| `site/demo/` | the demo page; `emu/` is the vendored emulator; `emulator-build.json` pins what it runs |
| `tools/build-site.mjs`, `test-site.mjs`, `serve-site.mjs` | build, tests, a server with the base path |
| `tools/promote-emulator-build.mjs` | put a package on the demo, by hand |
| `.github/workflows/deploy-pages.yml` | test and publish `site/` from `main` |

The research behind it is in [FM1-COMMUNITY.md](FM1-COMMUNITY.md).

## The demo

The deployed emulator cannot load a package that is not in its catalogue, and CLATTER Pocket cannot be
in its catalogue yet ([why](FM1-COMMUNITY.md#the-firmware-hubs-rules-and-clatter-pocket)). So the site
carries its own copy, **with no change to the emulator's code**:

- `site/demo/emu/` holds fm1-emu's page files at commit `93cb7ee` and the matching `fm1_web.wasm`, from
  the deployed site, which was built from that commit: on 2026-10-30 the deployed `main.js` equalled
  the commit's, and the wasm is from the same deployment. Its `LICENSE` is kept beside them.
- Only `firmwares.json` is ours: one entry, using the catalogue format fm1-emu already has.
- The demo page frames `emu/?fw=clatter-pocket&mhz=240&embed=1`, three parameters fm1-emu documents.
  240 MHz because the page's default of 100 is not the FM-1's clock.

The emulator was not changed, so there is no fork to maintain. To update it: copy `index.html`,
`style.css`, `main.js`, `persistence.js`, `worklet.js`, `jit.js`, `fwsc.js` and `LICENSE` from a commit
of fm1-emu, its wasm from the same deployment, change the commit in
`tools/promote-emulator-build.mjs`, promote, and run the tests.

To change the firmware the demo runs:

```
node tools/promote-emulator-build.mjs build/fm1-replace/clatter-pocket-fm1-replace-UNVERIFIED.fwsc <commit>
```

Nothing does this automatically. CI cannot build firmware, and the site never reads `build/`.
fm1-emu keeps the package a browser first ran, with its saves, so a returning visitor stays on the old
build until they press its Check for firmware updates. The demo page says so; new visitors get this one.

The package is stored as `clatter-pocket-emulator-only.bin`: the bytes are a real package, which a
determined visitor could rename, and the name is there to stop an accident rather than a person.

## Three kinds of build

| | Where | Reaches the site by |
|---|---|---|
| Development builds | `build/`, never committed | never |
| Emulator build | `site/demo/emu/`, pinned by SHA-256 in `emulator-build.json` | `promote-emulator-build.mjs`, by hand. Runs in the demo only |
| Release | `releases/<version>/`, named in `site/release.json` | `promote-release.mjs`, by hand. Installable |

## The installer

It is real: it transfers firmware to a device. It drives M-VAVE's unchanged `sdk/WebUpdate.js`, over
USB-MIDI SysEx through Web MIDI. CLATTER Pocket always goes with `force: false`, as
`tools/make-installer-page.py` does. The one forced route is official Open over stock V15, below.

### From stock: two stages

Connect classifies the FM-1 from what the protocol reports, and writes nothing:

| Reports | Means | Offered |
|---|---|---|
| Identity `FM-1_015` | stock V15, which has no build to read | Stage 1 |
| Family 7, build = the pinned Open's | official Open | Stage 2, with Stage 1 skipped |
| Family 7, build = this release's | CLATTER Pocket | nothing: already installed |
| Family 7, any other build | fm1_open firmware: another Open, or an older CLATTER Pocket | Stage 2, as before this page had stages. It says what it is not |
| `ota-FM-1_015` | the vendor's loader: an install was interrupted | resume |
| Anything else, or no capability reply | unknown | nothing |

CLATTER Pocket reports Open's identity on purpose, so only the hash tells them apart.

**Stage 1 is M-VAVE's own route.** Its manual says that when a normal connect recognises another
firmware, such as `FM-1_015`, you turn on forced install, reconnect, and confirm: stock has no
capability command, so `WebUpdate.js` keeps a `legacy` route for it, behind `force`. The driver
reaches it in exactly one function, only after a normal connect found stock and a person confirmed.
It then asks again, and only the exact stock report goes on. Force also drops the updater's port
filter, so the driver hides other MIDI devices from it. `tools/test-site.mjs` pins all of this.

After Stage 1 the updater has checked Open's family and exact build. The page then connects again,
unforced, and offers Stage 2 only on the pinned Open build. Stage 2 has its own confirmation.

**Interrupted installs.** Both packages use the vendor's loader, which reports one fixed identity, so
the device cannot say what it was receiving. The page records which package it is writing in
`localStorage`, and clears the record once a verified install ends or a device turns up that is not
mid-update. In the loader, with that record for this release's CLATTER Pocket, Stage 2 resumes.
Otherwise official Open is resumed, unforced and confirmed, and then verified: a browser with no
record must not put CLATTER Pocket on a device that may have come from stock. Nothing is retried
automatically: each failure ends at Connect, which finds out what the device is.

### Official Open is not in this repository

`site/release.json`'s `openFirmware` pins one release of [m-vave/fm1_open](https://github.com/m-vave/fm1_open).

- A browser cannot fetch it: GitHub's release downloads send no CORS header, the API allows 60 requests
  an hour per address, and the firmware is not in the source tree.
- It is published only inside the release's workbench page. The development kit holds V15, not Open.
- So `tools/fetch-open-firmware.mjs` runs at deployment. It checks `SHA256SUMS-V14` against the pin,
  the workbench against both, takes out the workbench's package, and checks every field of it and the
  firmware's hash. `FM1Web.prepare` checks it again at build and in the browser.
- The result lives in `build/upstream/`, and in the workflow's cache, and goes out with the site.
  A cached copy is checked before use. Any difference stops the build, so the deployed site stays
  as it was. There is no live dependency on GitHub when someone installs.

**Updating it** is a decision, not an automatic step: read the new release's notes, take the new
hashes and fields from its workbench, update `openFirmware`, and check that CLATTER Pocket's identity,
family, loader and flash layout still match it. The gate refuses a pin they do not match.

`site/release.json` drives the rest. `status` is `emulator-only`, `experimental` or `hardware-verified`:

| Status | Shown as | Can be installed | Needs |
|---|---|---|---|
| `emulator-only` | UNVERIFIED: EMULATOR ONLY | no | |
| `experimental` | UNVERIFIED: EXPERIMENTAL | when enabled | the checks below, and UNVERIFIED in the package's name |
| `hardware-verified` | HARDWARE VERIFIED | when enabled | the same, and a verification record for this package's hash |

Hardware verification is not needed to install an experimental release: installing it is how the
verification happens. **Nothing is installable unless `installEnabled` is exactly `true`** and one
package is named, so a new build never becomes installable by itself.

The gate (`site/installer/gate.mjs`) needs, for either kind: a release date and notes; a firmware
with a `clatter-pocket-*.fwsc` name, SHA-256, build identity, loader hash, size, fm1_open identity and
family 7; a record that the build was `--single-precision` and passed `check-firmware.py`,
`check-audio-path.py` and the emulator suite; a pinned official Open from `m-vave/fm1_open` with the
same identity, family and loader; and compatibility requirements.

What still stops an install, whatever the status:

| When | Check |
|---|---|
| Fetching official Open | the checksum file and workbench against the pin; every field of the package; the firmware's SHA-256 |
| Building the site | the gate; each file's size and SHA-256; `FM1Web.prepare` on both real files |
| Opening the page | the same gate; `FM1Web.prepare` on both: SHA-256, package format, CRCs, identity, loader hash |
| Connect | the table above. Unknown firmware stops here, with nothing written |
| Stage 1 | the explicit confirmation; the exact stock identity reported again on the forced connect; only official Open |
| Install | the explicit confirmation; identity and capability read again; the device's own acceptance of the package |
| While writing | every read must be inside the package; the stages must match; a stall or lost connection stops it |
| After restart | the device must report the exact build written. Stage 2 then needs the pinned Open on a fresh, unforced connect |

While the gate is closed, the build leaves out `driver.js`, M-VAVE's update code, official Open and any firmware, and
**stops with an error** if the output contains a firmware file or anything that could request MIDI
access outside the vendored emulator. If `installEnabled` is true and the gate refuses the release,
the build fails rather than quietly staying off. To turn installing off, set `installEnabled` to
`false` and `status` to `emulator-only`: the pages change with it.

The emulator can use Web MIDI for its MIDI input jack: it asks without SysEx, only when a visitor picks
an input, and carries no update code.

## Testing the installer

`make site-test` runs M-VAVE's real update code and the real package against
`tools/fm1-update-sim.mjs`, which plays the device at the MIDI port level: complete installs, comparing
every byte written; every refusal; and faults such as a lost cable, a refused package or a device that
restarts on another build.

**The simulator was written from the client's code, not from a device.** It shows that the installer
behaves. It cannot show that a real FM-1 will accept the transfer, or that the firmware starts.
Emulators cannot either: fm1-emu raises no USB interrupt, so the update path has never been run.

That goes double for Stage 1. The simulator's stock V15 is what `WebUpdate.js`'s comments say stock
is: an identity and a request-driven updater, with no capability or arm command. Its restart adopts
the identity of whichever package it was sent. Nobody here has seen a real V15 do any of it.

## Making a release

1. Commit the firmware. Build from that commit, on a clean tree:
   `firmware/fm1/build.sh --replace --single-precision`.
2. Run `python3 tools/check-firmware.py`, disassemble for `tools/check-audio-path.py`, and
   `make emu-test`, into a log that also has the package's `shasum -a 256`.
3. `node tools/promote-emulator-build.mjs <package> <commit>`, so that the demo runs the same bytes.
4. `node tools/promote-release.mjs <that log>`. It repeats the checks, refuses an outdated or changed
   build, copies the package to `releases/` and writes `site/release.json`.
5. `make site-test`, commit, and merge to `main`.

**A firmware commit on `main` stops the site's deployment until this has been done**: a test compares
the release with the firmware sources, so the site cannot silently go on offering an outdated build.

To mark a release verified, after FIRST-HARDWARE-TEST.md has passed for this exact package, including
leaving it for official firmware: give it a name without UNVERIFIED, set `status` to `hardware-verified`,
add `verification`, and change the pages' words. The tests pin today's status, on purpose: change them
in the same commit.

## Screenshots

`docs/website/` has the desktop and mobile layouts as of the first version of the site.

## Deployment

The workflow runs on `main`, and on demand. It needs **Settings → Pages → Source: GitHub Actions**.
Pull requests run the tests and publish nothing. Links into this repository point at `main`, and the
tests check that their files and headings exist.
