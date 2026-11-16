// The size the game renders matches at. With the display at 1280x720 the game draws the match at
// 1152x640 and stretches it to 1280x720 (as on the console), which softens all of it a little;
// native_720p renders it at 1280x720 instead, so nothing is stretched. Set before the game runs.
#pragma once

namespace scene_size
{
    void setNative720p(bool on);
}
