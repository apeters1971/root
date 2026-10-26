# Spectra8 compression in ROOT

ROOT can use [spectra8](https://github.com/apeters1971/spectra8) as an optional
compression backend for RNTuple, `TFile`, and any other component that goes through
the shared `RZip` layer in `core/zip`.

Spectra8 is exposed as compression algorithm **`kSpectra8`** (numeric id **6**). The
default setting is **`605`**: Crystal at level 5, with byte filters on. Decompression
sets `DecompressParams::gpu`, so Crystal lattice and profile blocks use Metal, CUDA,
or HIP when that backend and a device are present, and stay on the CPU otherwise.

This directory contains the ROOT wrapper (`ZipSpectra8.cxx`) that plugs spectra8 into
`R__zipMultipleAlgorithm()` / `R__unzip()`. RNTuple itself needs no separate codec
code; it calls `RNTupleCompressor::Zip()` which delegates to the shared machinery.

## Building ROOT with spectra8

Spectra8 support is **off by default**. Enable it at configure time with `-Dspectra8=ON`.

The upstream library is the GitHub `main` branch:

```text
https://github.com/apeters1971/spectra8.git
```

### Option A: built-in copy (automatic git clone)

If you have network access, ROOT can clone and build spectra8 automatically
(same pattern as clad):

```sh
cmake -S /path/to/root -B /path/to/root-build \
  -Dspectra8=ON \
  -Dbuiltin_spectra8=ON

cmake --build /path/to/root-build -j
```

The repository and commit are pinned in `builtins/spectra8/CMakeLists.txt`
(`ROOT_SPECTRA8_GIT_TAG`), currently GitHub `main`.

### Option B: built-in copy (local checkout)

Use a local tree instead of cloning (offline development or a custom branch):

```sh
git clone https://github.com/apeters1971/spectra8.git

cmake -S /path/to/root -B /path/to/root-build \
  -Dspectra8=ON \
  -Dbuiltin_spectra8=ON \
  -DSPECTRA8_SOURCE_DIR=/path/to/spectra8

cmake --build /path/to/root-build -j
```

`builtin_spectra8=ON` builds only the library (CLI, tests, Metal/HIP backends, and
LTO are disabled inside ROOT). When `SPECTRA8_SOURCE_DIR` is set it takes precedence
over the automatic git clone.

### Option C: system-installed library (RPM or Homebrew)

Prebuilt packages are published to
[spectra8-bin](https://gitlab.cern.ch/apeters/spectra8-bin):

```sh
# AlmaLinux 9 / 10 (x86_64)
sudo curl -fsSL -o /etc/yum.repos.d/spectra8.repo \
  https://gitlab.cern.ch/apeters/spectra8-bin/-/raw/master/spectra8-el9.repo   # or -el10
sudo dnf install spectra8 spectra8-devel

# macOS, Apple silicon (macOS 13+)
brew tap apeters/spectra8 https://gitlab.cern.ch/apeters/spectra8-bin.git
brew install apeters/spectra8/spectra8
```

Then configure ROOT (Homebrew prefixes `/opt/homebrew` and `/usr/local` are
searched automatically):

```sh
cmake -S /path/to/root -B /path/to/root-build -Dspectra8=ON
cmake --build /path/to/root-build -j
```

If the library is in a custom prefix:

```sh
cmake -S /path/to/root -B /path/to/root-build \
  -Dspectra8=ON \
  -DSPECTRA8_ROOT=/opt/spectra8
```

If `-Dspectra8=ON` is set and the library is missing, CMake prints these
install steps and stops. `-Dbuiltin_spectra8=ON` is the alternative that
clones `https://github.com/apeters1971/spectra8.git` instead.

CMake discovers the library through `cmake/modules/FindSPECTRA8.cmake`.

When enabled, ROOT defines `R__HAS_SPECTRA8` in `RConfigure.h`.

## CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `spectra8` | `OFF` | Enable Spectra8 compression support in Core |
| `builtin_spectra8` | `OFF` | Build spectra8 inside ROOT (git clone or local tree) |
| `SPECTRA8_SOURCE_DIR` | empty | Optional local checkout; skips git clone when set |

## Using Spectra8 in RNTuple

```cpp
#include "ROOT/RNTupleWriteOptions.hxx"
#include "Compression.h"

ROOT::Experimental::RNTupleWriteOptions options;

// shorthand: 605 = kSpectra8, Crystal level 5
options.SetCompression(605);

// or explicitly
options.SetCompression(ROOT::RCompressionSetting::EAlgorithm::kSpectra8,
                       ROOT::RCompressionSetting::ELevel::kDefaultSpectra8);
```

Level selects the spectra8 speed/ratio trade-off (1–9 via the ROOT compression
level; levels 10–11 are available inside spectra8 but not mapped through the
standard ROOT level field today).

## Optional behaviour: backends, filters, and BWT mode

Beyond algorithm + level, process-wide toggles control spectra8 behaviour.
They follow the same global pattern as `R__SetZipMode()` for the default codec.

### Compression backends

| `R__SetSpectra8Codec()` | Effect |
|---------------------|--------|
| `kSpectra8CodecBeam` (0) | Native Spectra8 Beam (LZ77) frames; `kSpectra8CodecLz` is a legacy alias |
| `kSpectra8CodecBwt` (1) | Native Spectra8 BWT block mode |
| `kSpectra8CodecZstd` (2) | Native zstd frames (if linked) |
| `kSpectra8CodecBzip3` (3) | Native bzip3 frames (if linked) |
| `kSpectra8CodecLzma` (4) | Native xz/LZMA frames (if linked) |
| `kSpectra8CodecAuto` (5) | Race available codecs at the chosen level |
| `kSpectra8CodecMosaic` (6) | Native Spectra8 Mosaic frames |
| `kSpectra8CodecOracle` (7) | Native Spectra8 Oracle frames (not raced by Auto) |
| `kSpectra8CodecCrystal` (8, default) | Native Spectra8 Crystal frames |
| `kSpectra8CodecFlash` (9) | Native Spectra8 Flash frames: byte-aligned LZ without an entropy stage (not raced by Auto) |
| `kSpectra8CodecLaser` (10) | Native Spectra8 Laser frames: LZMA2-class range coder with a decoupled decoder |
| `kSpectra8CodecGrating` (11) | Native Spectra8 Grating frames: order-0 integer rANS, no levels (not raced by Auto) |

Query availability with `R__Spectra8CodecAvailable(codec)`. Beam, Bwt, Mosaic, Oracle,
Crystal, Flash, Laser, Grating, and Auto are always available; optional backends (Zstd, Bzip3, Lzma) must
be linked for them to participate in the Auto race. `R__SetSpectra8Bwt(1/0)` remains as
a convenience alias for switching between Crystal and Bwt. Byte filters are on by
default (`R__SetSpectra8Filters(1)`).

### Byte filters (Beam/Bwt only)

When using spectra8 filters, disable RNTuple's own column encodings so the compressor
can apply byte transforms on plain page data. With Spectra8 compression, disabling column
encoding also enables spectra8 filters automatically when the page sink is created:

```cpp
#include "ROOT/RNTupleWriteOptions.hxx"

ROOT::Experimental::RNTupleWriteOptions options;
options.SetEnableColumnEncoding(false);
options.SetCompression(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
// Byte filters are already on. Disabling column encoding also turns them on
// if a caller had switched them off.

// ... create writer with options ...
```

Explicit control before writing:

```cpp
#include "ZipSpectra8.h"

R__SetSpectra8Codec(kSpectra8CodecBeam);
R__SetSpectra8Filters(1);           // shuffle/delta/zigzag/dict auto-detect
R__SetSpectra8FilterFallback(1);    // also try raw input, keep smaller (default)
R__SetSpectra8FilterRle(1);         // try post-filter byte-RLE (default)
R__SetSpectra8FilterDict(1);        // allow dict32/64 filters (default)

// External backends (when `R__Spectra8CodecAvailable()` returns 1) also support byte filters
via the L4EF wrapper frame:
R__SetSpectra8Codec(kSpectra8CodecZstd);
R__SetSpectra8Filters(0);           // filters are ignored for zstd/bzip3

// Auto races zstd/beam/crystal by default. Widen or restrict the set:
R__SetSpectra8Codec(kSpectra8CodecAuto);
R__SetSpectra8AutoCodecs(kSpectra8AutoCodecsRoot);   // default
R__SetSpectra8AutoCodecs(kSpectra8AutoCodecsAll);    // include mosaic/bwt/laser/lzma/bzip3
R__SetSpectra8AutoCodecs(kSpectra8CodecBit(kSpectra8CodecZstd) | kSpectra8CodecBit(kSpectra8CodecCrystal));
```

| API | Default | Effect |
|-----|---------|--------|
| `R__SetSpectra8Codec(int)` | Beam | Select Beam / Bwt / Zstd / Bzip3 / Lzma / Auto / Mosaic / Oracle / Crystal / Flash / Laser / Grating |
| `R__SetSpectra8AutoMinGainPct(int)` | 1 | Auto only: min % gain to prefer a slower decoder |
| `R__SetSpectra8AutoMaxLevel(int)` | off | Auto only: race each codec at max effort instead of level |
| `R__SetSpectra8AutoCodecs(unsigned)` | `kSpectra8AutoCodecsRoot` | Auto only: bitmask of codecs to race (`kSpectra8CodecBit`) |
| `R__SetSpectra8Filters(int)` | off | Auto-detect + apply byte transforms |
| `R__SetSpectra8FilterFallback(int)` | on | With filters: also compress raw and keep smaller |
| `R__SetSpectra8FilterRle(int)` | on | Try byte-RLE on filter/intermediate path |
| `R__SetSpectra8FilterDict(int)` | on | Allow dictionary-remap filters |
| `R__SetSpectra8Bwt(int)` | off | Legacy alias for Bwt/Beam codec selection |

When the codec is **Zstd** or **Bzip3**, enabled byte filters are stored in an **L4EF**
wrapper frame around the native backend payload (transparent to `R__unzipSpectra8()`).

Getters: `R__GetSpectra8*()` for each setter above, plus `R__Spectra8CodecAvailable()`.

**When to enable what**

- **Filters**: physics branches with integers/floats stored column-wise in pages.
- **Filter RLE**: repetitive filtered bytes (long zero runs in shuffled columns).
- **BWT / Bwt codec**: repetitive or text-like payloads where the Beam path is less effective.
- **Mosaic / Crystal**: additional native Spectra8 backends; Crystal targets best ratio with fast decode.
- **Oracle**: learned-context demo codec; available for forced selection, not raced by Auto.
- **Flash**: byte-aligned LZ without an entropy stage, the fastest decoder; for ingestion
  tiers where decode speed matters more than ratio. Forced selection only.
- **Laser**: LZMA2-class range coder with a decoupled, branchless decoder; LZMA ratio at
  higher decode speed. Part of `kSpectra8AutoCodecsAll`.
- **Grating**: order-0 integer rANS over 1/2/4/8-byte symbols with an escape for the tail;
  has no levels. Suited to peaked integer columns (counts, indices). Forced selection only.
- **Zstd / Bzip3 / Lzma**: when you want native frames from those tools inside ROOT's LC wrapper.
- **Auto**: benchmark-style codec selection. By default it races the ROOT set
  (`kSpectra8AutoCodecsRoot`: Zstd, Beam, Crystal) and picks a decode-speed-aware winner
  (`R__SetSpectra8AutoMinGainPct`, default 1%). Use `R__SetSpectra8AutoCodecs(kSpectra8AutoCodecsAll)`
  or a custom `kSpectra8CodecBit(...)` mask to change the candidates. Passing `0` resets to
  the ROOT default. `R__SetSpectra8AutoMaxLevel(1)` races each candidate at its maximum
  effort instead of the shared compression level. `tree2ntuple` / `rntuple2rntuple`
  expose the same mask as `--auto-codecs root|all|zstd,beam,...`.
  `--level N` (1–11, default 5) sets the RNTuple compression level for Spectra8
  variants; `spectra8_laser9` is equivalent to `--level 9 --only-variant spectra8_laser5`.
  Grating ignores the level. `rntupleinspect` samples packed pages and reports
  whether the remaining payload is mantissa-limited SplitReal32 (no Spectra8
  headroom) or peaked integers (Grating / Crystal). `rntuple2rntuple` unpacks
  split/delta/zigzag encodings before Spectra8 re-encode (matching `tree2ntuple`);
  `--keep-column-encoding` recompresses packed pages instead. `rntupletryzip`
  unpacks split/delta/zigzag encodings the same way, then recompresses in
  memory (no dictionaries, no output file), so CMSSW MiniAOD streamer fields
  do not block a size trial. `--keep-column-encoding` restores packed-page
  trials.
  `--brute-filters` races matching lossless filters per page on every hardware
  core (`--threads N` to cap). Without `--brute-filters`, `--threads` races
  codecs and pages in one pool (TLS codec override, shared inflated buffers).

### Crystal static profiles

Standard Crystal is the dynamic coder with byte filters, the same kind of compressor
as Beam or zstd. `R__SetSpectra8ProfilePerColumn(1)` turns on pretrained per-column models
while `R__GetSpectra8Profile()` stays `kSpectra8ProfileNone`. The choice is stored on the zip
call for that page, so IMT workers compressing different columns do not share one profile.
Decompression reads the profile id from the page.

| Column type | Profile |
|-------------|---------|
| `Real32` | `kSpectra8ProfileFloatKinematics` |
| `Index32`, `UInt32` | `kSpectra8ProfilePoissonCounts` |
| `Char` | `kSpectra8ProfileText` |
| anything else (`Real64`, `Index64`, `Int32`, split columns, …) | dynamic per-page model |

`R__SetSpectra8Profile()` forces one profile onto every column. `R__SetSpectra8ProfileId(id)`
overrides the on-wire id of that forced profile (`0` uses the enum value). Custom ids
must be >= 5 and come from `R__Spectra8TrainAndRegisterProfile()`.

`spectra8_crystal5` is standard Crystal. `spectra8_crystal_profile5` enables the per-column
choice. `--profile float|poisson|monotonic|text|custom` forces that one model on
`spectra8_crystal_profile5` only.

### Learned column compression profiles (`spectra8_crystal_learned`)

Rather than relying on hard-coded heuristics or paying the overhead of repeating dynamic
frequency tables on every page (~1.5 KiB/page), RNTuple supports data-learned column
compression profiles:
- **Empirical Training**: Trained from initial column data (Cluster 0) using
  `R__Spectra8TrainProfileModelLevel(sample, n, stride, level, ...)` with configurable
  levels (5, 7, 9) and order-1/2 context predictors (`C1`, `C2`, `Lat1`, `Trend`, `RecEq`).
- **Metadata Persistence**: The trained model (130–550 bytes) is stored in the RNTuple
  Header via `RCompressionContext` (Feature Flag `kFeatureFlag_CompressionContext`,
  record flag `0x04`), eliminating per-page table headers.
- **Precomputed Decoder Tables**: Decode tables (`dec_tab`, `q_scaled`, `DecLane`) are
  precomputed once during profile registration and cached in `StaticProfileHolder`.
  This delivers line-rate SIMD rANS decompression without per-page reconstruction overhead.

Decompression does **not** need these flags; the inner frame type and filter path are
restored automatically on read.

Set the toggles **before** writing compressed data. They apply to all subsequent Spectra8
compressions in the process until changed.

### Debugging corrupt frames

Failure dumps are compiled out by default. Rebuild `ROOTSpectra8Zip` with
`-DR__Spectra8_FAILURE_DUMP` (see `core/spectra8/CMakeLists.txt`) to write
artifacts to `/tmp/root-spectra8-failure-<pid>/` when compression or decompression
fails:

| File | Contents |
|------|----------|
| `<id>.meta` | stage, status, codec, filter settings, page sizes |
| `<id>.uncompressed.bin` | raw page bytes (when available, e.g. `--verify` or zip errors) |
| `<id>.compressed_lc.bin` | full ROOT `LC` frame (9-byte header + spectra8 payload) |

This is triggered automatically from `R__zipSpectra8`, `R__unzipSpectra8`, and RNTuple
`--verify` round-trip failures. Reproduce with:

```sh
tree2ntuple --verify --only-variant spectra8_bzip3 ...
```

Then inspect the dump and replay with the cerncodec CLI or a small standalone
decompress program.

## On-disk format

ROOT wraps each compressed block in the usual 9-byte header used by ZLIB/ZSTD/LZ4:

```text
'L' 'C' <version> | 3-byte compressed size | 3-byte uncompressed size | Spectra8 frame
```

The inner payload is a standard spectra8 frame (magic `LHC4`).

## Tests

With `-Dtesting=ON`, the following cover Spectra8 when `spectra8` is enabled:

- `core/zip/test/ZipTest.cxx` — buffer-size sweep for `kSpectra8`
- `tree/ntuple/test/ntuple_zip.cxx` — round-trip, filters, BWT, Mosaic/Crystal/Oracle, Auto, and Auto codec mask

## Layout

```text
core/spectra8/          ROOT wrapper (this directory)
  inc/ZipSpectra8.h          Public C API: zip/unzip + codec/filter toggles
  src/ZipSpectra8.cxx        Calls spectra8 C++ CompressParams API

builtins/spectra8/      Git clone or add_subdirectory of upstream source
cmake/modules/FindSPECTRA8.cmake
```

For details on spectra8 itself (levels, block sizes, CLI benchmarks), see the
upstream README in the spectra8 repository.
