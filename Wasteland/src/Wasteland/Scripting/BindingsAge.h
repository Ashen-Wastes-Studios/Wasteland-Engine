// Python bindings for Wasteland::Age -- declaration only.
//
// The implementation lives in BindingsAge.cpp and is called from
// PYBIND11_MODULE(Wasteland, m) in Bindings.cpp.
//
// Split out of Bindings.cpp so that the read-only property of every binding in
// this module can be reviewed in one place. See the .cpp for why read-only is
// the point rather than an omission.

#pragma once

#include <pybind11/pybind11.h>

namespace Wasteland
{

// Registers Wasteland.Age: enums, SafetyProfile, GuardianPolicy, the audit
// entry, and the read-only module functions.
//
// Exposes no way to change an enforcement decision from Python.
void BindAge(pybind11::module_ &m);

} // namespace Wasteland