#pragma once

#include "nga/diag/DiagnosticId.hpp"

#include <string>
#include <string_view>

namespace nga::diag
{

/// The Step a diagnostic's number is grouped under, which is what makes the
/// number alone say where to look. The ranges are declared beside this
/// function and explained in docs/spec/diagnostics.md.
std::string_view stepOf( DiagnosticId id );

/// Every diagnostic the tool can raise, as JSON: identifier, stable name,
/// default severity, Step, and the message template with its named arguments
/// left in.
///
/// It describes the tool and not a build, so it is written without a Project
/// and says nothing about one. What a reader does with it is turn `NGA5205`
/// into something they can look up: the tutorial's seventeenth chapter and the
/// published site are both generated from this.
std::string renderCatalogueJson();

} // namespace nga::diag
