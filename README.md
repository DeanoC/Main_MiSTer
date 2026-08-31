# DeanoC Main_MiSTer fork

This is DeanoC's `Main_MiSTer` fork used to understand and support FogCast's
working Main-compatible target path.

This baseline is upstream Main plus the deterministic `VDATE` build adjustment
at commit `d1a3a4e`. It is not the experimental FogCast native coordinator and
does not contain that runtime.

## How FogCast uses Main

On the MiSTer Pi, the resident binary normally runs as:

```text
/media/fat/MiSTer /media/fat/menu.rbf
```

FogCast's target agent uses the existing Main command contract:

- `/dev/MiSTer_cmd` is Main's command FIFO.
- `load_core PATH` asks Main to load an RBF or MGL path.
- `/tmp/CORENAME` reports the active core name.
- Loading `menu.rbf` returns the target to the menu.

The relevant source is in `input.cpp` for the command FIFO and `load_core`
parser, `fpga_io.cpp` for RBF loading, and `user_io.cpp` for core-name state.

FogCast already uses this path for its multi-system game library on real
hardware. The immediate extension is to let FogCast select and transfer an
arbitrary development RBF, then issue the same `load_core` command.

## Branch boundary

Branches named for `stage-c0-native-coordinator` or later Task 9 native work
are separate experiments toward a different runtime structure. They are not a
dependency of the working FogCast game path and must not be described as the
deployed runtime without checking the actual target artifact.

This recovery branch intentionally makes no native-runtime or production
deployment claim.

## Build

This fork requires an explicit six-digit `VDATE` so repeated builds do not
silently embed the wall-clock date:

```sh
make VDATE=260831
```

For the upstream ARM cross-compilation prerequisites, see the MiSTer Main
developer documentation:

https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/#general-prerequisites-for-arm-cross-compiling
