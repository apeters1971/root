// Original Author: ROOT Team

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ZipSpectra8.h"

#include "spectra8/spectra8.hpp"
#include "spectra8/spectra8_c.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

#if !defined(R__unlikely)
# define R__unlikely(expr) __builtin_expect(!!(expr), 0)
#endif

static const int kHeaderSize = 9;
static const unsigned char kSpectra8Version = 1;

// Crystal at level kDefaultSpectra8, with byte filters. Unzip asks for GPU
// decode of Crystal lattice and profile blocks.
static int R__Spectra8Codec = static_cast<int>(spectra8::Codec::Crystal);
static int R__Spectra8Filters = 1;
static int R__Spectra8FilterFallback = 1;
static int R__Spectra8FilterRle = 1;
static int R__Spectra8FilterDict = 1;
static unsigned long long R__Spectra8FilterMask = ~0ull;
static int R__Spectra8AutoMinGainPct = 1;
static int R__Spectra8AutoMaxLevel = 0;
static unsigned R__Spectra8AutoCodecs = kSpectra8AutoCodecsRoot;
static int R__Spectra8Profile = kSpectra8ProfileNone;
static unsigned R__Spectra8ProfileId = 0;
static int R__Spectra8ProfilePerColumn = 0;
// -1: this thread follows R__Spectra8Profile. Any other value is the profile for zip calls here.
static thread_local int R__Spectra8CallProfile = -1;
static thread_local unsigned R__Spectra8CallProfileId = 0;
// -1 / 0: this thread follows the process-wide filter knobs.
static thread_local int R__Spectra8CallFilters = -1;
static thread_local int R__Spectra8CallFilterFallback = -1;
static thread_local unsigned long long R__Spectra8CallFilterMask = 0;
// -1: this thread follows R__Spectra8Codec.
static thread_local int R__Spectra8CallCodec = -1;
static int R__Spectra8LastCxLevel = -1;

namespace {

#ifdef R__Spectra8_FAILURE_DUMP
std::atomic<unsigned> gFailureDumpCounter{0};

const char *CodecName(int codec)
{
   switch (codec) {
   case kSpectra8CodecBwt: return "bwt";
   case kSpectra8CodecZstd: return "zstd";
   case kSpectra8CodecBzip3: return "bzip3";
   case kSpectra8CodecLzma: return "lzma";
   case kSpectra8CodecAuto: return "auto";
   case kSpectra8CodecMosaic: return "mosaic";
   case kSpectra8CodecOracle: return "oracle";
   case kSpectra8CodecCrystal: return "crystal";
   case kSpectra8CodecFlash: return "flash";
   case kSpectra8CodecLaser: return "laser";
   case kSpectra8CodecGrating: return "grating";
   default: return "beam";
   }
}

std::string FailureDumpDirectory()
{
   static std::string dir;
   if (dir.empty()) {
      dir = std::string("/tmp/root-spectra8-failure-") + std::to_string(getpid());
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
   }
   return dir;
}

bool WriteBinaryFile(const std::string &path, const void *data, std::size_t size)
{
   if (!data || size == 0)
      return false;
   std::ofstream out(path, std::ios::binary);
   if (!out)
      return false;
   out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
   return static_cast<bool>(out);
}

void WriteMetaFile(const std::string &path, const R__Spectra8FailureDumpInfo &info)
{
   std::ofstream out(path);
   if (!out)
      return;

   out << "stage=" << (info.fStage ? info.fStage : "") << '\n';
   out << "status=" << (info.fStatusMessage ? info.fStatusMessage : "") << '\n';
   out << "codec=" << CodecName(R__Spectra8Codec) << " (" << R__Spectra8Codec << ")\n";
   out << "cxlevel=" << info.fCxLevel << '\n';
   out << "filters=" << R__GetSpectra8Filters() << '\n';
   out << "filter_fallback=" << R__GetSpectra8FilterFallback() << '\n';
   out << "filter_rle=" << R__GetSpectra8FilterRle() << '\n';
   out << "filter_dict=" << R__GetSpectra8FilterDict() << '\n';
   out << "auto_min_gain_pct=" << R__GetSpectra8AutoMinGainPct() << '\n';
   out << "auto_max_level=" << R__GetSpectra8AutoMaxLevel() << '\n';
   out << "auto_codecs=0x" << std::hex << R__GetSpectra8AutoCodecs() << std::dec << '\n';
   out << "profile=" << R__GetSpectra8Profile() << '\n';
   out << "profile_id=" << R__GetSpectra8ProfileId() << '\n';
   out << "uncompressed_size=" << info.fUncompressedSize << '\n';
   out << "compressed_lc_size=" << info.fCompressedLcSize << '\n';

   if (info.fCompressedLc && info.fCompressedLcSize >= static_cast<size_t>(kHeaderSize)) {
      const auto *lc = static_cast<const unsigned char *>(info.fCompressedLc);
      const std::size_t payload = static_cast<std::size_t>(lc[3]) | (static_cast<std::size_t>(lc[4]) << 8) |
                                (static_cast<std::size_t>(lc[5]) << 16);
      const std::size_t expected = static_cast<std::size_t>(lc[6]) | (static_cast<std::size_t>(lc[7]) << 8) |
                                   (static_cast<std::size_t>(lc[8]) << 16);
      out << "lc_payload_size=" << payload << '\n';
      out << "lc_expected_uncompressed=" << expected << '\n';
   }
}
#endif // R__Spectra8_FAILURE_DUMP

} // namespace

extern "C" void R__DumpSpectra8Failure(const R__Spectra8FailureDumpInfo *info)
{
#ifndef R__Spectra8_FAILURE_DUMP
   (void)info;
#else
   if (!info)
      return;

   const unsigned id = gFailureDumpCounter.fetch_add(1, std::memory_order_relaxed);
   const std::string base = FailureDumpDirectory() + "/" + std::to_string(id);

   WriteMetaFile(base + ".meta", *info);
   if (info->fUncompressed && info->fUncompressedSize > 0)
      WriteBinaryFile(base + ".uncompressed.bin", info->fUncompressed, info->fUncompressedSize);
   if (info->fCompressedLc && info->fCompressedLcSize > 0)
      WriteBinaryFile(base + ".compressed_lc.bin", info->fCompressedLc, info->fCompressedLcSize);

   std::cerr << "Spectra8 failure dump #" << id << " written to " << base
             << ".{meta,uncompressed.bin,compressed_lc.bin} (stage="
             << (info->fStage ? info->fStage : "?") << ", uncompressed=" << info->fUncompressedSize
             << " B, compressed_lc=" << info->fCompressedLcSize << " B, codec=" << CodecName(R__Spectra8Codec)
             << ", cxlevel=" << info->fCxLevel << ")\n";
#endif
}

static spectra8::Codec ToSpectra8Codec(int codec)
{
   switch (codec) {
   case kSpectra8CodecBwt: return spectra8::Codec::Bwt;
   case kSpectra8CodecZstd: return spectra8::Codec::Zstd;
   case kSpectra8CodecBzip3: return spectra8::Codec::Bzip3;
   case kSpectra8CodecLzma: return spectra8::Codec::Lzma;
   case kSpectra8CodecAuto: return spectra8::Codec::Auto;
   case kSpectra8CodecMosaic: return spectra8::Codec::Mosaic;
   case kSpectra8CodecOracle: return spectra8::Codec::Oracle;
   case kSpectra8CodecCrystal: return spectra8::Codec::Crystal;
   case kSpectra8CodecFlash: return spectra8::Codec::Flash;
   case kSpectra8CodecLaser: return spectra8::Codec::Laser;
   case kSpectra8CodecGrating: return spectra8::Codec::Grating;
   default: return spectra8::Codec::Beam;
   }
}

extern "C" void R__SetSpectra8Codec(int codec)
{
   R__Spectra8Codec = codec;
}

extern "C" int R__GetSpectra8Codec(void)
{
   return R__Spectra8Codec;
}

extern "C" int R__Spectra8CodecAvailable(int codec)
{
   return spectra8::codec_available(ToSpectra8Codec(codec)) ? 1 : 0;
}

extern "C" void R__SetSpectra8AutoMinGainPct(int pct)
{
   R__Spectra8AutoMinGainPct = pct;
}

extern "C" int R__GetSpectra8AutoMinGainPct(void)
{
   return R__Spectra8AutoMinGainPct;
}

extern "C" void R__SetSpectra8AutoMaxLevel(int enable)
{
   R__Spectra8AutoMaxLevel = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8AutoMaxLevel(void)
{
   return R__Spectra8AutoMaxLevel;
}

extern "C" void R__SetSpectra8AutoCodecs(unsigned mask)
{
   R__Spectra8AutoCodecs = mask ? mask : kSpectra8AutoCodecsRoot;
}

extern "C" unsigned R__GetSpectra8AutoCodecs(void)
{
   return R__Spectra8AutoCodecs;
}

extern "C" void R__SetSpectra8Filters(int enable)
{
   R__Spectra8Filters = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8Filters(void)
{
   return R__Spectra8Filters;
}

extern "C" void R__SetSpectra8FilterFallback(int enable)
{
   R__Spectra8FilterFallback = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8FilterFallback(void)
{
   return R__Spectra8FilterFallback;
}

extern "C" void R__SetSpectra8FilterRle(int enable)
{
   R__Spectra8FilterRle = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8FilterRle(void)
{
   return R__Spectra8FilterRle;
}

extern "C" void R__SetSpectra8FilterDict(int enable)
{
   R__Spectra8FilterDict = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8FilterDict(void)
{
   return R__Spectra8FilterDict;
}

extern "C" void R__SetSpectra8FilterMask(unsigned long long mask)
{
   R__Spectra8FilterMask = mask ? mask : ~0ull;
}

extern "C" unsigned long long R__GetSpectra8FilterMask(void)
{
   return R__Spectra8FilterMask;
}

extern "C" void R__SetSpectra8Profile(int profile)
{
   if (profile < kSpectra8ProfileNone || profile > kSpectra8ProfileCustom)
      profile = kSpectra8ProfileNone;
   R__Spectra8Profile = profile;
   spectra8::set_thread_profile(static_cast<spectra8::Profile>(R__Spectra8Profile));
}

extern "C" int R__GetSpectra8Profile(void)
{
   return R__Spectra8Profile;
}

extern "C" void R__SetSpectra8ProfilePerColumn(int enable)
{
   R__Spectra8ProfilePerColumn = enable ? 1 : 0;
}

extern "C" int R__GetSpectra8ProfilePerColumn(void)
{
   return R__Spectra8ProfilePerColumn;
}

extern "C" void R__SetSpectra8ProfileId(unsigned id)
{
   R__Spectra8ProfileId = id;
}

extern "C" unsigned R__GetSpectra8ProfileId(void)
{
   return R__Spectra8ProfileId;
}

extern "C" void R__SetSpectra8CallProfile(int profile)
{
   if (profile < -1 || profile > kSpectra8ProfileCustom)
      profile = -1;
   R__Spectra8CallProfile = profile;
}

extern "C" int R__GetSpectra8CallProfile(void)
{
   return R__Spectra8CallProfile;
}

extern "C" void R__SetSpectra8CallProfileId(unsigned id)
{
   R__Spectra8CallProfileId = id;
}

extern "C" unsigned R__GetSpectra8CallProfileId(void)
{
   return R__Spectra8CallProfileId;
}

extern "C" void R__SetSpectra8CallFilterTrial(int filters, unsigned long long mask, int fallback)
{
   R__Spectra8CallFilters = filters;
   R__Spectra8CallFilterMask = mask;
   R__Spectra8CallFilterFallback = fallback;
}

extern "C" void R__SetSpectra8CallCodec(int codec)
{
   R__Spectra8CallCodec = codec;
}

extern "C" int R__GetSpectra8CallCodec(void)
{
   return R__Spectra8CallCodec;
}

static int EffectiveSpectra8Profile()
{
   return R__Spectra8CallProfile >= 0 ? R__Spectra8CallProfile : R__Spectra8Profile;
}

extern "C" int R__Spectra8TrainAndRegisterProfile(unsigned profileId, const void *sample, size_t sampleSize, int stride)
{
   return R__Spectra8TrainAndRegisterProfileLevel(profileId, sample, sampleSize, stride, 5);
}

extern "C" int R__Spectra8TrainAndRegisterProfileLevel(unsigned profileId, const void *sample, size_t sampleSize, int stride, int level)
{
   if (!sample || sampleSize == 0)
      return 0;
   return spectra8::train_and_register_profile(
             profileId, std::span<const std::byte>(static_cast<const std::byte *>(sample), sampleSize), stride, level)
             ? 1
             : 0;
}

extern "C" int R__Spectra8TrainProfileModel(const void *sample, size_t sampleSize, int stride,
                                        void *outModel, size_t *outModelSize)
{
   return R__Spectra8TrainProfileModelLevel(sample, sampleSize, stride, 5, outModel, outModelSize);
}

extern "C" int R__Spectra8TrainProfileModelLevel(const void *sample, size_t sampleSize, int stride, int level,
                                             void *outModel, size_t *outModelSize)
{
   return spectra8_train_profile_model_level(sample, sampleSize, stride, level, outModel, outModelSize);
}

extern "C" int R__Spectra8RegisterProfileModel(unsigned profileId, const void *modelBytes, size_t modelSize)
{
   return spectra8_register_profile_model(profileId, modelBytes, modelSize);
}

extern "C" int R__Spectra8ExportProfileModel(unsigned profileId, void *outModel, size_t *outModelSize)
{
   return spectra8_export_profile_model(profileId, outModel, outModelSize);
}

extern "C" void R__SetSpectra8Bwt(int enable)
{
   if (enable)
      R__Spectra8Codec = kSpectra8CodecBwt;
   else if (R__Spectra8Codec == kSpectra8CodecBwt)
      R__Spectra8Codec = kSpectra8CodecCrystal;
}

extern "C" int R__GetSpectra8Bwt(void)
{
   return R__Spectra8Codec == kSpectra8CodecBwt ? 1 : 0;
}

static spectra8::CompressParams MakeSpectra8CompressParams(int cxlevel)
{
   spectra8::CompressParams params;
   // spectra8 treats level 0 as the Flash codec. Keep ROOT's Spectra8 levels on the
   // selected backend (1..kMaxLevel).
   if (cxlevel < 1)
      cxlevel = 1;
   if (cxlevel > spectra8::kMaxLevel)
      cxlevel = spectra8::kMaxLevel;
   params.codec = ToSpectra8Codec(R__Spectra8CallCodec >= 0 ? R__Spectra8CallCodec : R__Spectra8Codec);
   params.level = cxlevel;
   params.filters = (R__Spectra8CallFilters >= 0 ? R__Spectra8CallFilters : R__Spectra8Filters) != 0;
   params.filter_fallback = (R__Spectra8CallFilterFallback >= 0 ? R__Spectra8CallFilterFallback : R__Spectra8FilterFallback) != 0;
   params.filter_rle = R__Spectra8FilterRle != 0;
   params.filter_dict = R__Spectra8FilterDict != 0;
   params.filter_mask = R__Spectra8CallFilterMask ? R__Spectra8CallFilterMask : R__Spectra8FilterMask;
   params.auto_min_gain_pct = R__Spectra8AutoMinGainPct;
   params.auto_max_level = R__Spectra8AutoMaxLevel != 0;
   params.auto_codecs = R__Spectra8AutoCodecs;
   params.profile = static_cast<spectra8::Profile>(EffectiveSpectra8Profile());
   // A per-call profile is a builtin id unless call profile ID is set. The process-wide id applies only to the forced profile.
   if (R__Spectra8CallProfile >= 0) {
      params.profile_id = R__Spectra8CallProfileId ? R__Spectra8CallProfileId : static_cast<unsigned>(R__Spectra8CallProfile);
   } else {
      params.profile_id = R__Spectra8ProfileId;
   }
   return params;
}

static void DumpZipFailure(int cxlevel, int srcsize, const char *src, const char *status)
{
   R__Spectra8FailureDumpInfo dump{};
   dump.fStage = "zip";
   dump.fUncompressed = src;
   dump.fUncompressedSize = static_cast<size_t>(srcsize);
   dump.fCxLevel = cxlevel;
   dump.fStatusMessage = status;
   R__DumpSpectra8Failure(&dump);
}

void R__zipSpectra8(int cxlevel, int *srcsize, const char *src, int *tgtsize, char *tgt, int *irep)
{
   *irep = 0;
   R__Spectra8LastCxLevel = cxlevel;
   // CompressParams carries the profile. Also set the spectra8 thread-local default
   // so this worker matches the column (or the process-wide override).
   spectra8::set_thread_profile(static_cast<spectra8::Profile>(EffectiveSpectra8Profile()));

   const auto params = MakeSpectra8CompressParams(cxlevel);
   if (R__unlikely(!spectra8::codec_available(params.codec))) {
      std::cerr << "Error in zip Spectra8: requested codec is not available in this build" << std::endl;
      DumpZipFailure(cxlevel, *srcsize, src, "requested codec is not available in this build");
      return;
   }

   const auto result = spectra8::compress(
      std::span<const std::byte>(reinterpret_cast<const std::byte *>(src), static_cast<size_t>(*srcsize)),
      std::span<std::byte>(reinterpret_cast<std::byte *>(&tgt[kHeaderSize]),
                           static_cast<size_t>(*tgtsize - kHeaderSize)),
      params);

   if (R__unlikely(!result.ok())) {
      const std::string msg(spectra8::status_message(result.status));
      if (R__unlikely(result.status != spectra8::Status::BufferTooSmall)) {
         std::cerr << "Error in zip Spectra8: " << msg << std::endl;
         DumpZipFailure(cxlevel, *srcsize, src, msg.c_str());
      }
      return;
   }

   const size_t written = result.value;
   *irep = static_cast<int>(written + kHeaderSize);

   const size_t deflate_size = written;
   const size_t inflate_size = static_cast<size_t>(*srcsize);
   tgt[0] = 'L';
   tgt[1] = 'C';
   tgt[2] = static_cast<char>(kSpectra8Version);
   tgt[3] = deflate_size & 0xff;
   tgt[4] = (deflate_size >> 8) & 0xff;
   tgt[5] = (deflate_size >> 16) & 0xff;
   tgt[6] = inflate_size & 0xff;
   tgt[7] = (inflate_size >> 8) & 0xff;
   tgt[8] = (inflate_size >> 16) & 0xff;
}

void R__unzipSpectra8(int *srcsize, const unsigned char *src, int *tgtsize, unsigned char *tgt, int *irep)
{
   *irep = 0;

   if (R__unlikely(src[0] != 'L' || src[1] != 'C')) {
      std::cerr << "R__unzipSpectra8: algorithm run against buffer with incorrect header (got " << src[0] << src[1]
                << "; expected LC)." << std::endl;
      R__Spectra8FailureDumpInfo dump{};
      dump.fStage = "unzip";
      dump.fCompressedLc = src;
      dump.fCompressedLcSize = static_cast<size_t>(*srcsize);
      dump.fUncompressedSize = static_cast<size_t>(*tgtsize);
      dump.fCxLevel = R__Spectra8LastCxLevel;
      dump.fStatusMessage = "incorrect LC header";
      R__DumpSpectra8Failure(&dump);
      return;
   }

   if (R__unlikely(src[2] != kSpectra8Version)) {
      std::cerr << "R__unzipSpectra8: incompatible Spectra8 on-disk version (got " << static_cast<int>(src[2]) << "; expected "
                << static_cast<int>(kSpectra8Version) << ")" << std::endl;
      R__Spectra8FailureDumpInfo dump{};
      dump.fStage = "unzip";
      dump.fCompressedLc = src;
      dump.fCompressedLcSize = static_cast<size_t>(*srcsize);
      dump.fUncompressedSize = static_cast<size_t>(*tgtsize);
      dump.fCxLevel = R__Spectra8LastCxLevel;
      dump.fStatusMessage = "incompatible Spectra8 on-disk version";
      R__DumpSpectra8Failure(&dump);
      return;
   }

   spectra8::DecompressParams decompressParams;
   decompressParams.gpu = true;
   const auto result = spectra8::decompress(
      std::span<const std::byte>(reinterpret_cast<const std::byte *>(&src[kHeaderSize]),
                                 static_cast<size_t>(*srcsize - kHeaderSize)),
      std::span<std::byte>(reinterpret_cast<std::byte *>(tgt), static_cast<size_t>(*tgtsize)),
      decompressParams);

   if (R__unlikely(!result.ok())) {
      const std::string msg(spectra8::status_message(result.status));
      if (R__unlikely(result.status != spectra8::Status::BufferTooSmall)) {
         std::cerr << "Error in unzip Spectra8: " << msg << std::endl;
         R__Spectra8FailureDumpInfo dump{};
         dump.fStage = "unzip";
         dump.fCompressedLc = src;
         dump.fCompressedLcSize = static_cast<size_t>(*srcsize);
         dump.fUncompressedSize = static_cast<size_t>(*tgtsize);
         dump.fCxLevel = R__Spectra8LastCxLevel;
         dump.fStatusMessage = msg.c_str();
         R__DumpSpectra8Failure(&dump);
      }
      return;
   }

   *irep = static_cast<int>(result.value);
}

extern "C" void R__ResetSpectra8CompressStats(void)
{
   spectra8::reset_compress_stats();
}

extern "C" void R__PrintSpectra8CompressStats(void)
{
   const auto stats = spectra8::get_compress_stats();
   std::cout << spectra8::format_compress_stats(stats);
}

extern "C" void R__GetSpectra8CompressStatsSummary(R__Spectra8CompressStatsSummary *out)
{
   if (!out)
      return;
   const auto stats = spectra8::get_compress_stats();
   out->num_pages = stats.num_pages;
   out->uncompressed_bytes = stats.uncompressed_bytes;
   out->compressed_bytes = stats.compressed_bytes;
   out->filtered_won_pages = stats.filtered_won_pages;
   out->raw_won_pages = stats.raw_won_pages;
   out->no_transform_pages = stats.no_transform_pages;
   out->stored_fallback_pages = stats.stored_fallback_pages;
   out->plain_pages = stats.plain_pages;
   out->crystal_profile_blocks = stats.crystal_profile_blocks;
   out->crystal_profile_escapes = stats.crystal_profile_escapes;
}

extern "C" void R__GetSpectra8AutoCodecStatsSummary(R__Spectra8AutoCodecStatsSummary *out)
{
   if (!out)
      return;
   const auto stats = spectra8::get_compress_stats();
   out->auto_selections = stats.auto_selections;
   // The library counts hits only for the codecs Auto can race; its array may be
   // shorter than ROOT's enum (Grating, for instance, is never raced).
   constexpr int kLibHits = static_cast<int>(std::size(stats.auto_codec_hits));
   for (int i = 0; i < kSpectra8CodecCount; ++i)
      out->codec_hits[i] = (i < kLibHits) ? stats.auto_codec_hits[i] : 0ull;
}
