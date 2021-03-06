//===--- Refactoring.cpp - Framework for clang refactoring tools ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  Implements tools to support refactorings.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_os_ostream.h"
#include <algorithm>

#include "Refactoring.h"

static const char *const InvalidLocation = "";

using namespace clang;
using namespace clang::tooling;

Replacement::Replacement() : FilePath(InvalidLocation), Offset(0), Length(0) {}

Replacement::Replacement(llvm::StringRef FilePath, unsigned Offset,
                         unsigned Length, llvm::StringRef ReplacementText)
    : FilePath(FilePath), Offset(Offset), Length(Length),
      ReplacementText(ReplacementText) {}

Replacement::Replacement(SourceManager &Sources, SourceLocation Start,
                         unsigned Length, llvm::StringRef ReplacementText) {
  setFromSourceLocation(Sources, Start, Length, ReplacementText);
}

Replacement::Replacement(SourceManager &Sources, const CharSourceRange &Range,
                         llvm::StringRef ReplacementText) {
  setFromSourceRange(Sources, Range, ReplacementText);
}

bool Replacement::isApplicable() const { return FilePath != InvalidLocation; }

bool Replacement::apply(Rewriter &Rewrite) const {
  SourceManager &SM = Rewrite.getSourceMgr();
  const FileEntry *Entry = SM.getFileManager().getFile(FilePath);
  if (Entry == NULL)
    return false;
  FileID ID;
  // FIXME: Use SM.translateFile directly.
  SourceLocation Location = SM.translateFileLineCol(Entry, 1, 1);
  ID = Location.isValid()
           ? SM.getFileID(Location)
           : SM.createFileID(Entry, SourceLocation(), SrcMgr::C_User);
  // FIXME: We cannot check whether Offset + Length is in the file, as
  // the remapping API is not public in the RewriteBuffer.
  const SourceLocation Start =
      SM.getLocForStartOfFile(ID).getLocWithOffset(Offset);
  // ReplaceText returns false on success.
  // ReplaceText only fails if the source location is not a file location, in
  // which case we already returned false earlier.
  bool RewriteSucceeded = !Rewrite.ReplaceText(Start, Length, ReplacementText);
  assert(RewriteSucceeded);
  return RewriteSucceeded;
}

std::string Replacement::toString() const {
  std::string result;
  llvm::raw_string_ostream stream(result);
  stream << FilePath << ": " << Offset << ":+" << Length << ":\""
         << ReplacementText << "\"";
  return result;
}

bool Replacement::Equal::operator()(const Replacement &R1,
                                    const Replacement &R2) const {
  return R1.FilePath == R2.FilePath && R1.Offset == R2.Offset &&
         R1.Length == R2.Length && R1.ReplacementText == R2.ReplacementText;
}

void Replacement::setFromSourceLocation(SourceManager &Sources,
                                        SourceLocation Start, unsigned Length,
                                        llvm::StringRef ReplacementText) {
  const std::pair<FileID, unsigned> DecomposedLocation =
      Sources.getDecomposedLoc(Start);
  const FileEntry *Entry = Sources.getFileEntryForID(DecomposedLocation.first);
  this->FilePath = Entry != NULL ? Entry->getName() : InvalidLocation;
  this->Offset = DecomposedLocation.second;
  this->Length = Length;
  this->ReplacementText = ReplacementText;
}

// FIXME: This should go into the Lexer, but we need to figure out how
// to handle ranges for refactoring in general first - there is no obvious
// good way how to integrate this into the Lexer yet.
static int getRangeSize(SourceManager &Sources, const CharSourceRange &Range) {
  SourceLocation SpellingBegin = Sources.getSpellingLoc(Range.getBegin());
  SourceLocation SpellingEnd = Sources.getSpellingLoc(Range.getEnd());
  std::pair<FileID, unsigned> Start = Sources.getDecomposedLoc(SpellingBegin);
  std::pair<FileID, unsigned> End = Sources.getDecomposedLoc(SpellingEnd);
  if (Start.first != End.first)
    return -1;
  if (Range.isTokenRange())
    End.second +=
        Lexer::MeasureTokenLength(SpellingEnd, Sources, LangOptions());
  return End.second - Start.second;
}

void Replacement::setFromSourceRange(SourceManager &Sources,
                                     const CharSourceRange &Range,
                                     llvm::StringRef ReplacementText) {
  setFromSourceLocation(Sources, Sources.getSpellingLoc(Range.getBegin()),
                        getRangeSize(Sources, Range), ReplacementText);
}

bool applyAllReplacements(Replacements &Replaces, Rewriter &Rewrite) {
  bool Result = true;
  for (Replacements::iterator B = Replaces.begin(), I = B, E = Replaces.end();
       I != E; ++I) {
    Replacements::iterator S =
        std::search(I + 1, E, I, I + 1, Replacement::Equal());
    if (S != E) {
      I = Replaces.erase(I) - 1;
      E = Replaces.end();
    }
  }
  for (Replacements::const_iterator I = Replaces.begin(), E = Replaces.end();
       I != E; ++I) {
    if (I->isApplicable()) {
      Result = I->apply(Rewrite) && Result;
    } else {
      Result = false;
    }
  }
  return Result;
}

bool saveRewrittenFiles(Rewriter &Rewrite) {
  for (Rewriter::buffer_iterator I = Rewrite.buffer_begin(),
                                 E = Rewrite.buffer_end();
       I != E; ++I) {
    // FIXME: This code is copied from the FixItRewriter.cpp - I think it should
    // go into directly into Rewriter (there we also have the Diagnostics to
    // handle the error cases better).
    const FileEntry *Entry = Rewrite.getSourceMgr().getFileEntryForID(I->first);
    std::string ErrorInfo;
    llvm::raw_fd_ostream FileStream(Entry->getName(), ErrorInfo,
                                    llvm::raw_fd_ostream::F_Binary);
    llvm::raw_fd_ostream BackupStream(
        (std::string(Entry->getName()) + ".orig").c_str(), ErrorInfo,
        llvm::raw_fd_ostream::F_Binary);
    if (!ErrorInfo.empty())
      return false;
    BackupStream << Rewrite.getSourceMgr().getBufferData(I->first);
    BackupStream.flush();
    I->second.write(FileStream);
    FileStream.flush();
  }
  return true;
}

RefactoringTool::RefactoringTool(const CompilationDatabase &Compilations,
                                 ArrayRef<std::string> SourcePaths)
    : Tool(Compilations, SourcePaths) {}

Replacements &RefactoringTool::getReplacements() { return Replace; }

int RefactoringTool::run(FrontendActionFactory *ActionFactory) {
  int Result = Tool.run(ActionFactory);
  LangOptions DefaultLangOptions;

  DiagnosticOptions *DefaultDiagnosticOptions = new DiagnosticOptions;
  TextDiagnosticPrinter *DiagnosticPrinter =
      new TextDiagnosticPrinter(llvm::errs(), DefaultDiagnosticOptions);
  IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagID(new clang::DiagnosticIDs());
  DiagnosticsEngine Diagnostics(DiagID, DefaultDiagnosticOptions,
                                DiagnosticPrinter);

  SourceManager Sources(Diagnostics, Tool.getFiles());
  Rewriter Rewrite(Sources, DefaultLangOptions);
  if (!applyAllReplacements(Replace, Rewrite)) {
    llvm::errs() << "Skipped some replacements.\n";
  }
  if (!saveRewrittenFiles(Rewrite)) {
    llvm::errs() << "Could not save rewritten files.\n";
    return 1;
  }
  return Result;
}
