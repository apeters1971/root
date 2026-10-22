/// \file rntupletryzip.cxx
/// \brief Recompress RNTuple pages in memory with ROOT's native compressors.

/*************************************************************************
 * Copyright (C) 1995-2026, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RColumnElementBase.hxx>
#include <ROOT/RNTupleInspector.hxx>
#include <ROOT/RNTupleZip.hxx>

#include <Compression.h>
#include <TError.h>
#include <TFile.h>
#include <TKey.h>
#include <TSystem.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using ROOT::Experimental::RNTupleInspector;
using ROOT::Internal::GenerateColumnElement;
using ROOT::Internal::RColumnElementBase;
using ROOT::Internal::RNTupleCompressor;
using ROOT::Internal::RNTupleDecompressor;
using EAlgorithm = ROOT::RCompressionSetting::EAlgorithm;
using ELevel = ROOT::RCompressionSetting::ELevel;

constexpr const char *kUsage = R"USAGE(Usage: rntupletryzip [options] input.root [ntupleName]

Unzip RNTuple pages and recompress them in memory. No output file is written
and no C++ dictionaries are required.

By default, split/delta/zigzag pages are unpacked and packed again the way a
native RNTuple write would, then compressed. Pass --keep-column-encoding to
recompress the bytes already stored on disk.

Options:
  --top N                 Recode the N largest on-disk columns (default: 20).
  --all-columns           Recode every physical column.
  --pages N               Pages per column (default: 4, spread across clusters).
                          0 = every page of the selected columns.
  --level N               Level used when a variant name has none. zstd is 1..19,
                          other algorithms are 1..9.
  --only-variant LIST     Comma-separated codecs (default: native_zstd,native_lzma).
                          A trailing level selects that level (native_lzma4).
                          Families: native_zstd, native_lzma, native_zlib,
                          native_lz4, uncompressed.
  --keep-column-encoding  Compress the on-disk column encoding as stored.
  --threads N             Recode threads (default: all hardware cores).
  --list-variants         Print codec families and exit.
  --verify                Unzip every recompressed page and compare it.
  --csv                   Also print a CSV of per-column scaled sizes.
  --verbose               Keep TClass dictionary warnings.
  -h, --help              Show this help.

If ntupleName is omitted, Events is used when present, otherwise the RNTuple
with the largest on-disk payload.
)USAGE";

struct AlgoSpec {
   const char *fFamily;
   EAlgorithm::EValues fAlgorithm;
   int fDefaultLevel;
   bool fCompressed;
};

constexpr AlgoSpec kAlgos[] = {
   {"native_zstd", EAlgorithm::kZSTD, ELevel::kDefaultZSTD, true},
   {"native_lzma", EAlgorithm::kLZMA, ELevel::kDefaultLZMA, true},
   {"native_zlib", EAlgorithm::kZLIB, ELevel::kDefaultZLIB, true},
   {"native_lz4", EAlgorithm::kLZ4, ELevel::kDefaultLZ4, true},
   {"uncompressed", EAlgorithm::kZSTD, 0, false},
};

struct RunOptions {
   std::string fInputFile;
   std::string fNTupleName;
   int fTop = 20;
   int fPages = 4;
   int fLevel = 0;
   bool fLevelExplicit = false;
   bool fAllColumns = false;
   bool fCsv = false;
   bool fVerbose = false;
   bool fVerify = false;
   bool fListVariants = false;
   bool fKeepColumnEncoding = false;
   int fThreads = 0;
   std::vector<std::string> fOnly;
};

struct NTupleInfo {
   std::string fName;
   ROOT::NTupleSize_t fNEntries = 0;
   std::uint64_t fComp = 0;
   std::uint64_t fUncomp = 0;
   std::size_t fNColumns = 0;
};

struct ColRow {
   ROOT::DescriptorId_t fId = 0;
   std::string fField;
   std::string fTypeName;
   ROOT::ENTupleColumnType fType = ROOT::ENTupleColumnType::kUnknown;
   std::uint16_t fBits = 0;
   std::uint64_t fNPages = 0;
   std::uint64_t fComp = 0;
   std::uint64_t fUncomp = 0;
};

struct PageLoc {
   std::uint64_t fOffset = 0;
   std::uint64_t fSealed = 0;
   std::size_t fPacked = 0;
   std::uint32_t fNElements = 0;
};

struct ZipVariant {
   std::string fLabel;
   int fCompression = 0;
};

struct InflatedPage {
   std::vector<unsigned char> fRaw;
   std::vector<unsigned char> fPackedNative;
   std::uint64_t fSealed = 0;
};

class IgnoreDictWarnings {
public:
   IgnoreDictWarnings() : fOld(gErrorIgnoreLevel) { gErrorIgnoreLevel = kError; }
   ~IgnoreDictWarnings() { gErrorIgnoreLevel = fOld; }
   IgnoreDictWarnings(const IgnoreDictWarnings &) = delete;
   IgnoreDictWarnings &operator=(const IgnoreDictWarnings &) = delete;

private:
   Int_t fOld = 0;
};

class PageReader {
public:
   explicit PageReader(const std::string &path)
   {
#if !defined(_WIN32)
      fFd = ::open(path.c_str(), O_RDONLY);
#endif
      if (fFd < 0) {
         fFile.reset(TFile::Open(path.c_str(), "READ"));
         if (!fFile || fFile->IsZombie())
            throw std::runtime_error("cannot open " + path + " for page reads");
      }
   }

   ~PageReader()
   {
#if !defined(_WIN32)
      if (fFd >= 0)
         ::close(fFd);
#endif
   }

   bool Read(std::uint64_t offset, void *buf, std::size_t n)
   {
#if !defined(_WIN32)
      if (fFd >= 0)
         return ::pread(fFd, buf, n, static_cast<off_t>(offset)) == static_cast<ssize_t>(n);
#endif
      if (!fFile)
         return false;
      fFile->Seek(static_cast<Long64_t>(offset));
      return !fFile->ReadBuffer(static_cast<char *>(buf), static_cast<int>(n));
   }

private:
   int fFd = -1;
   std::unique_ptr<TFile> fFile;
};

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

std::vector<std::string> SplitComma(const std::string &text)
{
   std::vector<std::string> out;
   std::stringstream ss(text);
   std::string item;
   while (std::getline(ss, item, ',')) {
      const auto begin = item.find_first_not_of(" \t");
      if (begin == std::string::npos)
         continue;
      const auto end = item.find_last_not_of(" \t");
      out.push_back(item.substr(begin, end - begin + 1));
   }
   return out;
}

std::string FormatBytes(std::uint64_t n)
{
   std::ostringstream os;
   os << std::fixed << std::setprecision(2);
   if (n >= (1ull << 30))
      os << (n / 1073741824.0) << " GiB";
   else if (n >= (1ull << 20))
      os << (n / 1048576.0) << " MiB";
   else if (n >= 1024)
      os << (n / 1024.0) << " KiB";
   else
      os << n << " B";
   return os.str();
}

std::string ShortField(const std::string &name)
{
   if (name.size() <= 40)
      return name;
   return "..." + name.substr(name.size() - 37);
}

const AlgoSpec *FindAlgo(const std::string &family)
{
   for (const auto &algo : kAlgos) {
      if (family == algo.fFamily)
         return &algo;
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

void CheckLevel(const AlgoSpec &algo, int level)
{
   if (!algo.fCompressed)
      throw std::runtime_error(std::string(algo.fFamily) + " has no compression level");
   if (level < 1)
      throw std::runtime_error("compression level must be >= 1");
   if (algo.fAlgorithm == EAlgorithm::kZSTD) {
      if (level > 19)
         throw std::runtime_error("zstd level must be between 1 and 19");
   } else if (level > 9) {
      throw std::runtime_error(std::string(algo.fFamily) + " level must be between 1 and 9");
   }
}

ZipVariant MakeVariant(const AlgoSpec &algo, int level)
{
   ZipVariant variant;
   if (!algo.fCompressed) {
      variant.fLabel = algo.fFamily;
      variant.fCompression = 0;
      return variant;
   }
   CheckLevel(algo, level);
   variant.fLabel = std::string(algo.fFamily) + std::to_string(level);
   variant.fCompression = ROOT::CompressionSettings(algo.fAlgorithm, level);
   return variant;
}

std::vector<ZipVariant> SelectVariants(const RunOptions &opts)
{
   const auto requested = opts.fOnly.empty() ? std::vector<std::string>{"native_zstd", "native_lzma"} : opts.fOnly;
   std::vector<ZipVariant> out;
   for (const auto &name : requested) {
      std::string family;
      const auto suffixLevel = SplitLevelSuffix(name, family);
      const AlgoSpec *algo = FindAlgo(family);
      if (!algo) {
         std::ostringstream os;
         os << "unknown variant '" << name << "'; known:";
         for (const auto &item : kAlgos)
            os << ' ' << item.fFamily;
         throw std::runtime_error(os.str());
      }
      if (suffixLevel && opts.fLevelExplicit && *suffixLevel != opts.fLevel) {
         throw std::runtime_error(name + " asks for level " + std::to_string(*suffixLevel) + " but --level is " +
                                  std::to_string(opts.fLevel));
      }
      int level = algo->fDefaultLevel;
      if (opts.fLevelExplicit)
         level = opts.fLevel;
      if (suffixLevel)
         level = *suffixLevel;
      out.push_back(MakeVariant(*algo, level));
   }
   if (out.empty())
      throw std::runtime_error("no variants selected");
   return out;
}

void PrintVariantList()
{
   std::cout << "Variants (append a level, e.g. native_zstd9):\n";
   for (const auto &algo : kAlgos) {
      std::cout << "  " << algo.fFamily;
      if (algo.fCompressed)
         std::cout << "  default level " << algo.fDefaultLevel;
      std::cout << '\n';
   }
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
      if (arg == "--list-variants") {
         opts.fListVariants = true;
         continue;
      }
      if (arg == "--csv") {
         opts.fCsv = true;
         continue;
      }
      if (arg == "--verbose" || arg == "-v") {
         opts.fVerbose = true;
         continue;
      }
      if (arg == "--verify") {
         opts.fVerify = true;
         continue;
      }
      if (arg == "--all-columns") {
         opts.fAllColumns = true;
         continue;
      }
      if (arg == "--keep-column-encoding") {
         opts.fKeepColumnEncoding = true;
         continue;
      }
      if (const auto value = OptionValue(arg, "--top", i, argc, argv); !value.empty()) {
         opts.fTop = ParseInt(value, "--top");
         continue;
      }
      if (const auto value = OptionValue(arg, "--pages", i, argc, argv); !value.empty()) {
         opts.fPages = ParseInt(value, "--pages");
         continue;
      }
      if (const auto value = OptionValue(arg, "--level", i, argc, argv); !value.empty()) {
         opts.fLevel = ParseInt(value, "--level");
         opts.fLevelExplicit = true;
         continue;
      }
      if (const auto value = OptionValue(arg, "--only-variant", i, argc, argv); !value.empty()) {
         opts.fOnly = SplitComma(value);
         continue;
      }
      if (const auto value = OptionValue(arg, "--threads", i, argc, argv); !value.empty()) {
         opts.fThreads = ParseInt(value, "--threads");
         continue;
      }
      if (!arg.empty() && arg[0] == '-')
         throw std::runtime_error("unknown option: " + arg);
      positional.push_back(arg);
   }
   if (opts.fListVariants)
      return opts;
   if (positional.empty())
      throw std::runtime_error("missing input file (see --help)");
   if (positional.size() > 2)
      throw std::runtime_error("too many positional arguments (see --help)");
   opts.fInputFile = positional[0];
   if (positional.size() == 2)
      opts.fNTupleName = positional[1];
   if (opts.fTop < 1)
      throw std::runtime_error("--top must be >= 1");
   if (opts.fPages < 0)
      throw std::runtime_error("--pages must be >= 0");
   if (opts.fThreads < 0)
      throw std::runtime_error("--threads must be >= 0");
   return opts;
}

std::vector<std::string> ListRNTupleNames(const std::string &path)
{
   auto file = std::unique_ptr<TFile>(TFile::Open(path.c_str(), "READ"));
   if (!file || file->IsZombie())
      throw std::runtime_error("cannot open " + path);
   std::vector<std::string> names;
   TIter next(file->GetListOfKeys());
   while (auto key = static_cast<TKey *>(next())) {
      if (std::strcmp(key->GetClassName(), "ROOT::RNTuple") != 0)
         continue;
      const std::string name = key->GetName();
      if (std::find(names.begin(), names.end(), name) == names.end())
         names.push_back(name);
   }
   return names;
}

NTupleInfo DescribeRNTuple(const std::string &path, const std::string &name)
{
   auto inspector = RNTupleInspector::Create(name, path);
   const auto &desc = inspector->GetDescriptor();
   NTupleInfo info;
   info.fName = name;
   info.fNEntries = desc.GetNEntries();
   info.fComp = inspector->GetCompressedSize();
   info.fUncomp = inspector->GetUncompressedSize();
   info.fNColumns = desc.GetNPhysicalColumns();
   return info;
}

std::vector<NTupleInfo> DescribeAllRNTuples(const std::string &path)
{
   const auto names = ListRNTupleNames(path);
   if (names.empty())
      throw std::runtime_error("no RNTuple in " + path);
   std::vector<NTupleInfo> infos;
   infos.reserve(names.size());
   for (const auto &name : names)
      infos.push_back(DescribeRNTuple(path, name));
   return infos;
}

std::string SelectRNTuple(const std::vector<NTupleInfo> &infos, const std::string &requested)
{
   if (!requested.empty()) {
      for (const auto &info : infos) {
         if (info.fName == requested)
            return requested;
      }
      std::ostringstream os;
      os << "unknown RNTuple '" << requested << "'; available:";
      for (const auto &info : infos)
         os << ' ' << info.fName;
      throw std::runtime_error(os.str());
   }
   for (const auto &info : infos) {
      if (info.fName == "Events")
         return info.fName;
   }
   const auto best = std::max_element(infos.begin(), infos.end(),
                                      [](const NTupleInfo &a, const NTupleInfo &b) { return a.fComp < b.fComp; });
   return best->fName;
}

void PrintRNTupleCatalog(const std::vector<NTupleInfo> &infos, const std::string &selected)
{
   std::cout << "RNTuples in file:\n";
   for (const auto &info : infos) {
      const char *mark = (info.fName == selected) ? "  * " : "    ";
      std::cout << mark << std::left << std::setw(22) << info.fName << std::right << std::setw(10) << info.fNEntries
                << " entries  " << std::setw(12) << FormatBytes(info.fComp) << " payload\n";
   }
   if (infos.size() > 1 && !selected.empty())
      std::cout << "  (* selected)\n";
   std::cout << '\n';
}

bool IsSplitColumnType(ROOT::ENTupleColumnType type)
{
   using T = ROOT::ENTupleColumnType;
   switch (type) {
   case T::kSplitIndex64:
   case T::kSplitIndex32:
   case T::kSplitReal64:
   case T::kSplitReal32:
   case T::kSplitInt64:
   case T::kSplitInt32:
   case T::kSplitInt16:
   case T::kSplitUInt64:
   case T::kSplitUInt32:
   case T::kSplitUInt16: return true;
   default: return false;
   }
}

/// On-disk type a native RNTuple write would use for this column.
ROOT::ENTupleColumnType NativeSplitType(ROOT::ENTupleColumnType type)
{
   using T = ROOT::ENTupleColumnType;
   switch (type) {
   case T::kReal32:
   case T::kSplitReal32: return T::kSplitReal32;
   case T::kReal64:
   case T::kSplitReal64: return T::kSplitReal64;
   case T::kInt16:
   case T::kSplitInt16: return T::kSplitInt16;
   case T::kInt32:
   case T::kSplitInt32: return T::kSplitInt32;
   case T::kInt64:
   case T::kSplitInt64: return T::kSplitInt64;
   case T::kUInt16:
   case T::kSplitUInt16: return T::kSplitUInt16;
   case T::kUInt32:
   case T::kSplitUInt32: return T::kSplitUInt32;
   case T::kUInt64:
   case T::kSplitUInt64: return T::kSplitUInt64;
   case T::kIndex32:
   case T::kSplitIndex32: return T::kSplitIndex32;
   case T::kIndex64:
   case T::kSplitIndex64: return T::kSplitIndex64;
   default: return type;
   }
}

std::unique_ptr<RColumnElementBase> MakeNativePacker(const RColumnElementBase &src)
{
   const auto nativeType = NativeSplitType(src.GetIdentifier().fOnDiskType);
   auto dst = GenerateColumnElement(src.GetIdentifier().fInMemoryType, nativeType);
   if (src.GetBitsOnStorage() != dst->GetBitsOnStorage()) {
      try {
         dst->SetBitsOnStorage(src.GetBitsOnStorage());
      } catch (const std::exception &) {
      }
   }
   if (const auto range = src.GetValueRange()) {
      try {
         dst->SetValueRange(range->first, range->second);
      } catch (const std::exception &) {
      }
   }
   return dst;
}

std::vector<PageLoc> CollectPages(const ROOT::RNTupleDescriptor &desc, const ColRow &col, int maxPages)
{
   std::vector<PageLoc> all;
   for (const auto &clusterDesc : desc.GetClusterIterable()) {
      if (!clusterDesc.ContainsColumn(col.fId))
         continue;
      if (clusterDesc.GetColumnRange(col.fId).IsSuppressed())
         continue;
      const auto &pageRange = clusterDesc.GetPageRange(col.fId);
      for (const auto &pageInfo : pageRange.GetPageInfos()) {
         const auto &loc = pageInfo.GetLocator();
         if (loc.GetType() == ROOT::RNTupleLocator::kTypePageZero)
            continue;
         const auto sealed = loc.GetNBytesOnStorage();
         const auto packed = (static_cast<std::size_t>(col.fBits) * pageInfo.GetNElements() + 7) / 8;
         if (sealed == 0 || packed == 0)
            continue;
         PageLoc page;
         page.fOffset = loc.GetPosition<std::uint64_t>();
         page.fSealed = sealed;
         page.fPacked = packed;
         page.fNElements = pageInfo.GetNElements();
         all.push_back(page);
      }
   }
   if (maxPages <= 0 || static_cast<int>(all.size()) <= maxPages)
      return all;
   std::vector<PageLoc> sample;
   sample.reserve(static_cast<std::size_t>(maxPages));
   for (int i = 0; i < maxPages; ++i) {
      const std::size_t idx = (static_cast<std::size_t>(i) * all.size()) / static_cast<std::size_t>(maxPages);
      sample.push_back(all[idx]);
   }
   return sample;
}

std::vector<InflatedPage> InflatePages(PageReader &reader, const std::vector<PageLoc> &pages,
                                       const RColumnElementBase &elem, bool unpackEncodings,
                                       const RColumnElementBase *nativeElem, int *nFail)
{
   std::vector<InflatedPage> out;
   out.reserve(pages.size());
   std::vector<unsigned char> sealed;
   std::vector<unsigned char> packed;
   std::vector<unsigned char> logical;
   const bool unpack = unpackEncodings && IsSplitColumnType(elem.GetIdentifier().fOnDiskType);
   for (const auto &page : pages) {
      if (sealed.size() < page.fSealed)
         sealed.resize(static_cast<std::size_t>(page.fSealed));
      if (!reader.Read(page.fOffset, sealed.data(), static_cast<std::size_t>(page.fSealed))) {
         ++(*nFail);
         continue;
      }
      InflatedPage inflated;
      inflated.fSealed = page.fSealed;
      try {
         packed.resize(page.fPacked);
         RNTupleDecompressor::Unzip(sealed.data(), static_cast<std::size_t>(page.fSealed), page.fPacked, packed.data());
         if (unpack) {
            inflated.fRaw.resize(elem.GetSize() * page.fNElements);
            elem.Unpack(inflated.fRaw.data(), packed.data(), page.fNElements);
         } else {
            inflated.fRaw = packed;
         }
         if (nativeElem) {
            const unsigned char *src = packed.data();
            if (unpack) {
               src = inflated.fRaw.data();
            } else if (!elem.IsMappable()) {
               logical.resize(elem.GetSize() * page.fNElements);
               elem.Unpack(logical.data(), packed.data(), page.fNElements);
               src = logical.data();
            }
            inflated.fPackedNative.resize(nativeElem->GetPackedSize(page.fNElements));
            nativeElem->Pack(inflated.fPackedNative.data(), src, page.fNElements);
         }
      } catch (const std::exception &) {
         ++(*nFail);
         continue;
      }
      out.push_back(std::move(inflated));
   }
   return out;
}

std::size_t ZipChecked(const void *from, std::size_t nbytes, int compression, void *to, bool verify)
{
   const auto zipped = RNTupleCompressor::Zip(from, nbytes, compression, to);
   if (!verify)
      return zipped;
   if (zipped == nbytes) {
      if (std::memcmp(from, to, nbytes) != 0)
         throw std::runtime_error("zip verify: uncompressed page buffer mismatch");
      return zipped;
   }
   std::vector<unsigned char> back(nbytes);
   RNTupleDecompressor::Unzip(to, zipped, nbytes, back.data());
   if (std::memcmp(from, back.data(), nbytes) != 0)
      throw std::runtime_error("zip verify: decompressed page buffer mismatch");
   return zipped;
}

unsigned ChooseThreads(int requested, std::size_t nJobs)
{
   unsigned hw = std::thread::hardware_concurrency();
   if (hw == 0)
      hw = 1;
   unsigned n = requested > 0 ? static_cast<unsigned>(requested) : hw;
   if (nJobs > 0 && n > nJobs)
      n = static_cast<unsigned>(nJobs);
   if (n < 1)
      n = 1;
   return n;
}

struct RecodeJob {
   std::size_t fVariant = 0;
   std::size_t fCol = 0;
   std::size_t fPage = 0;
};

void ZipRecodeJobs(const std::vector<ZipVariant> &variants, const std::vector<std::vector<InflatedPage>> &inflated,
                   const std::vector<RecodeJob> &jobs, std::vector<std::uint64_t> &jobZip,
                   std::atomic<std::uint64_t> *cpuNs, bool verify, unsigned nThreads)
{
   std::atomic<std::size_t> next{0};
   std::mutex errorMu;
   std::string error;
   auto worker = [&] {
      std::vector<unsigned char> zipped;
      try {
         for (;;) {
            const std::size_t j = next.fetch_add(1);
            if (j >= jobs.size())
               break;
            const auto &job = jobs[j];
            const auto &page = inflated[job.fCol][job.fPage];
            const auto &src = page.fPackedNative.empty() ? page.fRaw : page.fPackedNative;
            if (src.empty())
               continue;
            if (zipped.size() < src.size())
               zipped.resize(src.size());
            const auto t0 = std::chrono::steady_clock::now();
            jobZip[j] = ZipChecked(src.data(), src.size(), variants[job.fVariant].fCompression, zipped.data(), verify);
            if (cpuNs) {
               const auto ns =
                  std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
               cpuNs[job.fVariant].fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
            }
         }
      } catch (const std::exception &e) {
         std::lock_guard<std::mutex> lock(errorMu);
         if (error.empty())
            error = e.what();
      }
   };
   if (nThreads <= 1) {
      worker();
   } else {
      std::vector<std::thread> pool;
      pool.reserve(nThreads);
      for (unsigned t = 0; t < nThreads; ++t)
         pool.emplace_back(worker);
      for (auto &th : pool)
         th.join();
   }
   if (!error.empty())
      throw std::runtime_error(error);
}

int RunProgram(int argc, char **argv)
{
   try {
      auto opts = ParseArgs(argc, argv);
      if (opts.fListVariants) {
         PrintVariantList();
         return 0;
      }

      FileStat_t st{};
      if (gSystem->GetPathInfo(opts.fInputFile.c_str(), st) != 0)
         throw std::runtime_error("cannot stat " + opts.fInputFile);

      std::unique_ptr<IgnoreDictWarnings> quiet;
      if (!opts.fVerbose)
         quiet = std::make_unique<IgnoreDictWarnings>();

      const auto catalog = DescribeAllRNTuples(opts.fInputFile);
      opts.fNTupleName = SelectRNTuple(catalog, opts.fNTupleName);
      auto inspector = RNTupleInspector::Create(opts.fNTupleName, opts.fInputFile);
      const auto &desc = inspector->GetDescriptor();
      const auto variants = SelectVariants(opts);

      std::cout << "File:     " << opts.fInputFile << " (" << FormatBytes(static_cast<std::uint64_t>(st.fSize))
                << ")\n";
      PrintRNTupleCatalog(catalog, opts.fNTupleName);
      std::cout << "RNTuple:  " << opts.fNTupleName << '\n';
      std::cout << "Entries:  " << desc.GetNEntries() << "   columns: " << desc.GetNPhysicalColumns() << '\n';
      std::cout << "On disk:  " << inspector->GetCompressionSettingsAsString() << "   payload "
                << FormatBytes(inspector->GetCompressedSize()) << " / " << FormatBytes(inspector->GetUncompressedSize())
                << " uncompressed  (" << std::fixed << std::setprecision(2) << inspector->GetCompressionFactor()
                << "x)\n";
      std::cout << "Mode:     in-memory recode ("
                << (opts.fKeepColumnEncoding ? "on-disk encodings kept" : "native split/delta/zigzag reapplied")
                << ")\n\n";

      std::vector<ColRow> cols;
      for (const auto &colDesc : desc.GetColumnIterable()) {
         if (colDesc.IsAliasColumn())
            continue;
         const auto id = colDesc.GetPhysicalId();
         const auto &ci = inspector->GetColumnInspector(id);
         ColRow row;
         row.fId = id;
         row.fField = desc.GetQualifiedFieldName(colDesc.GetFieldId());
         if (colDesc.GetIndex() != 0)
            row.fField += "[" + std::to_string(colDesc.GetIndex()) + "]";
         row.fTypeName = RColumnElementBase::GetColumnTypeName(colDesc.GetType());
         row.fType = colDesc.GetType();
         row.fBits = colDesc.GetBitsOnStorage();
         row.fNPages = ci.GetNPages();
         row.fComp = ci.GetCompressedSize();
         row.fUncomp = ci.GetUncompressedSize();
         cols.push_back(std::move(row));
      }
      std::sort(cols.begin(), cols.end(), [](const ColRow &a, const ColRow &b) { return a.fComp > b.fComp; });
      const int nSel =
         opts.fAllColumns ? static_cast<int>(cols.size()) : std::min(opts.fTop, static_cast<int>(cols.size()));
      cols.resize(static_cast<std::size_t>(nSel));

      std::uint64_t selectedOrig = 0;
      for (const auto &col : cols)
         selectedOrig += col.fComp;
      const auto totalOrig = inspector->GetCompressedSize();
      const auto restOrig = totalOrig > selectedOrig ? totalOrig - selectedOrig : 0;

      std::cout << "Selected " << cols.size() << " column" << (cols.size() == 1 ? "" : "s") << " covering "
                << FormatBytes(selectedOrig) << " / " << FormatBytes(totalOrig) << " payload (" << std::setprecision(1)
                << (totalOrig ? 100.0 * selectedOrig / totalOrig : 0) << "%).\n";
      if (opts.fPages > 0)
         std::cout << "Sampling " << opts.fPages << " page(s) per column and scaling to that column's on-disk size.\n";
      else
         std::cout << "Recoding every page of the selected columns.\n";
      std::cout << "Untouched remainder stays at " << FormatBytes(restOrig) << " in the projected file size.\n\n";

      PageReader reader(opts.fInputFile);
      int nInflateFail = 0;
      std::vector<std::vector<InflatedPage>> inflated;
      inflated.reserve(cols.size());
      int nSamplePages = 0;
      for (const auto &col : cols) {
         auto pages = CollectPages(desc, col, opts.fPages);
         auto elem = RColumnElementBase::Generate(col.fType);
         elem->SetBitsOnStorage(col.fBits);
         std::unique_ptr<RColumnElementBase> nativeElem;
         if (!opts.fKeepColumnEncoding)
            nativeElem = MakeNativePacker(*elem);
         auto pagesInflated =
            InflatePages(reader, pages, *elem, !opts.fKeepColumnEncoding, nativeElem.get(), &nInflateFail);
         nSamplePages += static_cast<int>(pagesInflated.size());
         inflated.push_back(std::move(pagesInflated));
      }
      std::cout << "Pages queued: " << nSamplePages;
      if (nInflateFail)
         std::cout << "  (" << nInflateFail << " unzip failures)";
      std::cout << '\n';

      struct VariantRow {
         std::string fLabel;
         std::uint64_t fScaledSel = 0;
         std::uint64_t fProjected = 0;
         double fSec = 0;
         std::vector<std::uint64_t> fColScaled;
      };

      std::vector<RecodeJob> jobs;
      jobs.reserve(static_cast<std::size_t>(nSamplePages) * variants.size());
      std::vector<std::vector<std::vector<std::uint64_t>>> zipByVar(variants.size());
      for (std::size_t vi = 0; vi < variants.size(); ++vi) {
         zipByVar[vi].resize(cols.size());
         for (std::size_t i = 0; i < cols.size(); ++i) {
            zipByVar[vi][i].assign(inflated[i].size(), 0);
            for (std::size_t p = 0; p < inflated[i].size(); ++p)
               jobs.push_back(RecodeJob{vi, i, p});
         }
      }
      const unsigned nThreads = ChooseThreads(opts.fThreads, jobs.size());
      auto cpuNs = std::make_unique<std::atomic<std::uint64_t>[]>(variants.size());
      for (std::size_t vi = 0; vi < variants.size(); ++vi)
         cpuNs[vi].store(0);
      std::cout << "Recoding " << variants.size() << " codec" << (variants.size() == 1 ? "" : "s") << " x "
                << nSamplePages << " page" << (nSamplePages == 1 ? "" : "s") << " on " << nThreads << " thread"
                << (nThreads == 1 ? "" : "s") << " ..." << std::flush;
      std::vector<std::uint64_t> jobZip(jobs.size(), 0);
      const auto t0 = std::chrono::steady_clock::now();
      ZipRecodeJobs(variants, inflated, jobs, jobZip, cpuNs.get(), opts.fVerify, nThreads);
      const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::cout << ' ' << std::setprecision(1) << std::fixed << wall << " s wall\n";
      for (std::size_t j = 0; j < jobs.size(); ++j)
         zipByVar[jobs[j].fVariant][jobs[j].fCol][jobs[j].fPage] = jobZip[j];

      std::vector<VariantRow> rows;
      for (std::size_t vi = 0; vi < variants.size(); ++vi) {
         VariantRow row;
         row.fLabel = variants[vi].fLabel;
         row.fColScaled.resize(cols.size(), 0);
         for (std::size_t i = 0; i < cols.size(); ++i) {
            if (inflated[i].empty()) {
               row.fColScaled[i] = cols[i].fComp;
               row.fScaledSel += cols[i].fComp;
               continue;
            }
            std::uint64_t sampleNew = 0;
            std::uint64_t sampleOrig = 0;
            for (std::size_t p = 0; p < inflated[i].size(); ++p) {
               sampleNew += zipByVar[vi][i][p];
               sampleOrig += inflated[i][p].fSealed;
            }
            if (sampleOrig > 0) {
               const double scale = static_cast<double>(cols[i].fComp) / static_cast<double>(sampleOrig);
               row.fColScaled[i] = static_cast<std::uint64_t>(sampleNew * scale + 0.5);
            } else {
               row.fColScaled[i] = cols[i].fComp;
            }
            row.fScaledSel += row.fColScaled[i];
         }
         row.fProjected = row.fScaledSel + restOrig;
         row.fSec = static_cast<double>(cpuNs[vi].load()) / 1e9;
         std::cout << "  " << row.fLabel << " ... " << FormatBytes(row.fScaledSel) << " selected  ("
                   << std::setprecision(1) << std::fixed << row.fSec << " s CPU)\n";
         rows.push_back(std::move(row));
      }

      std::cout << "\n=== projected payload ===\n";
      constexpr int kNameW = 16;
      std::cout << std::left << std::setw(kNameW) << "codec" << std::right << std::setw(14) << "selected"
                << std::setw(10) << "vs disk" << std::setw(14) << "file" << std::setw(10) << "vs file" << std::setw(10)
                << "cpu" << '\n';
      auto printRow = [&](const std::string &label, std::uint64_t selected, std::uint64_t file, double sec,
                          bool timed) {
         const double vsSel = selectedOrig ? 100.0 * (static_cast<double>(selected) / selectedOrig - 1.0) : 0;
         const double vsFile = totalOrig ? 100.0 * (static_cast<double>(file) / totalOrig - 1.0) : 0;
         std::cout << std::left << std::setw(kNameW) << label << std::right << std::setw(14) << FormatBytes(selected)
                   << std::setw(9) << std::fixed << std::setprecision(1) << vsSel << "%" << std::setw(14)
                   << FormatBytes(file) << std::setw(9) << vsFile << "%";
         if (timed)
            std::cout << std::setw(10) << std::setprecision(1) << sec;
         else
            std::cout << std::setw(10) << "-";
         std::cout << '\n';
      };
      printRow("on-disk", selectedOrig, totalOrig, 0, false);
      for (const auto &row : rows)
         printRow(row.fLabel, row.fScaledSel, row.fProjected, row.fSec, true);

      std::cout << "\n=== selected columns (scaled) ===\n";
      std::cout << std::left << std::setw(42) << "field" << std::setw(16) << "type" << std::right << std::setw(12)
                << "on disk";
      for (const auto &row : rows)
         std::cout << std::setw(16) << row.fLabel;
      std::cout << '\n';
      for (std::size_t i = 0; i < cols.size(); ++i) {
         std::cout << std::left << std::setw(42) << ShortField(cols[i].fField) << std::setw(16) << cols[i].fTypeName
                   << std::right << std::setw(12) << FormatBytes(cols[i].fComp);
         for (const auto &row : rows)
            std::cout << std::setw(16) << FormatBytes(row.fColScaled[i]);
         std::cout << '\n';
      }

      if (opts.fCsv) {
         std::cout << "\nfield,type,on_disk";
         for (const auto &row : rows)
            std::cout << ',' << row.fLabel;
         std::cout << '\n';
         for (std::size_t i = 0; i < cols.size(); ++i) {
            std::cout << '"' << cols[i].fField << '"' << ',' << cols[i].fTypeName << ',' << cols[i].fComp;
            for (const auto &row : rows)
               std::cout << ',' << row.fColScaled[i];
            std::cout << '\n';
         }
      }
      return 0;
   } catch (const std::exception &e) {
      std::cerr << "rntupletryzip: " << e.what() << '\n';
      std::cerr << kUsage;
      return 1;
   }
}

} // namespace

int main(int argc, char **argv)
{
   return RunProgram(argc, argv);
}
