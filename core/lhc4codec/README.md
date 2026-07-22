# LHC4 compression in ROOT

ROOT can use [lhc4codec](https://gitlab.cern.ch/eos/lhc4codec.git) as an optional
compression backend for RNTuple, `TFile`, and any other component that goes through
the shared `RZip` layer in `core/zip`.

LHC4 is exposed as compression algorithm **`kLHC4`** (numeric id **6**). A typical
setting is **`606`**: algorithm 6, level 6.

This directory contains the ROOT wrapper (`ZipLHC4.cxx`) that plugs lhc4codec into
`R__zipMultipleAlgorithm()` / `R__unzip()`. RNTuple itself needs no separate codec
code; it calls `RNTupleCompressor::Zip()` which delegates to the shared machinery.

## Building ROOT with lhc4codec

LHC4 support is **off by default**. Enable it at configure time with `-Dlhc4codec=ON`.

The upstream library lives on CERN GitLab and requires credentials to clone:

```text
https://gitlab.cern.ch/eos/lhc4codec.git
```

### Option A: built-in copy (automatic git clone)

If you have network access and GitLab credentials for CERN, ROOT can clone and build
lhc4codec automatically (same pattern as clad):

```sh
cmake -S /path/to/root -B /path/to/root-build \
  -Dlhc4codec=ON \
  -Dbuiltin_lhc4codec=ON

cmake --build /path/to/root-build -j
```

The repository and commit are pinned in `builtins/lhc4codec/CMakeLists.txt`
(`ROOT_LHC4CODEC_GIT_TAG`). GitLab authentication must work non-interactively
(SSH key, credential helper, or CI token).

### Option B: built-in copy (local checkout)

Use a local tree instead of cloning (offline development or a custom branch):

```sh
git clone https://gitlab.cern.ch/eos/lhc4codec.git   # requires CERN access

cmake -S /path/to/root -B /path/to/root-build \
  -Dlhc4codec=ON \
  -Dbuiltin_lhc4codec=ON \
  -DLHC4CODEC_SOURCE_DIR=/path/to/lhc4codec

cmake --build /path/to/root-build -j
```

`builtin_lhc4codec=ON` builds only the library (CLI, tests, Metal/HIP backends, and
LTO are disabled inside ROOT). When `LHC4CODEC_SOURCE_DIR` is set it takes precedence
over the automatic git clone.

### Option C: system-installed library

Install lhc4codec on the system first, then configure ROOT against it:

```sh
cmake -S /path/to/lhc4codec -B /path/to/lhc4codec-build -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/lhc4codec-build -j
cmake --install /path/to/lhc4codec-build --prefix /opt/lhc4codec

cmake -S /path/to/root -B /path/to/root-build \
  -Dlhc4codec=ON \
  -Dbuiltin_lhc4codec=OFF \
  -DLHC4CODEC_ROOT=/opt/lhc4codec

cmake --build /path/to/root-build -j
```

CMake discovers the library through `cmake/modules/FindLHC4CODEC.cmake`.

When enabled, ROOT defines `R__HAS_LHC4CODEC` in `RConfigure.h`.

## CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `lhc4codec` | `OFF` | Enable LHC4 compression support in Core |
| `builtin_lhc4codec` | `OFF` | Build lhc4codec inside ROOT (git clone or local tree) |
| `LHC4CODEC_SOURCE_DIR` | empty | Optional local checkout; skips git clone when set |

## Using LHC4 in RNTuple

```cpp
#include "ROOT/RNTupleWriteOptions.hxx"
#include "Compression.h"

ROOT::Experimental::RNTupleWriteOptions options;

// shorthand: 606 = kLHC4, level 6
options.SetCompression(606);

// or explicitly
options.SetCompression(ROOT::RCompressionSetting::EAlgorithm::kLHC4,
                       ROOT::RCompressionSetting::ELevel::kDefaultLHC4);
```

Level selects the lhc4codec speed/ratio trade-off (1–9 via the ROOT compression
level; levels 10–11 are available inside lhc4codec but not mapped through the
standard ROOT level field today).

## Optional behaviour: backends, filters, and BWT mode

Beyond algorithm + level, process-wide toggles control lhc4codec behaviour.
They follow the same global pattern as `R__SetZipMode()` for the default codec.

### Compression backends

| `R__SetLHC4Codec()` | Effect |
|---------------------|--------|
| `kLHC4CodecLz` (0, default) | Native LHC4 LZ frames |
| `kLHC4CodecBwt` (1) | Native LHC4 BWT block mode |
| `kLHC4CodecZstd` (2) | Native zstd frames (if linked) |
| `kLHC4CodecBzip3` (3) | Native bzip3 frames (if linked) |
| `kLHC4CodecLzma` (4) | Native xz/LZMA frames (if linked) |
| `kLHC4CodecAuto` (5) | Race available codecs at the chosen level |

Query availability with `R__LHC4CodecAvailable(codec)`. `Auto` is always available;
optional backends (Zstd, Bzip3, Lzma) must be linked for them to participate in the
race. `R__SetLHC4Bwt(1/0)` remains as a convenience alias for switching between Lz and Bwt.

### Byte filters (Lz/Bwt only)

When using lhc4codec filters, disable RNTuple's own column encodings so the compressor
can apply byte transforms on plain page data. With LHC4 compression, disabling column
encoding also enables lhc4codec filters automatically when the page sink is created:

```cpp
#include "ROOT/RNTupleWriteOptions.hxx"

ROOT::Experimental::RNTupleWriteOptions options;
options.SetEnableColumnEncoding(false);
options.SetCompression(ROOT::RCompressionSetting::EAlgorithm::kLHC4, 6);
// R__SetLHC4Filters(1) is applied automatically for LHC4

// ... create writer with options ...
```

Explicit control before writing:

```cpp
#include "ZipLHC4.h"

R__SetLHC4Codec(kLHC4CodecLz);
R__SetLHC4Filters(1);           // shuffle/delta/zigzag/dict auto-detect
R__SetLHC4FilterFallback(1);    // also try raw input, keep smaller (default)
R__SetLHC4FilterRle(1);         // try post-filter byte-RLE (default)
R__SetLHC4FilterDict(1);        // allow dict32/64 filters (default)

// External backends (when `R__LHC4CodecAvailable()` returns 1) also support byte filters
via the L4EF wrapper frame:
R__SetLHC4Codec(kLHC4CodecZstd);
R__SetLHC4Filters(0);           // filters are ignored for zstd/bzip3
```

| API | Default | Effect |
|-----|---------|--------|
| `R__SetLHC4Codec(int)` | Lz | Select Lz / Bwt / Zstd / Bzip3 / Lzma / Auto backend |
| `R__SetLHC4AutoMinGainPct(int)` | 1 | Auto only: min % gain to prefer a slower decoder |
| `R__SetLHC4Filters(int)` | off | Auto-detect + apply byte transforms |
| `R__SetLHC4FilterFallback(int)` | on | With filters: also compress raw and keep smaller |
| `R__SetLHC4FilterRle(int)` | on | Try byte-RLE on filter/intermediate path |
| `R__SetLHC4FilterDict(int)` | on | Allow dictionary-remap filters |
| `R__SetLHC4Bwt(int)` | off | Legacy alias for Bwt/Lz codec selection |

When the codec is **Zstd** or **Bzip3**, enabled byte filters are stored in an **L4EF**
wrapper frame around the native backend payload (transparent to `R__unzipLHC4()`).

Getters: `R__GetLHC4*()` for each setter above, plus `R__LHC4CodecAvailable()`.

**When to enable what**

- **Filters**: physics branches with integers/floats stored column-wise in pages.
- **Filter RLE**: repetitive filtered bytes (long zero runs in shuffled columns).
- **BWT / Bwt codec**: repetitive or text-like payloads where the LZ path is less effective.
- **Zstd / Bzip3 / Lzma**: when you want native frames from those tools inside ROOT's LC wrapper.
- **Auto**: benchmark-style codec selection; races linked backends and picks a decode-speed-aware
  winner (`R__SetLHC4AutoMinGainPct`, default 1%).

Decompression does **not** need these flags; the inner frame type and filter path are
restored automatically on read.

Set the toggles **before** writing compressed data. They apply to all subsequent LHC4
compressions in the process until changed.

## On-disk format

ROOT wraps each compressed block in the usual 9-byte header used by ZLIB/ZSTD/LZ4:

```text
'L' 'C' <version> | 3-byte compressed size | 3-byte uncompressed size | LHC4 frame
```

The inner payload is a standard lhc4codec frame (magic `LHC4`).

## Tests

With `-Dtesting=ON`, the following cover LHC4 when `lhc4codec` is enabled:

- `core/zip/test/ZipTest.cxx` — buffer-size sweep for `kLHC4`
- `tree/ntuple/test/ntuple_zip.cxx` — round-trip, filters, and BWT modes

## Layout

```text
core/lhc4codec/          ROOT wrapper (this directory)
  inc/ZipLHC4.h          Public C API: zip/unzip + codec/filter toggles
  src/ZipLHC4.cxx        Calls lhc4codec C++ CompressParams API

builtins/lhc4codec/      Git clone or add_subdirectory of upstream source
cmake/modules/FindLHC4CODEC.cmake
```

For details on lhc4codec itself (levels, block sizes, CLI benchmarks), see the
upstream README in the lhc4codec repository.
