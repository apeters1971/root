// Original Author: ROOT Team

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ZipLHC4.h"

#include "lhc4codec/lhc4codec.hpp"

#include <cstddef>
#include <iostream>

#if !defined(R__unlikely)
# define R__unlikely(expr) __builtin_expect(!!(expr), 0)
#endif

static const int kHeaderSize = 9;
static const unsigned char kLHC4Version = 1;

static int R__LHC4Codec = static_cast<int>(lhc4codec::Codec::Lz);
static int R__LHC4Filters = 0;
static int R__LHC4FilterFallback = 1;
static int R__LHC4FilterRle = 1;
static int R__LHC4FilterDict = 1;
static int R__LHC4AutoMinGainPct = 1;

static lhc4codec::Codec ToLHC4Codec(int codec)
{
   switch (codec) {
   case kLHC4CodecBwt: return lhc4codec::Codec::Bwt;
   case kLHC4CodecZstd: return lhc4codec::Codec::Zstd;
   case kLHC4CodecBzip3: return lhc4codec::Codec::Bzip3;
   case kLHC4CodecLzma: return lhc4codec::Codec::Lzma;
   case kLHC4CodecAuto: return lhc4codec::Codec::Auto;
   default: return lhc4codec::Codec::Lz;
   }
}

extern "C" void R__SetLHC4Codec(int codec)
{
   R__LHC4Codec = codec;
}

extern "C" int R__GetLHC4Codec(void)
{
   return R__LHC4Codec;
}

extern "C" int R__LHC4CodecAvailable(int codec)
{
   return lhc4codec::codec_available(ToLHC4Codec(codec)) ? 1 : 0;
}

extern "C" void R__SetLHC4AutoMinGainPct(int pct)
{
   R__LHC4AutoMinGainPct = pct;
}

extern "C" int R__GetLHC4AutoMinGainPct(void)
{
   return R__LHC4AutoMinGainPct;
}

extern "C" void R__SetLHC4Filters(int enable)
{
   R__LHC4Filters = enable ? 1 : 0;
}

extern "C" int R__GetLHC4Filters(void)
{
   return R__LHC4Filters;
}

extern "C" void R__SetLHC4FilterFallback(int enable)
{
   R__LHC4FilterFallback = enable ? 1 : 0;
}

extern "C" int R__GetLHC4FilterFallback(void)
{
   return R__LHC4FilterFallback;
}

extern "C" void R__SetLHC4FilterRle(int enable)
{
   R__LHC4FilterRle = enable ? 1 : 0;
}

extern "C" int R__GetLHC4FilterRle(void)
{
   return R__LHC4FilterRle;
}

extern "C" void R__SetLHC4FilterDict(int enable)
{
   R__LHC4FilterDict = enable ? 1 : 0;
}

extern "C" int R__GetLHC4FilterDict(void)
{
   return R__LHC4FilterDict;
}

extern "C" void R__SetLHC4Bwt(int enable)
{
   if (enable)
      R__LHC4Codec = kLHC4CodecBwt;
   else if (R__LHC4Codec == kLHC4CodecBwt)
      R__LHC4Codec = kLHC4CodecLz;
}

extern "C" int R__GetLHC4Bwt(void)
{
   return R__LHC4Codec == kLHC4CodecBwt ? 1 : 0;
}

static lhc4codec::CompressParams MakeLHC4CompressParams(int cxlevel)
{
   lhc4codec::CompressParams params;
   if (cxlevel < lhc4codec::kMinLevel)
      cxlevel = lhc4codec::kMinLevel;
   if (cxlevel > lhc4codec::kMaxLevel)
      cxlevel = lhc4codec::kMaxLevel;
   params.codec = ToLHC4Codec(R__LHC4Codec);
   params.level = cxlevel;
   params.filters = R__LHC4Filters != 0;
   params.filter_fallback = R__LHC4FilterFallback != 0;
   params.filter_rle = R__LHC4FilterRle != 0;
   params.filter_dict = R__LHC4FilterDict != 0;
   params.auto_min_gain_pct = R__LHC4AutoMinGainPct;
   return params;
}

void R__zipLHC4(int cxlevel, int *srcsize, const char *src, int *tgtsize, char *tgt, int *irep)
{
   *irep = 0;

   const auto params = MakeLHC4CompressParams(cxlevel);
   if (R__unlikely(!lhc4codec::codec_available(params.codec))) {
      std::cerr << "Error in zip LHC4: requested codec is not available in this build" << std::endl;
      return;
   }

   const auto result = lhc4codec::compress(
      std::span<const std::byte>(reinterpret_cast<const std::byte *>(src), static_cast<size_t>(*srcsize)),
      std::span<std::byte>(reinterpret_cast<std::byte *>(&tgt[kHeaderSize]),
                           static_cast<size_t>(*tgtsize - kHeaderSize)),
      params);

   if (R__unlikely(!result.ok())) {
      if (R__unlikely(result.status != lhc4codec::Status::BufferTooSmall)) {
         std::cerr << "Error in zip LHC4: " << lhc4codec::status_message(result.status) << std::endl;
      }
      return;
   }

   const size_t written = result.value;
   *irep = static_cast<int>(written + kHeaderSize);

   const size_t deflate_size = written;
   const size_t inflate_size = static_cast<size_t>(*srcsize);
   tgt[0] = 'L';
   tgt[1] = 'C';
   tgt[2] = static_cast<char>(kLHC4Version);
   tgt[3] = deflate_size & 0xff;
   tgt[4] = (deflate_size >> 8) & 0xff;
   tgt[5] = (deflate_size >> 16) & 0xff;
   tgt[6] = inflate_size & 0xff;
   tgt[7] = (inflate_size >> 8) & 0xff;
   tgt[8] = (inflate_size >> 16) & 0xff;
}

void R__unzipLHC4(int *srcsize, const unsigned char *src, int *tgtsize, unsigned char *tgt, int *irep)
{
   *irep = 0;

   if (R__unlikely(src[0] != 'L' || src[1] != 'C')) {
      std::cerr << "R__unzipLHC4: algorithm run against buffer with incorrect header (got " << src[0] << src[1]
                << "; expected LC)." << std::endl;
      return;
   }

   if (R__unlikely(src[2] != kLHC4Version)) {
      std::cerr << "R__unzipLHC4: incompatible LHC4 on-disk version (got " << static_cast<int>(src[2]) << "; expected "
                << static_cast<int>(kLHC4Version) << ")" << std::endl;
      return;
   }

   const auto result = lhc4codec::decompress(
      std::span<const std::byte>(reinterpret_cast<const std::byte *>(&src[kHeaderSize]),
                                 static_cast<size_t>(*srcsize - kHeaderSize)),
      std::span<std::byte>(reinterpret_cast<std::byte *>(tgt), static_cast<size_t>(*tgtsize)));

   if (R__unlikely(!result.ok())) {
      if (R__unlikely(result.status != lhc4codec::Status::BufferTooSmall)) {
         std::cerr << "Error in unzip LHC4: " << lhc4codec::status_message(result.status) << std::endl;
      }
      return;
   }

   *irep = static_cast<int>(result.value);
}

extern "C" void R__ResetLHC4CompressStats(void)
{
   lhc4codec::reset_compress_stats();
}

extern "C" void R__PrintLHC4CompressStats(void)
{
   const auto stats = lhc4codec::get_compress_stats();
   std::cout << lhc4codec::format_compress_stats(stats);
}

extern "C" void R__GetLHC4CompressStatsSummary(R__LHC4CompressStatsSummary *out)
{
   if (!out)
      return;
   const auto stats = lhc4codec::get_compress_stats();
   out->num_pages = stats.num_pages;
   out->filtered_won_pages = stats.filtered_won_pages;
   out->raw_won_pages = stats.raw_won_pages;
   out->no_transform_pages = stats.no_transform_pages;
   out->stored_fallback_pages = stats.stored_fallback_pages;
   out->plain_pages = stats.plain_pages;
}

extern "C" void R__GetLHC4AutoCodecStatsSummary(R__LHC4AutoCodecStatsSummary *out)
{
   if (!out)
      return;
   const auto stats = lhc4codec::get_compress_stats();
   out->auto_selections = stats.auto_selections;
   for (int i = 0; i < 6; ++i)
      out->codec_hits[i] = stats.auto_codec_hits[i];
}
