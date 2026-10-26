#include "ntuple_test.hxx"

#ifdef R__HAS_SPECTRA8
#include "ZipSpectra8.h"

#include <cstdint>
#include <cstring>
#include <vector>
#endif

TEST(RNTupleZip, Basics)
{
   std::string data = "xxxxxxxxxxxxxxxxxxxxxxxx";
   auto zipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), 101, zipBuffer.get());
   EXPECT_LT(szZipped, data.length());
   auto unzipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get());
   EXPECT_EQ(data, std::string_view(unzipBuffer.get(), data.length()));
}

TEST(RNTupleZip, Empty)
{
   char x = 0;
   char z;
   EXPECT_EQ(0U, RNTupleCompressor::Zip(&x, 0, 0, &z));
   EXPECT_EQ(0U, RNTupleCompressor::Zip(&x, 0, 101, &z));

   // Don't crash
   RNTupleDecompressor::Unzip(&x, 0, 0, &x);
}

TEST(RNTupleZip, Uncompressed)
{
   char X = 'x';
   char Z;
   EXPECT_EQ(1U, RNTupleCompressor::Zip(&X, 1, 0, &Z));
   RNTupleDecompressor::Unzip(&Z, 1, 1, &X);
   EXPECT_EQ('x', X);
}

TEST(RNTupleZip, Small)
{
   char X = 'x';
   char Z;
   EXPECT_EQ(1U, RNTupleCompressor::Zip(&X, 1, 101, &Z));
   RNTupleDecompressor::Unzip(&Z, 1, 1, &X);
   EXPECT_EQ('x', X);
}

TEST(RNTupleZip, LargeWithOutputBuffer)
{
   constexpr unsigned int N = kMAXZIPBUF + 32;
   auto zipBuffer = MakeUninitArray<unsigned char>(N);
   auto unzipBuffer = MakeUninitArray<char>(N);
   std::string data(N, 'x');

   /// Trailing byte cannot be compressed, entire buffer returns uncompressed
   auto szZip = RNTupleCompressor::Zip(data.data(), kMAXZIPBUF + 1, 101, zipBuffer.get());
   EXPECT_EQ(static_cast<unsigned int>(kMAXZIPBUF) + 1, szZip);

   szZip = RNTupleCompressor::Zip(data.data(), data.length(), 101, zipBuffer.get());
   EXPECT_LT(szZip, N);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZip, N, unzipBuffer.get());
   EXPECT_EQ(data, std::string_view(unzipBuffer.get(), N));
}

TEST(RNTupleZip, CorruptedInput)
{
   std::string data = "xxxxxxxxxxxxxxxxxxxxxxxx";
   auto zipBuffer = MakeUninitArray<unsigned char>(data.length());
   auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), 101, zipBuffer.get());
   EXPECT_LT(szZipped, data.length());
   auto unzipBuffer = MakeUninitArray<unsigned char>(data.length());
   // corrupt the buffer header
   memset(zipBuffer.get() + 1, 0xCD, 5);
   EXPECT_THROW(RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get()),
                ROOT::RException);
}

#ifdef R__HAS_SPECTRA8
TEST(RNTupleZip, Spectra8)
{
   std::string data = "xxxxxxxxxxxxxxxxxxxxxxxx";
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8,
                                                     ROOT::RCompressionSetting::ELevel::kDefaultSpectra8);
   auto zipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), compression, zipBuffer.get());
   EXPECT_LT(szZipped, data.length());
   auto unzipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get());
   EXPECT_EQ(data, std::string_view(unzipBuffer.get(), data.length()));
}

TEST(RNTupleZip, Spectra8Filters)
{
   R__SetSpectra8Filters(1);
   R__SetSpectra8Bwt(0);
   std::vector<std::uint32_t> column(1024);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 6);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8Bwt)
{
   R__SetSpectra8Filters(0);
   R__SetSpectra8Codec(kSpectra8CodecBwt);
   std::string data(4096, 'a');
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 6);
   auto zipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), compression, zipBuffer.get());
   EXPECT_LT(szZipped, data.length());
   auto unzipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get());
   EXPECT_EQ(data, std::string_view(unzipBuffer.get(), data.length()));
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8FilterOptions)
{
   R__SetSpectra8Codec(kSpectra8CodecBeam);
   R__SetSpectra8Filters(1);
   R__SetSpectra8FilterFallback(0);
   R__SetSpectra8FilterRle(1);
   R__SetSpectra8FilterDict(0);
   std::vector<std::uint32_t> column(1024);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 6);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
   R__SetSpectra8FilterFallback(1);
   R__SetSpectra8FilterDict(1);
}

TEST(RNTupleZip, Spectra8ExternalCodec)
{
   if (!R__Spectra8CodecAvailable(kSpectra8CodecZstd))
      GTEST_SKIP() << "spectra8 zstd backend not available";

   R__SetSpectra8Codec(kSpectra8CodecZstd);
   R__SetSpectra8Filters(0);
   std::string data(4096, 'x');
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 6);
   auto zipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), compression, zipBuffer.get());
   EXPECT_LT(szZipped, data.length());
   auto unzipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get());
   EXPECT_EQ(data, std::string_view(unzipBuffer.get(), data.length()));
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8NativeCodecs)
{
   const int codecs[] = {kSpectra8CodecMosaic, kSpectra8CodecCrystal, kSpectra8CodecOracle,
                         kSpectra8CodecFlash,  kSpectra8CodecLaser,   kSpectra8CodecGrating};
   std::string data(4096, 'x');
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
   for (int codec : codecs) {
      ASSERT_TRUE(R__Spectra8CodecAvailable(codec));
      R__SetSpectra8Codec(codec);
      R__SetSpectra8Filters(1);
      auto zipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
      auto szZipped = RNTupleCompressor::Zip(data.data(), data.length(), compression, zipBuffer.get());
      EXPECT_LT(szZipped, data.length()) << "codec " << codec;
      auto unzipBuffer = std::unique_ptr<char[]>(new char[data.length()]);
      RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, data.length(), unzipBuffer.get());
      EXPECT_EQ(data, std::string_view(unzipBuffer.get(), data.length())) << "codec " << codec;
   }
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8Auto)
{
   R__SetSpectra8Codec(kSpectra8CodecAuto);
   R__SetSpectra8AutoMinGainPct(1);
   R__SetSpectra8Filters(1);
   std::vector<std::uint32_t> column(1024);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8AutoMaxLevel)
{
   R__SetSpectra8Codec(kSpectra8CodecAuto);
   R__SetSpectra8AutoMinGainPct(1);
   R__SetSpectra8AutoMaxLevel(1);
   R__SetSpectra8Filters(1);
   std::vector<std::uint32_t> column(1024);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));
   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8AutoMaxLevel(0);
   R__SetSpectra8Filters(1);
}

TEST(RNTupleZip, Spectra8AutoCodecs)
{
   EXPECT_EQ(kSpectra8AutoCodecsRoot, R__GetSpectra8AutoCodecs());

   R__SetSpectra8Codec(kSpectra8CodecAuto);
   R__SetSpectra8AutoMinGainPct(0);
   R__SetSpectra8AutoCodecs(kSpectra8CodecBit(kSpectra8CodecBeam));
   R__SetSpectra8Filters(1);
   R__ResetSpectra8CompressStats();

   std::vector<std::uint32_t> column(1024);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));

   R__Spectra8AutoCodecStatsSummary stats{};
   R__GetSpectra8AutoCodecStatsSummary(&stats);
   EXPECT_GT(stats.auto_selections, 0u);
   EXPECT_EQ(stats.codec_hits[kSpectra8CodecBeam], stats.auto_selections);
   for (int i = 0; i < kSpectra8CodecCount; ++i) {
      if (i != kSpectra8CodecBeam)
         EXPECT_EQ(0u, stats.codec_hits[i]) << "codec " << i;
   }

   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8AutoCodecs(0);
   R__SetSpectra8AutoMinGainPct(1);
   R__SetSpectra8Filters(1);
   EXPECT_EQ(kSpectra8AutoCodecsRoot, R__GetSpectra8AutoCodecs());
}

TEST(RNTupleZip, Spectra8Profile)
{
   EXPECT_EQ(kSpectra8ProfileNone, R__GetSpectra8Profile());

   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(0);
   R__SetSpectra8Profile(kSpectra8ProfileIntMonotonic);
   R__SetSpectra8ProfileId(0);

   std::vector<std::uint32_t> column(4096);
   for (std::size_t i = 0; i < column.size(); ++i)
      column[i] = static_cast<std::uint32_t>(i);
   const auto *data = reinterpret_cast<const char *>(column.data());
   const auto dataLen = column.size() * sizeof(std::uint32_t);
   const int compression = ROOT::CompressionSettings(ROOT::RCompressionSetting::EAlgorithm::kSpectra8, 5);
   auto zipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   auto szZipped = RNTupleCompressor::Zip(data, dataLen, compression, zipBuffer.get());
   EXPECT_LT(szZipped, dataLen);
   auto unzipBuffer = std::unique_ptr<char[]>(new char[dataLen]);
   RNTupleDecompressor::Unzip(zipBuffer.get(), szZipped, dataLen, unzipBuffer.get());
   EXPECT_EQ(0, std::memcmp(data, unzipBuffer.get(), dataLen));

   R__SetSpectra8Codec(kSpectra8CodecCrystal);
   R__SetSpectra8Filters(1);
   R__SetSpectra8Profile(kSpectra8ProfileNone);
   R__SetSpectra8ProfileId(0);
   EXPECT_EQ(kSpectra8ProfileNone, R__GetSpectra8Profile());
}
#endif
