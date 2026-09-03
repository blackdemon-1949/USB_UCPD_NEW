/*
 * Out-of-class definitions of the constexpr static data members declared in
 * timers.h (PD_TIMEOUT::* and PD_TIMERS_RANGE::*).
 *
 * In C++14 a `static constexpr` data member of class type is NOT an inline
 * variable: it needs exactly one out-of-class definition whenever it is
 * ODR-used, and the PE/PRL/TC states ODR-use it constantly (they pass the
 * values by const reference, e.g. `timers.start(PD_TIMEOUT::tSenderResponse)`
 * or `timers.stop_range(PD_TIMERS_RANGE::PE)`).  Without the definitions the
 * firmware link fails with "undefined reference to pd::PD_TIMEOUT::...".
 *
 * The host test gates compile this same tree with gnu++17, where the members
 * are implicitly inline and the definitions below are simply not required -
 * but a single out-of-class definition remains valid there too, so this file
 * can stay unconditional.
 */
#include "timers.h"

namespace pd {

constexpr PD_TIMERS_RANGE::Type PD_TIMERS_RANGE::PE;
constexpr PD_TIMERS_RANGE::Type PD_TIMERS_RANGE::PRL;

constexpr PD_TIMEOUT::Type PD_TIMEOUT::TC_VBUS_DEBOUNCE;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::TC_CC_POLL;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tTypeCSinkWaitCap;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tSenderResponse;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tSinkRequest;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tPPSRequest;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tPSTransition_SPR;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tPSTransition_EPR;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tSinkEPRKeepAlive;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tEnterEPR;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tBISTCarrierMode;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tHardResetComplete;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tChunkSenderResponse;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tChunkSenderRequest;
constexpr PD_TIMEOUT::Type PD_TIMEOUT::tActiveCcPollingDebounce;

} // namespace pd
