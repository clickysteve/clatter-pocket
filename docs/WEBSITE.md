# The website

A static site for GitHub Pages, at <https://clickysteve.github.io/clatter-pocket/>. No framework,
no dependencies beyond Node 20, no backend, no tracking.

```
make site        # build into build/site
make site-test   # the site's tests
make site-serve  # http://127.0.0.1:8770/clatter-pocket/, the base path Pages will use
```

| Path | |
|---|---|
| `site/index.html`, `styles.css`, `app.js` | the page. `<!-- @name -->` and `{{name}}` are filled by the build |
| `site/assets/controls.mjs` | the control guide's data and the panel drawing, used by the build and the browser |
| `site/release.json` | **the public release**: the only thing that says what may be installed |
| `site/installer/gate.mjs` | the rule, used by the build and the browser |
| `site/installer/index.html`, `installer.js` | the installer's interface |
| `site/installer/driver.js` | the update driver: **left out of the site while the gate is closed** |
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
The package is stored as `clatter-pocket-emulator-only.bin`: the bytes are a real package, which a
determined visitor could rename, and the name is there to stop an accident rather than a person.

## Three kinds of build

| | Where | Reaches the site by |
|---|---|---|
| Development builds | `build/`, never committed | never |
| Emulator build | `site/demo/emu/`, pinned by SHA-256 in `emulator-build.json` | `promote-emulator-build.mjs`, by hand. Runs in the demo only |
| Public release | `releases/<version>/`, named in `site/release.json` | a release, below |

## The installer

`site/release.json` drives it. `status` is `emulator-only`, `experimental` or `hardware-verified`.

The gate (`site/installer/gate.mjs`) opens only if **all** of these hold: status `hardware-verified`;
`installEnabled` exactly `true`; a release date and notes; a firmware with a `clatter-pocket-*.fwsc`
name without UNVERIFIED, SHA-256, build identity, loader hash, size, fm1_open identity and family 7; a
verification record with date, units, an HTTPS report and `recoveryTested`, whose `firmwareSha256`
equals the firmware's; and compatibility requirements.

That last match is the point: a verification belongs to one package, by hash. A new build cannot
inherit it, and a firmware file sitting in the repository changes nothing.

While the gate is closed, the build leaves out `driver.js`, M-VAVE's update code and any firmware, and
**stops with an error** if the output contains a firmware file or anything that could request MIDI
access outside the vendored emulator. So the published site cannot reach a device: there is no flag to
flip in a browser. If `installEnabled` is true and the gate is closed, the build fails rather than
quietly staying off.

The emulator can use Web MIDI for its MIDI input jack: it asks without SysEx, only when a visitor picks
an input, and carries no update code.

When the gate is open, `driver.js` drives M-VAVE's unchanged `sdk/WebUpdate.js` with `force: false`,
exactly as `tools/make-installer-page.py` does. `FM1Web.prepare` then checks the package's SHA-256,
identity and loader in the browser.

## Making a release

Do this only after FIRST-HARDWARE-TEST.md has been completed for this exact package, including
leaving it for official firmware.

1. Commit the package as `releases/<version>/clatter-pocket-<version>.fwsc`.
2. Fill in `site/release.json`: `version`, `releaseDate`, `firmware`, `notes`, and `verification` with the
   hash of that file and a link to the published report. Then set `status` and `installEnabled`.
3. **Change the page's words.** `index.html` and the FAQ state the current status in prose, and only you
   can decide what is true of the new one: the status strip, Install, Recovery, "Is installation
   currently safe?", and the table in Source.
4. `make site-test`. The first tests will fail on purpose, because they pin today's status. Change them
   in the same commit, so that the change is on the record.
5. Merge to `main`.

## Screenshots

`docs/website/` has the desktop and mobile layouts as of the first version of the site.

## Deployment

The workflow runs on `main`, and on demand. It needs **Settings → Pages → Source: GitHub Actions**.
Pull requests run the tests and publish nothing. Links into this repository point at `main`, and the
tests check that their files and headings exist.
