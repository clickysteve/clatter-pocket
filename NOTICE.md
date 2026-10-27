# CLATTER Pocket: copyright, licence and trademark notice

Copyright (C) 2026 The CLATTER contributors.

CLATTER Pocket is free software: you can redistribute it and/or modify it under the terms of
the **GNU General Public License version 3 only** (GPL-3.0-only) as published by the Free
Software Foundation, with the additional terms below. The licence text is in
[LICENSE](LICENSE). It is distributed WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

## The LXR firmware

CLATTER Pocket contains and is a modified version of parts of the Erica Synths LXR-02 firmware
(<https://github.com/erica-synths/lxr-desktop-app>, revision
`a43cd4fcda23ec3d577dc425261c3eb28bd5abff`), Copyright (C) 2012-2026 Erica Synths, licensed
under GPL-3.0-only with additional terms under section 7, taken through CLATTER
(<https://github.com/clickysteve/clatter>, revision in `lxr/CLATTER_REVISION`). The firmware
files are in `lxr/upstream/`, unchanged, with the firmware's own `LICENSE` and `NOTICE.md`.
The firmware's additional terms apply to CLATTER Pocket as a whole:

1. **Trademarks (GPL-3.0 section 7(e)).** This licence grants no rights in the names "Erica
   Synths", "LXR" and "LXR-02" or the associated logos. CLATTER Pocket uses them only to state
   truthfully where its synthesis comes from, uses no Erica Synths logo, and is **not** made,
   approved, endorsed or supported by Erica Synths or affiliated with it.
2. **Marking of modified versions (GPL-3.0 section 7(c)).** CLATTER Pocket is a modified
   version of the firmware's sound engine and says so in its documentation. Modified versions
   must be marked as modified and must not be presented as official Erica Synths software.

The LXR hardware design is not licensed under the GPL and is not part of CLATTER Pocket.

## The FM-1 SDK

FM-1 firmware builds combine CLATTER Pocket with M-VAVE's fm1_open application (GPL-3.0-only
with a linking exception for the FM1 binary SDK) and its binary SDK (separate terms, FM-1
devices only). CLATTER Pocket's own files are GPL-3.0-only; the FM-1 adapter and patch are
written to be distributable under fm1_open's terms. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `third_party/fm1_open/LICENSE.md`.
"M-VAVE" and "FM-1" are used only to name the target hardware; CLATTER Pocket is not an
M-VAVE product.

## Source code

The complete corresponding source, including the firmware files, the shims, the build scripts
and the fm1_open revision used, is this repository.
