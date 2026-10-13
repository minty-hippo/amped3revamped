// amped3 - the version this build was made from: the branch and commit of the
// sources (git, or .amped3-source after a zip update), read when the build was
// configured. Unknown parts read "unknown".

#pragma once

#include <string>

namespace amped3::version {

const char* Branch();      // "main", "experimental", ... ("unknown")
const char* Commit();      // full hash ("unknown")
const char* Date();        // commit date, YYYY-MM-DD ("" when unknown)
std::string ShortCommit(); // first 7 characters
std::string Text();        // "experimental f69a25f (2026-10-13)"

}  // namespace amped3::version
