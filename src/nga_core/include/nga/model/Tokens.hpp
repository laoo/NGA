#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// What a token is, as far as a reader of the text needs to know.
///
/// Coarser than the grammar and finer than the lexer, which is deliberately
/// blunt: one NUMBER for three bases, and no kind that tells a directive from
/// a qualified name, because DOT is never resolved there. What a name *means*
/// -- a Section, a Label, a Reference -- is not here either: that is a
/// question for the Symbols, and a token knows only where it stands.
enum class TokenClass : std::uint8_t
{
  COMMENT,
  LABEL,     ///< A definition, which position alone marks -- see 0007.
  DIRECTIVE, ///< `.section`, `.proc`: a dot opening a statement.
  MNEMONIC,  ///< A statement the ISA has.
  MACRO,     ///< A statement it does not, which is a macro being used.
  KEYWORD,   ///< A word of the Project file's grammar, or of C.
  NUMBER,
  STRING,
  CHARACTER,
  NAME, ///< An identifier anywhere but at the head of a statement.
  PUNCTUATION,
  UNKNOWN, ///< What the lexer rejected, and already reported.
};

std::string_view nameOf( TokenClass value );

/// Which grammar a file is read by, which is what the classifier needs to know
/// and what a Module's extension says.
enum class Grammar : std::uint8_t
{
  ASSEMBLER,
  PROJECT,
  C,
};

std::string_view nameOf( Grammar grammar );

/// One token, by where it stands in its own file rather than in the
/// SourceManager's virtual space: a consumer has the file and not the space.
struct ClassifiedToken
{
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  TokenClass classification = TokenClass::UNKNOWN;
};

/// Every token of one file, classified.
///
/// The classification is positional, as the grammar is: a name in column one
/// defines a Label, a dot at the head of a statement opens a directive, and a
/// word at the head of one is a mnemonic where the ISA has it and a macro
/// where it does not. Nothing here needs the build to have succeeded, which is
/// why a chapter teaching a refusal is still coloured.
///
/// Findings the lexer raises go to `sink`, which for this purpose is usually a
/// sink nobody reads: the file has already been lexed once, by the build.
std::vector<ClassifiedToken>
classify( diag::SourceManager const& sources, diag::FileId file, Grammar grammar, diag::DiagnosticSink& sink );

/// The grammar a path is read by, from its extension; the assembler for
/// anything else, which is what a Module with no path is.
Grammar grammarOf( std::string_view path );

/// Classify files that are not part of a build, as JSON.
///
/// `--facts` answers for the files of a Project, which is what a reader of a
/// program wants. This answers for text that no Project holds: the shapes of
/// syntax a reference chapter is written in, a fragment somebody pasted, a
/// file in an editor that has not been built. The classification is the same
/// one, since it never needed the build.
std::string renderClassificationJson( diag::SourceManager const& sources,
                                      std::vector<diag::FileId> const& files,
                                      diag::DiagnosticSink& sink );

} // namespace nga::model
