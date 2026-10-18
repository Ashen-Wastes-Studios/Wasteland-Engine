// Python bindings for Wasteland::Age.
//
// Kept out of Bindings.cpp deliberately: that file is 230 KB, and the age
// module has a property nothing else in the engine has -- every binding here is
// READ-ONLY on enforcement, which is worth being able to review in one place.
//
// WHY READ-ONLY, AND WHY IT MATTERS MORE THAN IT LOOKS
//
// Python is the game's own scripting layer. It is inside the trust boundary of
// the thing being protected, not outside it: a mod, a gameplay script, or a
// remote-controlled entity could all run this. So any setter on SafetyProfile
// would be a setter that a script could use to hand a thirteen-year-old the
// adult feature set.
//
// There are no setters here, and there is no bound constructor. A script can
// ASK what the protections are. It cannot express a different answer.
//
// The two things that ARE writable are deliberate:
//
//   * EuScope -- a build configuration (does EU law apply to this release),
//     not a per-player safety setting. Set once at startup. It narrows the
//     jurisdiction rules and cannot relax SafetyProfile; see the .cpp.
//   * Refresh -- re-query the provider, for a studio that completes an OS
//     age-assurance flow and needs to pick the result up.
//
// NEITHER INSTRUMENT IS IN FORCE. See docs/AGE_ASSURANCE.md.

#include "wlpch.h"

#include "Wasteland/Scripting/BindingsAge.h"

#include <pybind11/pybind11.h>

#include <pybind11/stl.h>

#include "Wasteland/Age/AgeAuditLog.h"
#include "Wasteland/Age/AgeService.h"
#include "Wasteland/Age/GuardianPolicy.h"
#include "Wasteland/Age/SafetyProfile.h"

namespace py = pybind11;

void Wasteland::BindAge(py::module_ &m)
{
	// A submodule, so a script reads Wasteland.Age.Profile() rather than
	// Wasteland.Profile() sitting among a thousand other names.
	auto age = m.def_submodule("Age", "Age assurance: bands, safety profiles and guardian policy. Read-only.");

	// ---------------------------------------------------------------- enums --

	// py::enum_ without py::arithmetic() means a script cannot do arithmetic or
	// comparisons on band values -- it can only compare them for identity.
	// That is deliberate: the bands are not a numeric scale.
	py::enum_<Wasteland::Age::AgeBand>(age, "AgeBand")
		.value("Under13", Wasteland::Age::AgeBand::Under13)
		.value("Age13To15", Wasteland::Age::AgeBand::Age13To15)
		.value("Age16To17", Wasteland::Age::AgeBand::Age16To17)
		.value("Adult18", Wasteland::Age::AgeBand::Adult18)
		// `Unknown` is a real value, not a null: it is what a build reports when
		// no age provider has answered. Scripts must handle it explicitly --
		// treating it as Adult would silently unlock adult content.
		.value("Unknown", Wasteland::Age::AgeBand::Unknown);

	// Ordered, strongest last. The ordering is load-bearing in C++ and is
	// preserved here so `level >= Age.AssuranceLevel.OsSignal` behaves the
	// same in a script as in the engine.
	// NOTE: the comment above claims the ordering is preserved for `>=`, but
	// py::enum_ does not define __ge__ without py::arithmetic(). As bound here,
	// a script CANNOT order these values; it would have to compare against
	// .value() or use a mapping. The C++ ordering is real, the Python
	// comparison operators are not.
	py::enum_<Wasteland::Age::AssuranceLevel>(age, "AssuranceLevel")
		.value("SelfDeclared", Wasteland::Age::AssuranceLevel::SelfDeclared)
		.value("OsSignal", Wasteland::Age::AssuranceLevel::OsSignal)
		.value("Commercial", Wasteland::Age::AssuranceLevel::Commercial)
		.value("EuEaa", Wasteland::Age::AssuranceLevel::EuEaa);

	// The surface is an INPUT to the authorization decision, not a filter:
	// passing a stricter surface than the content actually is would over-restrict,
	// passing a laxer one would under-restrict. A script therefore supplies it
	// as a declaration, not as a decision.
	py::enum_<Wasteland::Age::SurfaceKind>(age, "SurfaceKind")
		.value("OnlineGame", Wasteland::Age::SurfaceKind::OnlineGame)
		.value("AiChatbot", Wasteland::Age::SurfaceKind::AiChatbot)
		.value("AppStore", Wasteland::Age::SurfaceKind::AppStore)
		.value("SocialNetwork", Wasteland::Age::SurfaceKind::SocialNetwork)
		.value("VideoSharing", Wasteland::Age::SurfaceKind::VideoSharing);

	py::enum_<Wasteland::Age::GuardianState>(age, "GuardianState")
		.value("Unconfigured", Wasteland::Age::GuardianState::Unconfigured)
		.value("MinorNeedsSetup", Wasteland::Age::GuardianState::MinorNeedsSetup)
		.value("Minor", Wasteland::Age::GuardianState::Minor)
		.value("Adult", Wasteland::Age::GuardianState::Adult)
		.value("PendingReview", Wasteland::Age::GuardianState::PendingReview);

	py::enum_<Wasteland::Age::AuditDecision>(age, "AuditDecision")
		.value("Granted", Wasteland::Age::AuditDecision::Granted)
		.value("Denied", Wasteland::Age::AuditDecision::Denied);

	py::enum_<Wasteland::Age::DenyReason>(age, "DenyReason")
		.value("None", Wasteland::Age::DenyReason::None)
		.value("AssuranceInsufficient", Wasteland::Age::DenyReason::AssuranceInsufficient)
		.value("SelfDeclared", Wasteland::Age::DenyReason::SelfDeclared)
		.value("NotRecorded", Wasteland::Age::DenyReason::NotRecorded);

	// -------------------------------------------------------- SafetyProfile --
	//
	// Returned BY VALUE on purpose. SafetyProfile is immutable in C++ (no
	// assignment operator), so a live reference would imply mutability that
	// does not exist -- and pybind11 would have to fabricate it.
	// NOTE: no py::init(), so a script cannot construct one. Every value a
	// script sees came from C++ through Profile(). That closes the obvious
	// bypass -- building a permissive profile object in Python -- without
	// needing any check inside the C++ consumers.
	py::class_<Wasteland::Age::SafetyProfile>(age, "SafetyProfile")
		// def_property_readonly raises AttributeError on assignment, which is the
		// enforcement: not merely absent, but actively refused. Using
		// def_readwrite here would be a silent hole.
		.def_property_readonly("ProfileVisibilityPublic", &Wasteland::Age::SafetyProfile::ProfileVisibilityPublic)
		.def_property_readonly("DirectMessagesFromStrangers", &Wasteland::Age::SafetyProfile::DirectMessagesFromStrangers)
		.def_property_readonly("FreeTextChat", &Wasteland::Age::SafetyProfile::FreeTextChat)
		.def_property_readonly("VoiceChat", &Wasteland::Age::SafetyProfile::VoiceChat)
		.def_property_readonly("UserGeneratedContentUpload", &Wasteland::Age::SafetyProfile::UserGeneratedContentUpload)
		.def_property_readonly("MarketplaceAccess", &Wasteland::Age::SafetyProfile::MarketplaceAccess)
		.def_property_readonly("LocationSharing", &Wasteland::Age::SafetyProfile::LocationSharing)
		.def_property_readonly("FriendRequestsRequireConfirmation", &Wasteland::Age::SafetyProfile::FriendRequestsRequireConfirmation)
		// PROTECTIVE flag: True is the RESTRICTED state. Named to make that
		// obvious, because it is the inverse of every property above it and a
		// script author will otherwise read it backwards.
		// INVERTED polarity, called out above the binding. Every other property
		// reads "True = this feature is available"; this one reads "True = this
		// feature is taken away". A script mixing the two conventions gets it
		// exactly backwards.
		.def_property_readonly("NightHoursRestricted", &Wasteland::Age::SafetyProfile::NightHoursRestricted)
		.def_property_readonly("ReportAvailable", &Wasteland::Age::SafetyProfile::ReportAvailable)
		.def_property_readonly("MuteAvailable", &Wasteland::Age::SafetyProfile::MuteAvailable);

	// ------------------------------------------------------- GuardianPolicy --

	// Lambdas, not member-function pointers.
	//
	// GuardianSettings' fields are PUBLIC and non-const, so
	// &GuardianSettings::ChatRestricted is `bool GuardianSettings::*` -- a
	// pointer-to-member that pybind11 cannot call on the const reference it gets
	// for a read-only property. That surfaces deep inside pybind11.h as
	// "cannot convert from 'const Getter' to 'cpp_function'", naming the
	// pybind11 header and none of the real culprits. SafetyProfile below gets
	// away with member pointers only because its accessors are const methods.
	// Again no py::init(): a script cannot synthesise guardian settings, only
	// read the ones policy produced.
	py::class_<Wasteland::Age::GuardianSettings>(age, "GuardianSettings")
		// The lambda's const-ref parameter is what makes this work where a member
		// pointer would not: pybind11 hands a read-only property a const object,
		// and a pointer-to-member of a non-const field cannot be called on one.
		// The lambda copies the value out instead, so mutating the returned Python
		// object cannot write back into the C++ field.
		.def_property_readonly("NightHoursEnforced", [](const Wasteland::Age::GuardianSettings &s) { return s.NightHoursEnforced; })
		.def_property_readonly("CurfewHour", [](const Wasteland::Age::GuardianSettings &s) { return s.CurfewHour; })
		.def_property_readonly("ReportActivityToGuardian", [](const Wasteland::Age::GuardianSettings &s) { return s.ReportActivityToGuardian; })
		.def_property_readonly("PurchaseLimitsEnforced", [](const Wasteland::Age::GuardianSettings &s) { return s.PurchaseLimitsEnforced; })
		.def_property_readonly("ChatRestricted", [](const Wasteland::Age::GuardianSettings &s) { return s.ChatRestricted; })
		.def_property_readonly("DailyPlaytimeMinutes", [](const Wasteland::Age::GuardianSettings &s) { return s.DailyPlaytimeMinutes; });

	// The "no limit" sentinel. A module function rather than a static property:
	// py::class_ has def_property_readonly_static, but the value is a constant
	// the whole module shares, so it belongs here.
	// A module-level FUNCTION, not a value. py::module_ has no property
	// support, and exposing the constant as a function keeps a script from
	// rebinding it: `Age.UnlimitedPlaytime = 999` raises, because the module
	// attribute is read-only from Python.
	age.def("UnlimitedPlaytime", []() { return Wasteland::Age::GuardianSettings::kUnlimited; });

	// No constructor bound: policies are minted in C++ and handed to scripts.
	py::class_<Wasteland::Age::GuardianPolicy>(age, "GuardianPolicy")
		.def_property_readonly("State", &Wasteland::Age::GuardianPolicy::State)
		.def_property_readonly("Band", &Wasteland::Age::GuardianPolicy::Band)
		// Settings: what is in force ignoring a pending change.
		// EffectiveSettings: what is enforced RIGHT NOW.
		// Exposing both is deliberate -- a script that renders UI needs to know
		// a change is pending, and a script that gates a feature must use the
		// effective one.
		.def_property_readonly("Settings", &Wasteland::Age::GuardianPolicy::Settings)
		.def_property_readonly("EffectiveSettings", &Wasteland::Age::GuardianPolicy::EffectiveSettings)
		.def_property_readonly("PendingSettings", &Wasteland::Age::GuardianPolicy::PendingSettings)
		.def_property_readonly("HasPendingChange", &Wasteland::Age::GuardianPolicy::HasPendingChange)
		// Transitions return NEW policies; there is no mutating variant bound.
		// A script can therefore compute a proposed change and hand it back to
		// C++ for review, but cannot apply one.
		// py::arg names the parameter so the generated signature and any generated
		// stubs read `proposed=...` instead of the C++ type name. This is the one
		// place a script can CREATE a policy change -- as a value that has no
		// effect until C++ reviews it.
		.def("WithPendingChange", &Wasteland::Age::GuardianPolicy::WithPendingChange, py::arg("proposed"))
		.def("Approved", &Wasteland::Age::GuardianPolicy::Approved)
		.def("Denied", &Wasteland::Age::GuardianPolicy::Denied);

	// ----------------------------------------------------------- audit log --

	py::class_<Wasteland::Age::AgeAuditEntry>(age, "AgeAuditEntry")
		.def_readonly("TimestampSeconds", &Wasteland::Age::AgeAuditEntry::TimestampSeconds)
		.def_readonly("Decision", &Wasteland::Age::AgeAuditEntry::Decision)
		.def_readonly("Surface", &Wasteland::Age::AgeAuditEntry::Surface)
		.def_readonly("RequiredAssurance", &Wasteland::Age::AgeAuditEntry::RequiredAssurance)
		.def_readonly("ProvidedAssurance", &Wasteland::Age::AgeAuditEntry::ProvidedAssurance)
		.def_readonly("Band", &Wasteland::Age::AgeAuditEntry::Band)
		.def_readonly("Reason", &Wasteland::Age::AgeAuditEntry::Reason);

	// --------------------------------------------------------- module funcs --

	age.def("IsMinor", []() { return Wasteland::Age::AgeService::Instance().IsMinor(); })
	    .def("IsVerified", []() { return Wasteland::Age::AgeService::Instance().IsVerified(); })
	    .def("Band", []() { return Wasteland::Age::AgeService::Instance().Band(); })
	    .def("Assurance", []() { return Wasteland::Age::AgeService::Instance().Assurance(); })
	    .def("HasProvider", []() { return Wasteland::Age::AgeService::Instance().HasProvider(); })
	    .def("Profile", []() { return Wasteland::Age::AgeService::Instance().Profile(); })
	    .def("Policy", []() { return Wasteland::Age::AgeService::Instance().Policy(); });

	// EuScope as two plain functions rather than a module-level property.
	//
	// py::module_ has no def_property (that is py::class_ only), and py::property
	// is not available in this binding's include set. Two functions read better
	// in a script than a property that looks assignable at import time, and they
	// make the write explicit at the call site: SetEuScope is visibly a
	// configuration change.
	age.def("GetEuScope", []() { return Wasteland::Age::AgeService::Instance().EuScope(); })
	    .def("SetEuScope", [](bool value) { Wasteland::Age::AgeService::Instance().SetEuScope(value); },
	         py::arg("enabled"),
	         "Does EU law apply to this build. True by default. Narrowing it changes which\n"
	         "jurisdiction rules are consulted; it does NOT relax the safety profile.");

	// Ask the provider again.
	//
	// BLOCKING and not thread-safe. A studio calls this after an OS
	// age-assurance flow completes. Calling it every frame is a bug: the
	// provider may perform a round trip, and this runs on the render thread
	// under the GIL.
	age.def("Refresh", []() { Wasteland::Age::AgeService::Instance().Refresh(); },
	        "Re-query the age provider. Blocking and not thread-safe -- call after a provider flow completes, never per frame.");

	// The gate. Returns True only when the policy finds the assurance
	// sufficient AND the decision was recorded.
	age.def("AuthorizeFeature",
	        [](Wasteland::Age::SurfaceKind surface) {
		        return Wasteland::Age::AgeAuditLog::Instance().AuthorizeFeature(
		            Wasteland::Age::AgeService::Instance(), surface,
		            Wasteland::Age::AgeService::Instance().Assurance());
	        },
	        py::arg("surface"));

	age.def("AuditEntryCount", []() { return Wasteland::Age::AgeAuditLog::Instance().Size(); })
	    .def("AuditEntries", []() { return Wasteland::Age::AgeAuditLog::Instance().Entries(); })
	    .def("AuditWrapped", []() { return Wasteland::Age::AgeAuditLog::Instance().Wrapped(); });
}