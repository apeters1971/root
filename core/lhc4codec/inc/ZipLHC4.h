// Original Author: ROOT Team
/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_ZipLHC4
#define ROOT_ZipLHC4

#ifdef __cplusplus
extern "C" {
#endif

enum {
   kLHC4CodecLz = 0,
   kLHC4CodecBwt = 1,
   kLHC4CodecZstd = 2,
   kLHC4CodecBzip3 = 3,
   kLHC4CodecLzma = 4,
   kLHC4CodecAuto = 5,
};

void R__zipLHC4(int cxlevel, int *srcsize, const char *src, int *tgtsize, char *tgt, int *irep);
void R__unzipLHC4(int *srcsize, const unsigned char *src, int *tgtsize, unsigned char *tgt, int *irep);

/// Select the lhc4codec backend (Lz, Bwt, Zstd, Bzip3, Lzma, Auto). Defaults to Lz.
void R__SetLHC4Codec(int codec);
int R__GetLHC4Codec(void);
/// Returns 1 when the requested backend was linked into lhc4codec, else 0.
/// Auto is always available; it races whichever optional backends are linked.
int R__LHC4CodecAvailable(int codec);

/// Auto only: min % size gain to prefer a slower decoder (default 1). 0 = smallest wins.
void R__SetLHC4AutoMinGainPct(int pct);
int R__GetLHC4AutoMinGainPct(void);

/// Enable lhc4codec byte filters (shuffle/delta/zigzag/dict) for columnar data.
void R__SetLHC4Filters(int enable);
int R__GetLHC4Filters(void);

/// With filters: also compress raw input and keep the smaller result (default on).
void R__SetLHC4FilterFallback(int enable);
int R__GetLHC4FilterFallback(void);

/// Try post-filter byte-RLE when it shrinks the intermediate (default on).
void R__SetLHC4FilterRle(int enable);
int R__GetLHC4FilterRle(void);

/// Allow dictionary-remap filters (dict32/64) in auto-detect (default on).
void R__SetLHC4FilterDict(int enable);
int R__GetLHC4FilterDict(void);

/// Legacy convenience: enable BWT block mode (sets codec to Bwt/Lz).
void R__SetLHC4Bwt(int enable);
int R__GetLHC4Bwt(void);

/// Reset aggregated lhc4codec compression statistics.
void R__ResetLHC4CompressStats(void);
/// Print aggregated lhc4codec compression statistics to stdout.
void R__PrintLHC4CompressStats(void);

/// Page-outcome counters from the most recent compress-stats window.
struct R__LHC4CompressStatsSummary {
   unsigned long long num_pages;
   unsigned long long filtered_won_pages;
   unsigned long long raw_won_pages;
   unsigned long long no_transform_pages;
   unsigned long long stored_fallback_pages;
   unsigned long long plain_pages;
};
void R__GetLHC4CompressStatsSummary(struct R__LHC4CompressStatsSummary *out);

/// Auto-mode winner histogram (indexed by kLHC4Codec*; only populated for lhc4_auto runs).
struct R__LHC4AutoCodecStatsSummary {
   unsigned long long auto_selections;
   unsigned long long codec_hits[6];
};
void R__GetLHC4AutoCodecStatsSummary(struct R__LHC4AutoCodecStatsSummary *out);

#ifdef __cplusplus
}
#endif

#endif
