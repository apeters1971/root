/// \file rntuple2rntuple.cxx
/// \brief Re-encode an RNTuple with ROOT's native page compressors.

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RNTupleMerger.hxx>
#include <ROOT/RNTupleReadOptions.hxx>
#include <ROOT/RNTupleWriteOptions.hxx>
#include <ROOT/RPageStorage.hxx>
#include <ROOT/RPageStorageFile.hxx>

#include "RNTupleBench.hxx"

#include <Compression.h>
#include <TFile.h>
#include <TKey.h>
#include <TROOT.h>
#include <TString.h>
#include <TSystem.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using ROOT::RNTupleReadOptions;
using ROOT::RNTupleWriteOptions;
using ROOT::Experimental::Internal::RNTupleMergeOptions;
using ROOT::Experimental::Internal::RNTupleMerger;
using ROOT::Internal::RPageSinkFile;
using ROOT::Internal::RPageSource;
using EAlgorithm = ROOT::RCompressionSetting::EAlgorithm;
using ELevel = ROOT::RCompressionSetting::ELevel;

constexpr const char *kUsage = R"USAGE(Usage: rntuple2rntuple [options] input.root [ntupleName] [outputBase]

Re-encode an RNTuple into one file per native ROOT compressor. Column
encodings and page boundaries are kept; each page is rewritten with the
selected algorithm. Streamer fields need their C++ dictionaries. To estimate
page sizes without dictionaries, use rntupletryzip.

Options:
  --page-size N            Forwarded to the destination RNTupleWriteOptions.
  --initial-page-size N    Forwarded to the destination RNTupleWriteOptions.
  --level N                Compression level for every selected variant.
                           Default levels: zstd 5, lzma 7, zlib 1, lz4 4.
                           zstd accepts 1..19; the others accept 1..9.
  --only-variant LIST      Comma-separated variants. A trailing level overrides
                           --level for that variant (native_lzma9). Families:
                           native_zstd, native_lzma, native_zlib, native_lz4,
                           uncompressed.
  --jobs N                 Up to N variants at once (default: 1). 0 = one job
                           per CPU, capped by the variant count. Requires fork().
  --no-out                 Re-encode, print the summary, and delete the outputs.
  --quiet                  Suppress per-variant progress.
  --bench                  After re-encoding, benchmark page decompression and
                           LoadEntry on one thread and with implicit MT.
  --bench-only             Do not re-encode. Benchmark the given RNTuple file(s).
                           With two arguments, the second is the RNTuple name
                           when it does not contain ".root".
  --bench-threads N        Threads for the multi-core LoadEntry benchmark
                           (default: hardware concurrency).
  --bench-iters N          Passes for the page-decompression benchmark (default: 5).
  --bench-entries N        Entries to read (default: 0 = all).
  --bench-percent N        First N percent of pages and entries (default: 100).
  --worker-result PATH     Internal: write one variant's metrics to PATH.
  -h, --help               Show this help.

Default variants:

  <outputBase>_native_zstd5.root
  <outputBase>_native_lzma7.root
  <outputBase>_native_zlib1.root
  <outputBase>_native_lz4.root

If ntupleName is omitted, the first RNTuple in the file is used.
If outputBase is omitted, it defaults to "<input-stem>_recode".
)USAGE";

struct Spec {
   const char *fFamily;
   const char *fSuffix;
   EAlgorithm::EValues fAlgorithm;
   int fDefaultLevel;
   bool fCompressed;
};

constexpr Spec kSpecs[] = {
   {"native_zstd", "_native_zstd", EAlgorithm::kZSTD, ELevel::kDefaultZSTD, true},
   {"native_lzma", "_native_lzma", EAlgorithm::kLZMA, ELevel::kDefaultLZMA, true},
   {"native_zlib", "_native_zlib", EAlgorithm::kZLIB, ELevel::kDefaultZLIB, true},
   {"native_lz4", "_native_lz4", EAlgorithm::kLZ4, ELevel::kDefaultLZ4, true},
   {"uncompressed", "_uncompressed", EAlgorithm::kZSTD, 0, false},
};

struct RunOptions {
   std::optional<std::size_t> fPageSize;
   std::optional<std::size_t> fInitialPageSize;
   int fLevel = 0;
   bool fLevelExplicit = false;
   std::string fOnlyVariant;
   int fJobs = 1;
   bool fNoOut = false;
   bool fQuiet = false;
   std::string fWorkerResult;
   std::string fInputFile;
   std::string fNTupleName;
   std::string fOutputBase;
   bool fBench = false;
   bool fBenchOnly = false;
   std::vector<std::string> fPositionalArgs;
   int fBenchThreads = 0;
   int fBenchIters = 5;
   std::uint64_t fBenchEntries = 0;
   double fBenchPercent = 100.0;
};

struct Variant {
   std::string fFamily;
   std::string fLabel;
   std::string fSuffix;
   int fLevel = 0;
   RNTupleWriteOptions fOptions;
};

struct VariantResult {
   std::string fLabel;
   std::uint64_t fOutputBytes = 0;
   double fElapsedSec = 0.0;
};

struct Request {
   const Spec *fSpec = nullptr;
   std::optional<int> fLevel;
};

std::string BasenameNoExt(std::string path)
{
   const auto slash = path.find_last_of("/\\");
   if (slash != std::string::npos)
      path.erase(0, slash + 1);
   const auto dot = path.rfind('.');
   if (dot != std::string::npos)
      path.erase(dot);
   return path;
}

void RemoveIfExists(const std::string &path)
{
   if (!gSystem->AccessPathName(path.c_str()))
      gSystem->Unlink(path.c_str());
}

std::string MakeTempRootPath()
{
   TString tmp;
   FILE *f = gSystem->TempFileName(tmp, nullptr, ".root");
   if (!f)
      throw std::runtime_error("failed to create a temporary output file");
   std::fclose(f);
   return std::string(tmp.Data());
}

std::string ResolveVariantOutputPath(const RunOptions &opts, const std::string &suffix)
{
   if (opts.fNoOut)
      return MakeTempRootPath();
   return opts.fOutputBase + suffix + ".root";
}

std::optional<std::uint64_t> FileSizeBytes(const std::string &path)
{
   FileStat_t st{};
   if (gSystem->GetPathInfo(path.c_str(), st) != 0)
      return std::nullopt;
   return static_cast<std::uint64_t>(st.fSize);
}

std::string FormatBytes(std::uint64_t nbytes)
{
   const char *suffix = "B";
   double v = static_cast<double>(nbytes);
   if (v >= 1024.0 * 1024.0 * 1024.0) {
      v /= 1024.0 * 1024.0 * 1024.0;
      suffix = "GiB";
   } else if (v >= 1024.0 * 1024.0) {
      v /= 1024.0 * 1024.0;
      suffix = "MiB";
   } else if (v >= 1024.0) {
      v /= 1024.0;
      suffix = "KiB";
   }
   std::ostringstream os;
   os.setf(std::ios::fixed);
   os.precision(v >= 100.0 ? 0 : (v >= 10.0 ? 1 : 2));
   os << v << ' ' << suffix;
   return os.str();
}

std::string FormatSeconds(double sec)
{
   std::ostringstream os;
   os.setf(std::ios::fixed);
   os.precision(sec >= 100.0 ? 0 : 1);
   os << sec << 's';
   return os.str();
}

std::string FormatRatio(std::uint64_t outputBytes, std::uint64_t inputBytes)
{
   if (inputBytes == 0)
      return "n/a";
   std::ostringstream os;
   os.setf(std::ios::fixed);
   os.precision(3);
   os << static_cast<double>(outputBytes) / static_cast<double>(inputBytes);
   return os.str();
}

std::string FormatPct(double pct)
{
   std::ostringstream os;
   os.setf(std::ios::fixed);
   os.precision(1);
   os << pct << '%';
   return os.str();
}

std::string RequireArg(int &i, int argc, char **argv, const char *opt)
{
   if (i + 1 >= argc)
      throw std::runtime_error(std::string(opt) + " requires an argument");
   return argv[++i];
}

std::string OptionValue(const std::string &arg, const char *opt, int &i, int argc, char **argv)
{
   const std::string prefix = std::string(opt) + "=";
   if (arg.rfind(prefix, 0) == 0)
      return arg.substr(prefix.size());
   if (arg == opt)
      return RequireArg(i, argc, argv, opt);
   return {};
}

int ParseInt(const std::string &text, const char *opt)
{
   try {
      std::size_t idx = 0;
      const int value = std::stoi(text, &idx);
      if (idx != text.size())
         throw std::invalid_argument("trailing");
      return value;
   } catch (const std::exception &) {
      throw std::runtime_error(std::string(opt) + " expects an integer");
   }
}

std::uint64_t ParseU64(const std::string &text, const char *opt)
{
   try {
      std::size_t idx = 0;
      const auto value = std::stoull(text, &idx);
      if (idx != text.size())
         throw std::invalid_argument("trailing");
      return value;
   } catch (const std::exception &) {
      throw std::runtime_error(std::string(opt) + " expects a positive integer");
   }
}

double ParseDouble(const std::string &text, const char *opt)
{
   try {
      std::size_t idx = 0;
      const double value = std::stod(text, &idx);
      if (idx != text.size())
         throw std::invalid_argument("trailing");
      return value;
   } catch (const std::exception &) {
      throw std::runtime_error(std::string(opt) + " expects a number");
   }
}

const Spec *FindSpec(const std::string &family)
{
   for (const auto &spec : kSpecs) {
      if (family == spec.fFamily)
         return &spec;
   }
   return nullptr;
}

std::optional<int> SplitLevelSuffix(const std::string &token, std::string &family)
{
   std::size_t i = token.size();
   while (i > 0 && std::isdigit(static_cast<unsigned char>(token[i - 1])))
      --i;
   if (i == token.size()) {
      family = token;
      return std::nullopt;
   }
   if (i == 0)
      throw std::runtime_error("unknown variant label: " + token);
   family = token.substr(0, i);
   return std::stoi(token.substr(i));
}

std::string KnownFamilies()
{
   std::string out;
   for (const auto &spec : kSpecs) {
      if (!out.empty())
         out += ", ";
      out += spec.fFamily;
   }
   return out;
}

void CheckLevel(const Spec &spec, int level)
{
   if (!spec.fCompressed)
      throw std::runtime_error(std::string(spec.fFamily) + " has no compression level");
   if (level < 1)
      throw std::runtime_error("compression level must be >= 1");
   if (spec.fAlgorithm == EAlgorithm::kZSTD) {
      if (level > 19)
         throw std::runtime_error("zstd level must be between 1 and 19");
   } else if (level > 9) {
      throw std::runtime_error(std::string(spec.fFamily) + " level must be between 1 and 9");
   }
}

int ResolveLevel(const Spec &spec, const std::optional<int> &requested, const RunOptions &opts,
                 const std::string &label)
{
   if (!spec.fCompressed) {
      if (requested)
         throw std::runtime_error(label + " has no compression level");
      return 0;
   }
   if (requested && opts.fLevelExplicit && *requested != opts.fLevel) {
      throw std::runtime_error(label + " asks for level " + std::to_string(*requested) + " but --level is " +
                               std::to_string(opts.fLevel));
   }
   int level = spec.fDefaultLevel;
   if (opts.fLevelExplicit)
      level = opts.fLevel;
   if (requested)
      level = *requested;
   CheckLevel(spec, level);
   return level;
}

std::vector<Request> ParseRequests(const std::string &only)
{
   std::vector<Request> requests;
   if (only.empty()) {
      for (const auto &spec : kSpecs) {
         if (spec.fCompressed)
            requests.push_back(Request{&spec, std::nullopt});
      }
      return requests;
   }

   std::stringstream ss(only);
   std::string item;
   while (std::getline(ss, item, ',')) {
      const auto begin = item.find_first_not_of(" \t");
      if (begin == std::string::npos)
         continue;
      const auto end = item.find_last_not_of(" \t");
      item = item.substr(begin, end - begin + 1);
      std::string family;
      const auto level = SplitLevelSuffix(item, family);
      const Spec *spec = FindSpec(family);
      if (!spec)
         throw std::runtime_error("unknown variant label: " + item + " (known: " + KnownFamilies() + ")");
      requests.push_back(Request{spec, level});
   }
   if (requests.empty())
      throw std::runtime_error("--only-variant list is empty");
   return requests;
}

void ApplyPageSizeOptions(RNTupleWriteOptions &writeOpts, const RunOptions &runOpts)
{
   if (runOpts.fPageSize)
      writeOpts.SetMaxUnzippedPageSize(*runOpts.fPageSize);

   if (runOpts.fInitialPageSize) {
      writeOpts.SetInitialUnzippedPageSize(*runOpts.fInitialPageSize);
      // ReservePage() allocates one initial buffer per column. Scale the auto budget only when
      // the user raises the initial page size above the ROOT default (256 B).
      constexpr std::size_t kDefaultInitialPageSize = 256;
      if (*runOpts.fInitialPageSize > kDefaultInitialPageSize) {
         std::size_t autoBudget = writeOpts.GetApproxZippedClusterSize();
         if (writeOpts.GetCompression() != 0)
            autoBudget += writeOpts.GetApproxZippedClusterSize();
         writeOpts.SetPageBufferBudget(autoBudget * (*runOpts.fInitialPageSize) / kDefaultInitialPageSize);
      }
   }
}

std::string FormatPageSizeSettings(const RunOptions &opts)
{
   if (!opts.fPageSize && !opts.fInitialPageSize)
      return "default (initial=256 B, max=1 MiB)";
   const auto initial = opts.fInitialPageSize ? FormatBytes(*opts.fInitialPageSize) : std::string("256 B (default)");
   const auto maxSize = opts.fPageSize ? FormatBytes(*opts.fPageSize) : std::string("1 MiB (default)");
   return "initial=" + initial + ", max=" + maxSize;
}

std::vector<Variant> MakeVariants(const RunOptions &opts)
{
   std::vector<Variant> variants;
   for (const auto &request : ParseRequests(opts.fOnlyVariant)) {
      const int level = ResolveLevel(*request.fSpec, request.fLevel, opts, request.fSpec->fFamily);
      Variant variant;
      variant.fFamily = request.fSpec->fFamily;
      variant.fLevel = level;
      if (request.fSpec->fCompressed) {
         variant.fLabel = variant.fFamily + std::to_string(level);
         variant.fSuffix = std::string(request.fSpec->fSuffix) + std::to_string(level);
         variant.fOptions.SetCompression(request.fSpec->fAlgorithm, level);
      } else {
         variant.fLabel = variant.fFamily;
         variant.fSuffix = request.fSpec->fSuffix;
         variant.fOptions.SetCompression(0);
      }
      ApplyPageSizeOptions(variant.fOptions, opts);
      variants.push_back(std::move(variant));
   }
   return variants;
}

std::string FindFirstNTupleName(const std::string &fileName)
{
   auto file = std::unique_ptr<TFile>(TFile::Open(fileName.c_str()));
   if (!file || file->IsZombie())
      throw std::runtime_error("cannot open input file: " + fileName);
   TIter next(file->GetListOfKeys());
   while (auto key = static_cast<TKey *>(next())) {
      if (std::strcmp(key->GetClassName(), "ROOT::RNTuple") == 0)
         return key->GetName();
   }
   throw std::runtime_error("no RNTuple found in " + fileName);
}

RunOptions ParseArgs(int argc, char **argv)
{
   RunOptions opts;
   std::vector<std::string> positional;
   for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "-h" || arg == "--help") {
         std::cout << kUsage;
         std::exit(0);
      }
      if (arg == "--no-out") {
         opts.fNoOut = true;
         continue;
      }
      if (arg == "--quiet") {
         opts.fQuiet = true;
         continue;
      }
      if (arg == "--bench") {
         opts.fBench = true;
         continue;
      }
      if (arg == "--bench-only") {
         opts.fBenchOnly = true;
         continue;
      }
      if (const auto value = OptionValue(arg, "--page-size", i, argc, argv); !value.empty()) {
         opts.fPageSize = static_cast<std::size_t>(ParseU64(value, "--page-size"));
         continue;
      }
      if (const auto value = OptionValue(arg, "--initial-page-size", i, argc, argv); !value.empty()) {
         opts.fInitialPageSize = static_cast<std::size_t>(ParseU64(value, "--initial-page-size"));
         continue;
      }
      if (const auto value = OptionValue(arg, "--level", i, argc, argv); !value.empty()) {
         opts.fLevel = ParseInt(value, "--level");
         opts.fLevelExplicit = true;
         continue;
      }
      if (const auto value = OptionValue(arg, "--only-variant", i, argc, argv); !value.empty()) {
         opts.fOnlyVariant = value;
         continue;
      }
      if (const auto value = OptionValue(arg, "--jobs", i, argc, argv); !value.empty()) {
         opts.fJobs = ParseInt(value, "--jobs");
         continue;
      }
      if (const auto value = OptionValue(arg, "--worker-result", i, argc, argv); !value.empty()) {
         opts.fWorkerResult = value;
         continue;
      }
      if (const auto value = OptionValue(arg, "--bench-threads", i, argc, argv); !value.empty()) {
         opts.fBenchThreads = ParseInt(value, "--bench-threads");
         continue;
      }
      if (const auto value = OptionValue(arg, "--bench-iters", i, argc, argv); !value.empty()) {
         opts.fBenchIters = ParseInt(value, "--bench-iters");
         continue;
      }
      if (const auto value = OptionValue(arg, "--bench-entries", i, argc, argv); !value.empty()) {
         opts.fBenchEntries = ParseU64(value, "--bench-entries");
         continue;
      }
      if (const auto value = OptionValue(arg, "--bench-percent", i, argc, argv); !value.empty()) {
         opts.fBenchPercent = ParseDouble(value, "--bench-percent");
         continue;
      }
      if (!arg.empty() && arg[0] == '-')
         throw std::runtime_error("unknown option: " + arg);
      positional.push_back(arg);
   }

   if (opts.fBenchOnly) {
      if (positional.empty())
         throw std::runtime_error("--bench-only requires at least one RNTuple file");
      opts.fPositionalArgs = std::move(positional);
   } else {
      if (positional.empty())
         throw std::runtime_error("missing input file (see --help)");
      if (positional.size() > 3)
         throw std::runtime_error("too many positional arguments (see --help)");
      opts.fInputFile = positional[0];
      if (positional.size() >= 2)
         opts.fNTupleName = positional[1];
      else
         opts.fNTupleName = FindFirstNTupleName(opts.fInputFile);
      if (positional.size() == 3)
         opts.fOutputBase = positional[2];
      else
         opts.fOutputBase = BasenameNoExt(opts.fInputFile) + "_recode";
   }

   if (opts.fPageSize && *opts.fPageSize == 0)
      throw std::runtime_error("--page-size must be > 0");
   if (opts.fInitialPageSize && *opts.fInitialPageSize == 0)
      throw std::runtime_error("--initial-page-size must be > 0");
   if (opts.fPageSize && opts.fInitialPageSize && *opts.fInitialPageSize > *opts.fPageSize)
      throw std::runtime_error("--initial-page-size must not exceed --page-size");
   if (opts.fJobs < 0)
      throw std::runtime_error("--jobs must be >= 0");
   if (opts.fBenchThreads < 0)
      throw std::runtime_error("--bench-threads must be >= 0");
   if (opts.fBenchIters < 1)
      throw std::runtime_error("--bench-iters must be >= 1");
   if (!(opts.fBenchPercent > 0.0 && opts.fBenchPercent <= 100.0))
      throw std::runtime_error("--bench-percent must be in (0, 100]");
   if (!opts.fBenchOnly && !opts.fWorkerResult.empty() && opts.fOnlyVariant.empty())
      throw std::runtime_error("--worker-result requires --only-variant");
   return opts;
}

void WriteWorkerResult(const std::string &path, const VariantResult &result)
{
   std::ofstream out(path);
   if (!out)
      throw std::runtime_error("cannot write worker result: " + path);
   out << "label=" << result.fLabel << '\n';
   out << "output_bytes=" << result.fOutputBytes << '\n';
   out << "elapsed_sec=" << std::setprecision(std::numeric_limits<double>::max_digits10) << result.fElapsedSec << '\n';
}

VariantResult ReadWorkerResult(const std::string &path)
{
   std::ifstream in(path);
   if (!in)
      throw std::runtime_error("cannot read worker result: " + path);
   VariantResult result;
   std::string line;
   while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos)
         continue;
      const std::string key = line.substr(0, eq);
      const std::string value = line.substr(eq + 1);
      if (key == "label")
         result.fLabel = value;
      else if (key == "output_bytes")
         result.fOutputBytes = std::stoull(value);
      else if (key == "elapsed_sec")
         result.fElapsedSec = std::stod(value);
   }
   if (result.fLabel.empty())
      throw std::runtime_error("worker result is missing a label: " + path);
   return result;
}

VariantResult ReencodeVariant(const RunOptions &opts, const Variant &variant, bool quiet)
{
   const std::string outputFile = ResolveVariantOutputPath(opts, variant.fSuffix);
   RemoveIfExists(outputFile);
   if (!quiet) {
      if (opts.fNoOut)
         std::cout << "Converting " << variant.fLabel << " (--no-out)\n";
      else
         std::cout << "Writing " << variant.fLabel << " -> " << outputFile << '\n';
   }

   const auto t0 = std::chrono::steady_clock::now();
   auto source = RPageSource::Create(opts.fNTupleName, opts.fInputFile, RNTupleReadOptions{});
   auto destination = std::make_unique<RPageSinkFile>(opts.fNTupleName, outputFile, variant.fOptions);
   std::vector<RPageSource *> sources{source.get()};
   RNTupleMergeOptions mergeOpts;
   mergeOpts.fCompressionSettings = variant.fOptions.GetCompression();
   RNTupleMerger merger(std::move(destination));
   merger.Merge(sources, mergeOpts).ThrowOnError();
   const auto t1 = std::chrono::steady_clock::now();

   VariantResult result;
   result.fLabel = variant.fLabel;
   result.fElapsedSec = std::chrono::duration<double>(t1 - t0).count();
   if (const auto size = FileSizeBytes(outputFile))
      result.fOutputBytes = *size;
   if (!quiet)
      std::cout << "  done\n";
   if (opts.fNoOut)
      RemoveIfExists(outputFile);
   return result;
}

std::uint64_t FindNativeZstdBytes(const std::vector<VariantResult> &results)
{
   for (const auto &row : results) {
      if (row.fLabel.rfind("native_zstd", 0) == 0)
         return row.fOutputBytes;
   }
   return 0;
}

std::string FormatGain(const VariantResult &row, std::uint64_t nativeZstdBytes)
{
   if (row.fLabel.rfind("native_zstd", 0) == 0)
      return "0.0%";
   if (nativeZstdBytes == 0)
      return "n/a";
   const double gain = 100.0 * (static_cast<double>(nativeZstdBytes) - static_cast<double>(row.fOutputBytes)) /
                       static_cast<double>(nativeZstdBytes);
   return FormatPct(gain);
}

void PrintSummaryTable(const RunOptions &opts, std::uint64_t inputBytes, const std::vector<VariantResult> &results)
{
   std::cout << "\n=== rntuple2rntuple summary ===\n";
   std::cout << "Input:  " << opts.fInputFile << " [" << opts.fNTupleName << "]\n";
   std::cout << "Input size: " << FormatBytes(inputBytes) << '\n';
   if (opts.fNoOut)
      std::cout << "Output: (none, --no-out)\n";
   else
      std::cout << "Output base: " << opts.fOutputBase << '\n';
   std::cout << "Page size: " << FormatPageSizeSettings(opts) << '\n';
   if (opts.fLevelExplicit)
      std::cout << "Level: " << opts.fLevel << "\n\n";
   else
      std::cout << "Level: per-algorithm defaults\n\n";

   const std::uint64_t nativeZstdBytes = FindNativeZstdBytes(results);
   constexpr int kLabelWidth = 16;
   std::cout << std::left << std::setw(kLabelWidth) << "Format" << std::right << std::setw(12) << "Output"
             << std::setw(10) << "Ratio" << std::setw(10) << "Gain" << std::setw(10) << "Time" << '\n';
   std::cout << std::string(kLabelWidth + 12 + 10 + 10 + 10, '-') << '\n';
   for (const auto &row : results) {
      std::cout << std::left << std::setw(kLabelWidth) << row.fLabel << std::right << std::setw(12)
                << FormatBytes(row.fOutputBytes) << std::setw(10) << FormatRatio(row.fOutputBytes, inputBytes)
                << std::setw(10) << FormatGain(row, nativeZstdBytes) << std::setw(10) << FormatSeconds(row.fElapsedSec)
                << '\n';
   }
   std::cout << "\nRatio = RNTuple output size / input file size (lower is smaller).\n";
   std::cout << "Gain  = space saved vs native_zstd (positive = smaller file).\n";
}

std::vector<VariantResult> RunSequential(const RunOptions &opts)
{
   std::vector<VariantResult> results;
   for (const auto &variant : MakeVariants(opts))
      results.push_back(ReencodeVariant(opts, variant, opts.fQuiet));
   return results;
}

#if !defined(_WIN32)
void AppendOpt(std::vector<std::string> &args, const char *flag, const std::string &value)
{
   args.emplace_back(flag);
   args.push_back(value);
}

std::vector<std::string>
BuildWorkerArgv(const char *executable, const RunOptions &opts, const Variant &variant, const std::string &resultPath)
{
   std::vector<std::string> args;
   args.emplace_back(executable);
   AppendOpt(args, "--only-variant", variant.fLabel);
   AppendOpt(args, "--worker-result", resultPath);
   args.emplace_back("--quiet");
   if (opts.fLevelExplicit)
      AppendOpt(args, "--level", std::to_string(opts.fLevel));
   if (opts.fPageSize)
      AppendOpt(args, "--page-size", std::to_string(*opts.fPageSize));
   if (opts.fInitialPageSize)
      AppendOpt(args, "--initial-page-size", std::to_string(*opts.fInitialPageSize));
   if (opts.fNoOut)
      args.emplace_back("--no-out");
   args.push_back(opts.fInputFile);
   args.push_back(opts.fNTupleName);
   args.push_back(opts.fOutputBase);
   return args;
}

[[noreturn]] void ExecWorker(const std::vector<std::string> &args)
{
   std::vector<char *> argv;
   argv.reserve(args.size() + 1);
   for (const auto &arg : args)
      argv.push_back(const_cast<char *>(arg.c_str()));
   argv.push_back(nullptr);
   execvp(argv[0], argv.data());
   std::cerr << "rntuple2rntuple: failed to exec worker: " << args[0] << " (" << std::strerror(errno) << ")\n";
   _exit(127);
}

unsigned ResolveJobCount(int jobs, std::size_t numVariants)
{
   if (jobs == 0) {
      unsigned hw = std::thread::hardware_concurrency();
      if (hw == 0)
         hw = 1;
      return static_cast<unsigned>(std::min<std::size_t>(hw, numVariants));
   }
   return static_cast<unsigned>(std::min<std::size_t>(static_cast<std::size_t>(jobs), numVariants));
}

std::vector<VariantResult> RunParallel(const RunOptions &opts, const char *executable)
{
   const auto variants = MakeVariants(opts);
   const unsigned jobCount = ResolveJobCount(opts.fJobs, variants.size());
   if (jobCount <= 1) {
      ROOT::EnableImplicitMT();
      return RunSequential(opts);
   }

   std::cout << "Running " << variants.size() << " variants with up to " << jobCount << " parallel jobs\n";
   const std::string resultDir =
      std::string("/tmp/rntuple2rntuple-") + std::to_string(static_cast<long long>(getpid()));
   gSystem->mkdir(resultDir.c_str(), true);

   struct RunningJob {
      pid_t fPid = -1;
      std::string fResultPath;
      std::string fLabel;
   };

   std::vector<RunningJob> running;
   std::vector<VariantResult> results;
   results.reserve(variants.size());
   std::size_t nextVariant = 0;
   int failedWorkers = 0;

   auto reapOne = [&]() {
      int status = 0;
      const pid_t done = wait(&status);
      if (done <= 0)
         throw std::runtime_error("wait() failed while collecting parallel workers");
      auto it =
         std::find_if(running.begin(), running.end(), [done](const RunningJob &job) { return job.fPid == done; });
      if (it == running.end())
         throw std::runtime_error("unexpected worker pid " + std::to_string(done));
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
         std::cerr << "Worker failed for variant " << it->fLabel << '\n';
         ++failedWorkers;
      } else {
         results.push_back(ReadWorkerResult(it->fResultPath));
      }
      gSystem->Unlink(it->fResultPath.c_str());
      running.erase(it);
   };

   auto launchOne = [&](const Variant &variant) {
      RunningJob job;
      job.fLabel = variant.fLabel;
      job.fResultPath = resultDir + "/" + variant.fLabel + ".result";
      RemoveIfExists(job.fResultPath);
      const auto workerArgs = BuildWorkerArgv(executable, opts, variant, job.fResultPath);
      std::cout.flush();
      std::cerr.flush();
      const pid_t pid = fork();
      if (pid < 0)
         throw std::runtime_error(std::string("fork() failed for variant ") + variant.fLabel);
      if (pid == 0)
         ExecWorker(workerArgs);
      job.fPid = pid;
      running.push_back(std::move(job));
   };

   while (nextVariant < variants.size() || !running.empty()) {
      while (running.size() < jobCount && nextVariant < variants.size())
         launchOne(variants[nextVariant++]);
      if (!running.empty())
         reapOne();
   }
   gSystem->Unlink(resultDir.c_str());
   if (failedWorkers > 0)
      throw std::runtime_error(std::to_string(failedWorkers) + " parallel worker(s) failed");

   std::sort(results.begin(), results.end(), [&variants](const VariantResult &a, const VariantResult &b) {
      auto rank = [&variants](const std::string &label) {
         for (std::size_t i = 0; i < variants.size(); ++i) {
            if (variants[i].fLabel == label)
               return i;
         }
         return variants.size();
      };
      return rank(a.fLabel) < rank(b.fLabel);
   });
   return results;
}
#endif

int RunWorkerMode(const RunOptions &opts)
{
   const auto variants = MakeVariants(opts);
   if (variants.size() != 1)
      throw std::runtime_error("--worker-result requires exactly one variant");
   ROOT::EnableImplicitMT();
   const auto result = ReencodeVariant(opts, variants[0], true);
   WriteWorkerResult(opts.fWorkerResult, result);
   return 0;
}

void RunBench(const RunOptions &opts, const std::vector<VariantResult> &results)
{
   const auto variants = MakeVariants(opts);
   std::vector<RNTupleBench::BenchmarkResult> benchResults;
   for (const auto &row : results) {
      const auto it = std::find_if(variants.begin(), variants.end(),
                                   [&row](const Variant &variant) { return variant.fLabel == row.fLabel; });
      if (it == variants.end())
         continue;
      const std::string path = opts.fOutputBase + it->fSuffix + ".root";
      benchResults.push_back(RNTupleBench::RunBenchmarkForFile(row.fLabel, path, opts.fNTupleName, opts.fBenchThreads,
                                                               opts.fBenchIters, opts.fBenchEntries,
                                                               opts.fBenchPercent));
   }
   RNTupleBench::PrintBenchmarkSummary(benchResults);
}

int RunBenchOnly(const RunOptions &opts)
{
   std::vector<std::pair<std::string, std::string>> files;
   std::string ntupleName;
   if (opts.fPositionalArgs.size() == 2 && opts.fPositionalArgs[1].find(".root") == std::string::npos) {
      files.emplace_back(RNTupleBench::Basename(opts.fPositionalArgs[0]), opts.fPositionalArgs[0]);
      ntupleName = opts.fPositionalArgs[1];
   } else {
      for (const auto &path : opts.fPositionalArgs)
         files.emplace_back(RNTupleBench::Basename(path), path);
   }

   std::vector<RNTupleBench::BenchmarkResult> benchResults;
   for (const auto &[label, path] : files) {
      benchResults.push_back(RNTupleBench::RunBenchmarkForFile(
         label, path, ntupleName, opts.fBenchThreads, opts.fBenchIters, opts.fBenchEntries, opts.fBenchPercent));
   }
   RNTupleBench::PrintBenchmarkSummary(benchResults);
   return 0;
}

int RunMain(const RunOptions &opts, const char *executable)
{
   if (opts.fBenchOnly)
      return RunBenchOnly(opts);

   if (!opts.fWorkerResult.empty())
      return RunWorkerMode(opts);

   const auto inputSize = FileSizeBytes(opts.fInputFile);
   if (!inputSize)
      throw std::runtime_error("cannot stat input file: " + opts.fInputFile);

   if (!opts.fQuiet) {
      std::cout << "Input: " << opts.fInputFile << " [" << opts.fNTupleName << "]\n";
      std::cout << "Page size: " << FormatPageSizeSettings(opts) << '\n';
   }

   std::vector<VariantResult> results;
#if defined(_WIN32)
   (void)executable;
   if (opts.fJobs != 1 && !opts.fQuiet)
      std::cerr << "rntuple2rntuple: parallel mode is not supported on Windows; running sequentially\n";
   ROOT::EnableImplicitMT();
   results = RunSequential(opts);
#else
   if (opts.fJobs == 1) {
      ROOT::EnableImplicitMT();
      results = RunSequential(opts);
   } else {
      results = RunParallel(opts, executable);
   }
#endif

   if (!opts.fQuiet)
      PrintSummaryTable(opts, *inputSize, results);
   if (opts.fBench && !opts.fNoOut)
      RunBench(opts, results);
   return 0;
}

int RunProgram(int argc, char **argv)
{
   try {
      const auto opts = ParseArgs(argc, argv);
      return RunMain(opts, argv[0]);
   } catch (const std::exception &e) {
      std::cerr << "rntuple2rntuple: " << e.what() << '\n';
      std::cerr << kUsage;
      return 1;
   }
}

} // namespace

int main(int argc, char **argv)
{
   return RunProgram(argc, argv);
}
