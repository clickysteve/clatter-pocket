# Third-party notices

| Component | Where | Licence | Use |
|---|---|---|---|
| Erica Synths LXR-02 firmware, revision `a43cd4fc` | `lxr/upstream/` | GPL-3.0-only with section 7 terms (`lxr/upstream/LICENSE`, `NOTICE.md`) | the sound engine, unchanged |
| CLATTER (revision in `lxr/CLATTER_REVISION`) | `lxr/shim/`, `lxr/bridge/`, `lxr/tools/`; `engine/pocket_lxr.c` adapts `clatter_lxr.c`; `tools/gen-param-header.py` adapts `gen-param-ids.py` | GPL-3.0-only (CLATTER's NOTICE) | the port glue: HAL/CMSIS shim, parameter handler extraction, libm |
| CLATTER's rhythm generation | `seq/pocket_gen.c` ports `src/rhythm/generate.ts`, `mutate.ts`, `euclid.ts`, `rng.ts`; `seq/pocket_styles.h` is generated from `styles.ts` | GPL-3.0-only | pattern generation |
| DOWNBEAT | mutation, Euclidean rhythms, the keyed random and metric weights, through CLATTER | MIT | the same |
| M-VAVE fm1_open, commit `4c7496d` | `third_party/fm1_open` (git submodule, not copied); `firmware/fm1/fm1_open.patch` modifies two application files; `firmware/fm1/Build-ClatterPocket.ps1` is derived from `sdk/Build.ps1` | application: GPL-3.0-only WITH the FM1 SDK linking exception; binary SDK: M-VAVE terms (FM-1 devices only); boot/recovery resources: Apache-2.0; components per `THIRD_PARTY.md` | the FM-1 platform |
| JieLi toolchain 2.5.3 | not included; downloaded by `tools/setup-toolchain.sh` | JieLi's terms | compiler |

`lxr/MANIFEST.sha256` lists the SHA-256 of every file taken from CLATTER at import.
