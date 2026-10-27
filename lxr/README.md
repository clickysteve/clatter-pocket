# lxr/

The LXR-02 synthesis engine as CLATTER ports it, taken from CLATTER at the revision in
`CLATTER_REVISION` (firmware revision in `upstream/UPSTREAM_REVISION`). `MANIFEST.sha256`
records every file's SHA-256 at import; none has been changed here.

- `upstream/` - Erica Synths' firmware files, byte-identical to the firmware repository
- `shim/` - CLATTER's replacements for the STM32 HAL/CMSIS and C library headers
- `bridge/lxr_params.c` - the firmware's parameter handler, extracted by CLATTER
- `bridge/libm.c` - CLATTER's freestanding maths functions
- `tools/` - CLATTER's import and extraction scripts

CLATTER's WebAssembly interface (`clatter_lxr.c`) is not used; `engine/pocket_lxr.c` is its
real-time counterpart. To re-import, copy the same paths from a newer CLATTER checkout,
regenerate `MANIFEST.sha256` and `engine/pocket_params.h`, and run `make test compare`.
