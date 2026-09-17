#pragma once
#include <cstdint>

// Xbox controllers through XInput. The mod polls all four slots itself, so a pad
// behind Steam Input or DS4Windows is seen wherever it enumerates. The game's own
// XInputGetState import is filtered so a bound combo's pressed button never
// reaches it, and B stays hidden after the mod closes a storage until it is let
// go, so the press that closed the screen does not also dodge.
namespace psm::pad
{
    bool Init();       // loads XInput and installs the import filter
    uint16_t Poll();   // buttons held on any pad, OR-ed together; poller thread only
    uint16_t Last();   // what the last Poll saw, from any thread
    int Slot();        // first XInput slot with a pad, or -1
    void HideBUntilReleased();
    void Shutdown();
}
