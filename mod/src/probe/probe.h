#pragma once

// Research build, pinned to exe 1.0.0.2850 and refused on any other build.
// Probe 1 only watched natural chest opens. Probe 2 keeps those hooks and adds
// a remote open of each chest through the game's StageChartUIControl event
// wrap, an owned close on the warehouse close routine, and move logging.
namespace psm::probe
{
    bool Start();
    void Stop();
}
