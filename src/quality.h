// amped3 - graphics quality: a preset (Low / Medium / High / Ultra) and a
// setting for each part of the lighting (sun shadows, lamp shadows, the
// atmosphere, ambient occlusion, surface detail), each of which sets the
// detailed settings under it. The detailed settings stay adjustable: a part
// whose settings match none of its levels shows Custom (and the preset too,
// when the parts match none of its).
//
// Cvars: native_quality (-1 Custom, 0 Low .. 3 Ultra) and
// native_quality_sun_shadows / _lamp_shadows / _atmosphere /
// _ambient_occlusion / _surface (-1 Custom, then their levels).

#pragma once

namespace amped3::quality {

// Every frame before the renderer's settings are read: a preset or a part
// just changed sets what's under it; then each shows what matches.
void Apply();

}  // namespace amped3::quality
