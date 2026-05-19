// Counters.h
// =============================================================================
// NAMED INTEGER COUNTERS - 'SUGGESTIONS_SHOWN', 'APPLY_FIRED', ETC.
// =============================================================================
// Written by: Eriumsss
//
// User actions and ranker fires bump named counters. The dashboard
// reads them for the editor's online metrics: show-rate, accept-
// rate, undo-rate. Locally tracked because Vespucci is single-user;
// these counters substitute for the server-side telemetry the
// AAA design doc would have us ship in a multi-tenant product.
// =============================================================================

#ifndef VESPUCCI_TELEMETRY_COUNTERS_H_
#define VESPUCCI_TELEMETRY_COUNTERS_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Telemetry {

void   IncrementCounter(const char* name, u64 delta = 1);
u64    GetCounter(const char* name);
void   ResetAllCounters();

i32         CounterCount();
const char* CounterNameAt(i32 i);
u64         CounterValueAt(i32 i);

} // namespace Telemetry
} // namespace Vespucci

#endif // VESPUCCI_TELEMETRY_COUNTERS_H_
