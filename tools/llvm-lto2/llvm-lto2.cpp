//===-- llvm-lto2: test harness for the resolution-based LTO interface ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program takes in a list of bitcode files, links them and performs
// link-time optimization according to the provided symbol resolutions using the
// resolution-based LTO interface, and outputs one or more object files.
//
// This program is intended to eventually replace llvm-lto which uses the legacy
// LTO interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/LTO/Caching.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Threading.h"

using namespace llvm;
using namespace lto;

static cl::opt<char>
    OptLevel("O", cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                           "(default = '-O2')"),
             cl::Prefix, cl::ZeroOrMore, cl::init('2'));

static cl::opt<char> CGOptLevel(
    "cg-opt-level",
    cl::desc("Codegen optimization level (0, 1, 2 or 3, default = '2')"),
    cl::init('2'));

static cl::list<std::string> InputFilenames(cl::Positional, cl::OneOrMore,
                                            cl::desc("<input bitcode files>"));

static cl::opt<std::string> OutputFilename("o", cl::Required,
                                           cl::desc("Output filename"),
                                           cl::value_desc("filename"));

static cl::opt<std::string> CacheDir("cache-dir", cl::desc("Cache Directory"),
                                     cl::value_desc("directory"));

static cl::opt<std::string> OptPipeline("opt-pipeline",
                                        cl::desc("Optimizer Pipeline"),
                                        cl::value_desc("pipeline"));

static cl::opt<std::string> AAPipeline("aa-pipeline",
                                       cl::desc("Alias Analysis Pipeline"),
                                       cl::value_desc("aapipeline"));

static cl::opt<bool> SaveTemps("save-temps", cl::desc("Save temporary files"));

static cl::opt<bool>
    ThinLTODistributedIndexes("thinlto-distributed-indexes", cl::init(false),
                              cl::desc("Write out individual index and "
                                       "import files for the "
                                       "distributed backend case"));

static cl::opt<int> Threads("thinlto-threads",
                            cl::init(llvm::heavyweight_hardware_concurrency()));

static cl::list<std::string> SymbolResolutions(
    "r",
    cl::desc("Specify a symbol resolution: filename,symbolname,resolution\n"
             "where \"resolution\" is a sequence (which may be empty) of the\n"
             "following characters:\n"
             " p - prevailing: the linker has chosen this definition of the\n"
             "     symbol\n"
             " l - local: the definition of this symbol is unpreemptable at\n"
             "     runtime and is known to be in this linkage unit\n"
             " x - externally visible: the definition of this symbol is\n"
             "     visible outside of the LTO unit\n"
             "A resolution for each symbol must be specified."),
    cl::ZeroOrMore);

static cl::opt<std::string> OverrideTriple(
    "override-triple",
    cl::desc("Replace target triples in input files with this triple"));

static cl::opt<std::string> DefaultTriple(
    "default-triple",
    cl::desc(
        "Replace unspecified target triples in input files with this triple"));

static cl::opt<std::string>
    OptRemarksOutput("pass-remarks-output",
                     cl::desc("YAML output file for optimization remarks"));

static cl::opt<bool> OptRemarksWithHotness(
    "pass-remarks-with-hotness",
    cl::desc("Whether to include hotness informations in the remarks.\n"
             "Has effect only if -pass-remarks-output is specified."));

static cl::opt<bool>
    UseNewPM("use-new-pm",
             cl::desc("Run LTO passes using the new pass manager"),
             cl::init(false), cl::Hidden);

static void check(Error E, std::string Msg) {
  if (!E)
    return;
  handleAllErrors(std::move(E), [&](ErrorInfoBase &EIB) {
    errs() << "llvm-lto2: " << Msg << ": " << EIB.message().c_str() << '\n';
  });
  exit(1);
}

template <typename T> static T check(Expected<T> E, std::string Msg) {
  if (E)
    return std::move(*E);
  check(E.takeError(), Msg);
  return T();
}

static void check(std::error_code EC, std::string Msg) {
  check(errorCodeToError(EC), Msg);
}

template <typename T> static T check(ErrorOr<T> E, std::string Msg) {
  if (E)
    return std::move(*E);
  check(E.getError(), Msg);
  return T();
}

static int usage() {
  errs() << "Available subcommands: dump-symtab run\n";
  return 1;
}

static int run(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "Resolution-based LTO test harness");

  // FIXME: Workaround PR30396 which means that a symbol can appear
  // more than once if it is defined in module-level assembly and
  // has a GV declaration. We allow (file, symbol) pairs to have multiple
  // resolutions and apply them in the order observed.
  std::map<std::pair<std::string, std::string>, std::list<SymbolResolution>>
      CommandLineResolutions;
  for (std::string R : SymbolResolutions) {
    StringRef Rest = R;
    StringRef FileName, SymbolName;
    std::tie(FileName, Rest) = Rest.split(',');
    if (Rest.empty()) {
      llvm::errs() << "invalid resolution: " << R << '\n';
      return 1;
    }
    std::tie(SymbolName, Rest) = Rest.split(',');
    SymbolResolution Res;
    for (char C : Rest) {
      if (C == 'p')
        Res.Prevailing = true;
      else if (C == 'l')
        Res.FinalDefinitionInLinkageUnit = true;
      else if (C == 'x')
        Res.VisibleToRegularObj = true;
      else {
        llvm::errs() << "invalid character " << C << " in resolution: " << R
                     << '\n';
        return 1;
      }
    }
    CommandLineResolutions[{FileName, SymbolName}].push_back(Res);
  }

  std::vector<std::unique_ptr<MemoryBuffer>> MBs;

  Config Conf;
  Conf.DiagHandler = [](const DiagnosticInfo &DI) {
    DiagnosticPrinterRawOStream DP(errs());
    DI.print(DP);
    errs() << '\n';
    exit(1);
  };

  Conf.CPU = MCPU;
  Conf.Options = InitTargetOptionsFromCodeGenFlags();
  Conf.MAttrs = MAttrs;
  if (auto RM = getRelocModel())
    Conf.RelocModel = *RM;
  Conf.CodeModel = CMModel;

  if (SaveTemps)
    check(Conf.addSaveTemps(OutputFilename + "."),
          "Config::addSaveTemps failed");

  // Optimization remarks.
  Conf.RemarksFilename = OptRemarksOutput;
  Conf.RemarksWithHotness = OptRemarksWithHotness;

  // Run a custom pipeline, if asked for.
  Conf.OptPipeline = OptPipeline;
  Conf.AAPipeline = AAPipeline;

  Conf.OptLevel = OptLevel - '0';
  Conf.UseNewPM = UseNewPM;
  switch (CGOptLevel) {
  case '0':
    Conf.CGOptLevel = CodeGenOpt::None;
    break;
  case '1':
    Conf.CGOptLevel = CodeGenOpt::Less;
    break;
  case '2':
    Conf.CGOptLevel = CodeGenOpt::Default;
    break;
  case '3':
    Conf.CGOptLevel = CodeGenOpt::Aggressive;
    break;
  default:
    llvm::errs() << "invalid cg optimization level: " << CGOptLevel << '\n';
    return 1;
  }

  if (FileType.getNumOccurrences())
    Conf.CGFileType = FileType;

  Conf.OverrideTriple = OverrideTriple;
  Conf.DefaultTriple = DefaultTriple;

  ThinBackend Backend;
  if (ThinLTODistributedIndexes)
    Backend = createWriteIndexesThinBackend("", "", true, "");
  else
    Backend = createInProcessThinBackend(Threads);
  LTO Lto(std::move(Conf), std::move(Backend));

  bool HasErrors = false;
  for (std::string F : InputFilenames) {
    std::unique_ptr<MemoryBuffer> MB = check(MemoryBuffer::getFile(F), F);
    std::unique_ptr<InputFile> Input =
        check(InputFile::create(MB->getMemBufferRef()), F);

    std::vector<SymbolResolution> Res;
    for (const InputFile::Symbol &Sym : Input->symbols()) {
      auto I = CommandLineResolutions.find({F, Sym.getName()});
      if (I == CommandLineResolutions.end()) {
        llvm::errs() << argv[0] << ": missing symbol resolution for " << F
                     << ',' << Sym.getName() << '\n';
        HasErrors = true;
      } else {
        Res.push_back(I->second.front());
        I->second.pop_front();
        if (I->second.empty())
          CommandLineResolutions.erase(I);
      }
    }

    if (HasErrors)
      continue;

    MBs.push_back(std::move(MB));
    check(Lto.add(std::move(Input), Res), F);
  }

  if (!CommandLineResolutions.empty()) {
    HasErrors = true;
    for (auto UnusedRes : CommandLineResolutions)
      llvm::errs() << argv[0] << ": unused symbol resolution for "
                   << UnusedRes.first.first << ',' << UnusedRes.first.second
                   << '\n';
  }
  if (HasErrors)
    return 1;

  auto AddStream =
      [&](size_t Task) -> std::unique_ptr<lto::NativeObjectStream> {
    std::string Path = OutputFilename + "." + utostr(Task);

    std::error_code EC;
    auto S = llvm::make_unique<raw_fd_ostream>(Path, EC, sys::fs::F_None);
    check(EC, Path);
    return llvm::make_unique<lto::NativeObjectStream>(std::move(S));
  };

  auto AddBuffer = [&](size_t Task, std::unique_ptr<MemoryBuffer> MB) {
    *AddStream(Task)->OS << MB->getBuffer();
  };

  NativeObjectCache Cache;
  if (!CacheDir.empty())
    Cache = check(localCache(CacheDir, AddBuffer), "failed to create cache");

  check(Lto.run(AddStream, Cache), "LTO::run failed");
  return 0;
}

static int dumpSymtab(int argc, char **argv) {
  for (StringRef F : make_range(argv + 1, argv + argc)) {
    std::unique_ptr<MemoryBuffer> MB = check(MemoryBuffer::getFile(F), F);
    std::unique_ptr<InputFile> Input =
        check(InputFile::create(MB->getMemBufferRef()), F);

    outs() << "target triple: " << Input->getTargetTriple() << '\n';
    Triple TT(Input->getTargetTriple());

    outs() << "source filename: " << Input->getSourceFileName() << '\n';

    if (TT.isOSBinFormatCOFF())
      outs() << "linker opts: " << Input->getCOFFLinkerOpts() << '\n';

    std::vector<StringRef> ComdatTable = Input->getComdatTable();
    for (const InputFile::Symbol &Sym : Input->symbols()) {
      switch (Sym.getVisibility()) {
      case GlobalValue::HiddenVisibility:
        outs() << 'H';
        break;
      case GlobalValue::ProtectedVisibility:
        outs() << 'P';
        break;
      case GlobalValue::DefaultVisibility:
        outs() << 'D';
        break;
      }

      auto PrintBool = [&](char C, bool B) { outs() << (B ? C : '-'); };
      PrintBool('U', Sym.isUndefined());
      PrintBool('C', Sym.isCommon());
      PrintBool('W', Sym.isWeak());
      PrintBool('I', Sym.isIndirect());
      PrintBool('O', Sym.canBeOmittedFromSymbolTable());
      PrintBool('T', Sym.isTLS());
      PrintBool('X', Sym.isExecutable());
      outs() << ' ' << Sym.getName() << '\n';

      if (Sym.isCommon())
        outs() << "         size " << Sym.getCommonSize() << " align "
               << Sym.getCommonAlignment() << '\n';

      int Comdat = Sym.getComdatIndex();
      if (Comdat != -1)
        outs() << "         comdat " << ComdatTable[Comdat] << '\n';

      if (TT.isOSBinFormatCOFF() && Sym.isWeak() && Sym.isIndirect())
        outs() << "         fallback " << Sym.getCOFFWeakExternalFallback() << '\n';
    }

    outs() << '\n';
  }

  return 0;
}

int main(int argc, char **argv) {
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // FIXME: This should use llvm::cl subcommands, but it isn't currently
  // possible to pass an argument not associated with a subcommand to a
  // subcommand (e.g. -use-new-pm).
  if (argc < 2)
    return usage();

  StringRef Subcommand = argv[1];
  // Ensure that argv[0] is correct after adjusting argv/argc.
  argv[1] = argv[0];
  if (Subcommand == "dump-symtab")
    return dumpSymtab(argc - 1, argv + 1);
  if (Subcommand == "run")
    return run(argc - 1, argv + 1);
  return usage();
}
