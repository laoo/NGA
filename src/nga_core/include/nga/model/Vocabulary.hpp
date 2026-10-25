#pragma once

#include <string>

namespace nga::model
{

/// The words of the languages the tool reads, as one JSON document: the
/// mnemonics of each processor, the assembler's directives and the Project
/// file's words. It describes the tool and not a build, as the diagnostics
/// catalogue does, and is what an editor completes a word from — see
/// docs/spec/facts.md.
std::string renderVocabularyJson();

} // namespace nga::model
