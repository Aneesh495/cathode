// ==========================================================================
// cpp_abi.cpp — ABI version handshake for the C++ subsystems.
// The rest of the C++ core (tracer, synth, scene graph) lives in sibling TUs;
// this one anchors the version contract and documents the extern "C" boundary.
// ==========================================================================
#include "cathode/cppcore.h"

extern "C" u32 cpp_core_abi_version(void) {
    return CATHODE_CPP_ABI;   // == 6 after adding the soft-body simulator
}
