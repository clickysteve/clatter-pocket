# The FM-1 firmware community: what the website was built on

Researched 2026-10-30. Sources are linked; **reported** means a project's own statement, not
checked here. No device was used for this research; what has been seen on one since is noted where
it answers something, and is in FIRST-HARDWARE-TEST.md.

## How firmware is distributed

- **There is a large community.** [fm1-emu](https://github.com/joakimeriksson/fm1-emu) catalogues about
  70 firmwares, most of them forks of or built on [Felucca](https://github.com/hugelton/Felucca), the
  first open firmware, or [Dinghy](https://github.com/zvenson/dinghy), a documented base. This replaces
  HARDWARE.md's earlier finding that none was found.
- **The usual landing page** has a description, a "try" link into an emulator, and an install page on
  the project's GitHub Pages, with releases on GitHub. [AMB-1](https://github.com/charlesvestal/fm1-amb),
  [FiMba-1](https://github.com/jadamsowers/fm1-fimba) and [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1) are typical.
- **[The FM-1 Firmware Hub](https://thefirmwarehub.com/)** (zvenson) is the shared catalogue: one-click
  install of open firmware from Chrome or Edge, with a checksum against its catalogue. It is not
  affiliated with M-VAVE, and does not test firmware itself.

## The update protocol

**USB-MIDI SysEx through Web MIDI. Not WebUSB.** Verified in `third_party/fm1_open/sdk/WebUpdate.js`;
the [official workbench](https://open.m-vave.com/fm1-workbench), the Hub and community installers all
describe the same route:

1. Chrome or Edge on a computer, HTTPS or localhost, MIDI with SysEx allowed.
2. The running firmware answers an identity and capability query (`FMC1`: family, state, build).
3. It is told to start the update loader in the new package, and re-enumerates as `ota-…`.
4. The loader writes the image, block by block, and the device restarts.
5. The page checks that it reports the new build.

So the running firmware's co-operation is what makes an update possible. `sdk/WebUpdate.js` is
`GPL-3.0-only WITH GPL-3.0-linking-exception`, and works as a plain script with no build step.

## Can an installer be reused?

**Yes, and this repository already does.** `tools/make-installer-page.py` drives M-VAVE's `sdk/WebUpdate.js`,
unchanged, and the website's installer (`site/installer/driver.js`) is the same logic. No protocol was written
here. M-VAVE's code is the source; a community installer would only be a copy of it. The Hub links no
source for its own installer.

`force: false` keeps M-VAVE's safety checks: it installs only over firmware reporting an fm1_open
identity and update family 7, or resumes an interrupted install of the same package. The consequence
for users: **from V15 they first need official Open**, as FIRST-HARDWARE-TEST.md says. Since
2026-10-31 the installer does that step itself ([how](WEBSITE.md#from-stock-two-stages)).

## Official Open: what M-VAVE publishes

Checked 2026-10-31 against [m-vave/fm1_open](https://github.com/m-vave/fm1_open) and its V14 release.

| Question | Finding |
|---|---|
| Where is it published? | GitHub Releases, tags `V12` and `V14`. Each has a workbench page, a development kit and a `SHA256SUMS-Vn` |
| Is the binary a release file? | **No.** Open's `.fwsc` exists only as base64 inside `FM1-Open-V14-Workbench.html` (`FM1_PACKAGE`). The development kit and the source tree hold stock V15, `FM-1_015.fwsc`, not Open |
| Which is compatible? | V14: build `de48badc…`, identity `FM-1_8000014`, family 7, vendor loader `ef1f2db8…`. CLATTER Pocket shares its family, loader and flash layout, and is built from `fm1_open` just after V14. Until 0.9.4 it shared the identity too; since 1.0 it reports its own, `FM-1_8133700` |
| Can a browser fetch it? | No. Release downloads send no `Access-Control-Allow-Origin`. The API does, at 60 requests an hour per address, but its asset URLs lead to the same downloads |
| How is the updater related? | The `WebUpdate.js` in the V14 workbench is byte-identical to the one pinned here |
| Is the route documented? | Yes, in the manual inside the workbench, chapter 9: from another firmware such as `FM-1_015`, enable forced install, reconnect, confirm. It warns of lost data and of failed boots needing special tools |
| Does stock need a native program? | No, by M-VAVE's client code and manual: USB-MIDI SysEx through Web MIDI, both stages. No workbench is needed for it. **Seen on one device:** 0.8.2 went on from stock through both stages in a browser |
| What does the package say of itself? | `hardware_validated: false`. `sdk/Build.ps1` writes that for every package it builds, so it says nothing either way about V14 |

M-VAVE's terms allow all this. The engine may be redistributed "as part of firmware" for FM-1 devices,
and the boot files and V15 image under Apache-2.0, with their notices. `LICENSE.md` and
`THIRD_PARTY.md` travel with the site, and `legal/fm1_open-SOURCE.txt` points to the corresponding
source of the Open build it serves.

## Recovery in the community

| Route | What it is | Source | With CLATTER Pocket |
|---|---|---|---|
| Official V15 restore | Installs M-VAVE's V15 over USB-MIDI | workbench, Hub | the documented route; untested |
| OCT− at power-on | "USB rescue": no sound, only the updater. Two crashes at boot enter it too | Dinghy, FANTASMA, the Hub | **unknown**: nothing in this repository implements it. Dinghy and FANTASMA describe it as their firmware's feature; the Hub describes it as working for any firmware. Do not rely on it |
| ROM bootloader | A USB disk "WL82 UBOOT1.00" appears; no firmware can overwrite that bootloader | [Hub](https://thefirmwarehub.com/kb/#rescue) | reported; untested |
| `fm1_rescue.py` (Linux), [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) (Windows, Linux, MIT) | Write the firmware area through that bootloader, with verification and a full backup | Hub | reported; untested |
| FM-1 Transporter | An RP2040 board on the USB lines, for units that never show that disk | MvaveFM1Unbricker's guide | reported; untested |

CLATTER Pocket's own measure is **SAFE MODE**: hold HOME 1.5 s after power-on, and the engine stays off
with the updater running (FM1-REPLACEMENT.md).

This changes one thing in HARDWARE.md: a ROM recovery route does exist in the community, for units that
will not boot. It is not M-VAVE's, and nobody here has used it.

## The emulator

[fm1-emu](https://github.com/joakimeriksson/fm1-emu) (MIT, Joakim Eriksson) runs packages unchanged in
WebAssembly. Its page takes `?fw=<catalogue id>`, `?mhz=`, and `?embed=1` for a window on another site.
Packages come from its catalogue, or a file the visitor opens. **There is no way to pass it a URL.**
The deployed page's catalogue is generated from the Hub's list, so a firmware can only appear there
once it is on the Hub, and it cannot be there yet (below).

## The Firmware Hub's rules, and CLATTER Pocket

From [its submission page](https://thefirmwarehub.com/submit/):

| Requirement | CLATTER Pocket |
|---|---|
| Tested on a real FM-1, installed from V15 and from another firmware, and leaving it again. "Emulator runs and builds don't count" | **Met for 0.9.4** on one FM-1: installed from V15 through official Open and over earlier CLATTER Pocket builds, left for V15 and installed again ([report](https://github.com/clickysteve/clatter-pocket/issues/1)). 1.0's own identity needs the same once more |
| A release with a `.fwsc`, containing the update loader | the package has the loader; the GitHub release comes with 1.0 |
| Open licence, complete source public, in the repository at each tag | GPL-3.0-only, public |
| A `FM-1_` identity no other firmware uses | **Met from 1.0:** `FM-1_8133700`, free on the hub when chosen. fm1_open's updater (`WebUpdate.js`, `openIdentity`) takes any `FM-1_8` and six digits as Open-family, so the workbench and the installer go on treating it as Open, and the way back is unchanged. The hub's own rule is `FM-1_` and 3 to 8 digits; the short ones belong to firmwares with their own loader |
| An `fmhub.json` in the repository's root | in the root from 1.0 |

Once these are met, a listing would put CLATTER Pocket in the Hub and, from there, in the public
emulator's catalogue.

## Licences

| Thing | Terms | What that asked of the site |
|---|---|---|
| fm1-emu | MIT | its `LICENSE` is kept beside the vendored files |
| CLATTER Pocket | GPL-3.0-only with the LXR firmware's section 7 terms | a binary on the site needs its corresponding source available; modified versions are marked; no Erica Synths logo, no implied endorsement |
| fm1_open app | GPL-3.0-only with the FM1 SDK linking exception | include its `LICENSE.md` and `THIRD_PARTY.md` with distributions |
| M-VAVE's engine binary | Redistribution allowed "as part of firmware and firmware-inclusive workbenches for M-VAVE FM1 devices"; running is granted "on M-VAVE FM1 devices" | see below |
| Boot and update resources | Apache-2.0 | carried in `THIRD_PARTY.md` |
| Silkscreen, IBM Plex Sans | OFL 1.1 | licence files beside the fonts |

**An open question for the owner.** M-VAVE's permission to *run* the engine binary names FM-1 devices, and
an emulator is not one. Community practice is clear: fm1-emu runs packages containing it, and
M-VAVE's own workbench has a simulator, though that is a separate WebAssembly build. Hosting a package
for an emulator is not obviously what the grant covers, and M-VAVE has not been asked. The package is
redistributed in the form the grant does allow, as firmware for FM-1 devices, with the notices.
