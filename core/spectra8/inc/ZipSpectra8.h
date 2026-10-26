// Original Author: ROOT Team
/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_ZipSpectra8
#define ROOT_ZipSpectra8

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
   kSpectra8CodecBeam = 0,     ///< Native Spectra8 Beam (LZ77) frames
   kSpectra8CodecLz = 0,       ///< Legacy name for Beam
   kSpectra8CodecBwt = 1,
   kSpectra8CodecZstd = 2,
   kSpectra8CodecBzip3 = 3,
   kSpectra8CodecLzma = 4,
   kSpectra8CodecAuto = 5,
   kSpectra8CodecMosaic = 6,
   kSpectra8CodecOracle = 7,
   kSpectra8CodecCrystal = 8,
   kSpectra8CodecFlash = 9,    ///< Byte-aligned LZ without an entropy stage (ingestion tier)
   kSpectra8CodecLaser = 10,   ///< LZMA2-class binary compressor with a decoupled range decoder
   kSpectra8CodecGrating = 11, ///< Order-0 integer rANS over 1/2/4/8-byte symbols; has no levels
   kSpectra8CodecCount = 12,
};

/// Bit in an Auto codec mask: `1u << kSpectra8Codec*`. Auto, Oracle, Flash and Grating are not raceable.
#define kSpectra8CodecBit(c) (1u << (unsigned)(c))
/// Default Auto race for ROOT / RNTuple: zstd | beam | crystal (fast decoders).
#define kSpectra8AutoCodecsRoot \
   (kSpectra8CodecBit(kSpectra8CodecZstd) | kSpectra8CodecBit(kSpectra8CodecBeam) | kSpectra8CodecBit(kSpectra8CodecCrystal))
/// Every codec Auto can race (Oracle, Flash and Grating are never included).
#define kSpectra8AutoCodecsAll                                                                 \
   (kSpectra8AutoCodecsRoot | kSpectra8CodecBit(kSpectra8CodecMosaic) | kSpectra8CodecBit(kSpectra8CodecBwt) | \
    kSpectra8CodecBit(kSpectra8CodecLaser) | kSpectra8CodecBit(kSpectra8CodecLzma) | kSpectra8CodecBit(kSpectra8CodecBzip3))

/// Pretrained Crystal static profiles (spectra8::Profile). None keeps dynamic per-page models.
enum {
   kSpectra8ProfileNone = 0,
   kSpectra8ProfileFloatKinematics = 1, ///< IEEE-754 float columns (pT, eta, phi, mass, energy)
   kSpectra8ProfilePoissonCounts = 2,   ///< multiplicities, track counts, jagged offsets
   kSpectra8ProfileIntMonotonic = 3,    ///< event IDs, timestamps, entry counters
   kSpectra8ProfileText = 4,            ///< text, JSON, XML
   kSpectra8ProfileCustom = 5,          ///< id registered with R__Spectra8TrainAndRegisterProfile
};

void R__zipSpectra8(int cxlevel, int *srcsize, const char *src, int *tgtsize, char *tgt, int *irep);
void R__unzipSpectra8(int *srcsize, const unsigned char *src, int *tgtsize, unsigned char *tgt, int *irep);

/// Select the spectra8 backend (Beam/Lz, Bwt, Zstd, Bzip3, Lzma, Auto, Mosaic,
/// Oracle, Crystal, Flash, Laser, Grating). Defaults to Crystal.
void R__SetSpectra8Codec(int codec);
int R__GetSpectra8Codec(void);
/// Returns 1 when the requested backend was linked into spectra8, else 0.
/// Auto is always available; it races backends that are both linked and present
/// in `R__GetSpectra8AutoCodecs()`.
int R__Spectra8CodecAvailable(int codec);

/// Auto only: min % size gain to prefer a slower decoder (default 1). 0 = smallest wins.
void R__SetSpectra8AutoMinGainPct(int pct);
int R__GetSpectra8AutoMinGainPct(void);

/// Auto only: race each codec at max effort instead of the compression level (default off).
void R__SetSpectra8AutoMaxLevel(int enable);
int R__GetSpectra8AutoMaxLevel(void);

/// Auto only: bitmask of codecs to race (`kSpectra8CodecBit`). Default is
/// `kSpectra8AutoCodecsRoot` (zstd | beam | crystal). 0 resets to that default.
/// `kSpectra8AutoCodecsAll` races every available flavour except Oracle, Flash and Grating.
void R__SetSpectra8AutoCodecs(unsigned mask);
unsigned R__GetSpectra8AutoCodecs(void);

/// Enable spectra8 byte filters (shuffle/delta/zigzag/dict) for columnar data.
/// On by default.
void R__SetSpectra8Filters(int enable);
int R__GetSpectra8Filters(void);

/// With filters: also compress raw input and keep the smaller result (default on).
void R__SetSpectra8FilterFallback(int enable);
int R__GetSpectra8FilterFallback(void);

/// Try post-filter byte-RLE when it shrinks the intermediate (default on).
void R__SetSpectra8FilterRle(int enable);
int R__GetSpectra8FilterRle(void);

/// Allow dictionary-remap filters (dict32/64) in auto-detect (default on).
void R__SetSpectra8FilterDict(int enable);
int R__GetSpectra8FilterDict(void);

/// Bitmask of FilterId values detection may nominate (`1ull << id`).
/// Default is all (`~0ull`). 0 resets to all. A singleton bit forces that
/// filter (no entropy reject); use with `R__SetSpectra8FilterFallback(0)` to
/// measure a transform even when it loses.
void R__SetSpectra8FilterMask(unsigned long long mask);
unsigned long long R__GetSpectra8FilterMask(void);

/// Crystal static profile (`kSpectra8Profile*`) forced on every column.
/// `kSpectra8ProfileNone` (default) keeps the dynamic Crystal coder and byte filters.
/// A non-none profile skips per-page model headers.
void R__SetSpectra8Profile(int profile);
int R__GetSpectra8Profile(void);

/// When enabled, and no profile is forced, RNTuple selects a pretrained profile from
/// the column type. Default is off, so Crystal behaves like the other codecs.
void R__SetSpectra8ProfilePerColumn(int enable);
int R__GetSpectra8ProfilePerColumn(void);

/// Profile for `R__zipSpectra8` calls on this thread. `-1` (default) uses `R__GetSpectra8Profile()`.
/// RNTuple sets this around each column page so workers do not share one profile.
void R__SetSpectra8CallProfile(int profile);
int R__GetSpectra8CallProfile(void);

/// Per-thread filter trial for `R__zipSpectra8`. `filters`/`fallback` of -1 and
/// `mask` of 0 follow the process-wide settings. rntupletryzip uses this so
/// workers can race different filters without clobbering each other.
void R__SetSpectra8CallFilterTrial(int filters, unsigned long long mask, int fallback);

/// Codec for `R__zipSpectra8` calls on this thread. `-1` (default) uses `R__GetSpectra8Codec()`.
/// rntupletryzip sets this so workers can race Crystal/Laser/… without clobbering
/// the process-wide codec.
void R__SetSpectra8CallCodec(int codec);
int R__GetSpectra8CallCodec(void);

/// Call profile id for `R__zipSpectra8` calls on this thread. 0 uses the call profile enum value.
void R__SetSpectra8CallProfileId(unsigned id);
unsigned R__GetSpectra8CallProfileId(void);

/// Profile id when `R__GetSpectra8Profile()` is not none. 0 uses the profile enum value.
/// Custom trained profiles use an id >= 5.
void R__SetSpectra8ProfileId(unsigned id);
unsigned R__GetSpectra8ProfileId(void);

/// Train a custom static profile from columnar sample bytes and register it.
/// `profileId` must be >= 5. `stride` is the element width in bytes. Returns 1 on success.
int R__Spectra8TrainAndRegisterProfile(unsigned profileId, const void *sample, size_t sampleSize, int stride);
int R__Spectra8TrainAndRegisterProfileLevel(unsigned profileId, const void *sample, size_t sampleSize, int stride, int level);

/// Train a static Crystal profile model from sample columnar data.
/// If outModel is NULL, writes required size to *outModelSize and returns 1.
/// Returns 1 on success, 0 on failure.
int R__Spectra8TrainProfileModel(const void *sample, size_t sampleSize, int stride,
                             void *outModel, size_t *outModelSize);
int R__Spectra8TrainProfileModelLevel(const void *sample, size_t sampleSize, int stride, int level,
                                  void *outModel, size_t *outModelSize);

/// Register a serialized profile model under profileId (>= 5).
/// Returns 1 on success, 0 on failure.
int R__Spectra8RegisterProfileModel(unsigned profileId, const void *modelBytes, size_t modelSize);

/// Export an already-registered or builtin profile model to serialized bytes.
/// If outModel is NULL, writes required size to *outModelSize and returns 1.
/// Returns 1 on success, 0 on failure.
int R__Spectra8ExportProfileModel(unsigned profileId, void *outModel, size_t *outModelSize);

/// Legacy convenience: enable BWT block mode. Disabling it returns to Crystal.
void R__SetSpectra8Bwt(int enable);
int R__GetSpectra8Bwt(void);

/// Reset aggregated spectra8 compression statistics.
void R__ResetSpectra8CompressStats(void);
/// Print aggregated spectra8 compression statistics to stdout.
void R__PrintSpectra8CompressStats(void);

/// Page-outcome counters from the most recent compress-stats window.
struct R__Spectra8CompressStatsSummary {
   unsigned long long num_pages;
   unsigned long long uncompressed_bytes;
   unsigned long long compressed_bytes;
   unsigned long long filtered_won_pages;
   unsigned long long raw_won_pages;
   unsigned long long no_transform_pages;
   unsigned long long stored_fallback_pages;
   unsigned long long plain_pages;
   /// Crystal blocks requested with a static profile: coded with it / escaped to a dynamic block
   unsigned long long crystal_profile_blocks;
   unsigned long long crystal_profile_escapes;
};
void R__GetSpectra8CompressStatsSummary(struct R__Spectra8CompressStatsSummary *out);

/// Auto-mode winner histogram (indexed by kSpectra8Codec*; only populated for spectra8_auto runs).
/// Slots for Auto and Oracle are unused: Auto is the racer, Oracle is not raced.
struct R__Spectra8AutoCodecStatsSummary {
   unsigned long long auto_selections;
   unsigned long long codec_hits[kSpectra8CodecCount];
};
void R__GetSpectra8AutoCodecStatsSummary(struct R__Spectra8AutoCodecStatsSummary *out);

/// Optional inputs for R__DumpSpectra8Failure (null pointers / zero sizes are omitted from dump files).
struct R__Spectra8FailureDumpInfo {
   const char *fStage;            ///< e.g. "zip", "unzip", "verify_unzip", "verify_mismatch"
   const void *fUncompressed;     ///< raw page bytes before compression (may be null)
   size_t fUncompressedSize;
   const void *fCompressedLc;       ///< full ROOT LC frame (9-byte header + spectra8 payload)
   size_t fCompressedLcSize;
   int fCxLevel;                  ///< compression level if known, else -1
   const char *fStatusMessage;    ///< human-readable error (may be null)
};
/// Write fUncompressed / fCompressedLc payloads and a .meta sidecar under /tmp/root-spectra8-failure-<pid>/.
/// No-op unless compiled with `-DR__Spectra8_FAILURE_DUMP`.
void R__DumpSpectra8Failure(const struct R__Spectra8FailureDumpInfo *info);

#ifdef __cplusplus
}
#endif

#endif
