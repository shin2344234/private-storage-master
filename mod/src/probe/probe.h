#pragma once

// Research build. Read-only hooks on the warehouse UI, the menu queue, the mode
// switch and the panel lookup, pinned to exe 1.0.0.2850 and refused on any
// other build. It changes nothing in the game: every detour logs and then runs
// the original. The point is to record what the game itself does when a chest
// is opened in person, so the mod can do the same thing.
namespace psm::probe
{
    bool Start();
    void Stop();
}
