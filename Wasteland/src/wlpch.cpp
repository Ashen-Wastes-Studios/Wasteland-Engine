// The entire translation unit. Its only job is to force wlpch.h into
// exactly one object file so the precompiled header is actually
// compiled once rather than being discarded by the compiler.
// Without this file the PCH is generated and then dropped, and every
// including .cpp pays full parse cost.
#include "wlpch.h"