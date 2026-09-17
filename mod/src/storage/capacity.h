#pragma once

// Storage sizes. The InventoryInfo reader is hooked and, after it returns, the
// default and maximum slot counts of the configured inventories are shifted by
// the same amount before any save builds its storage from them
// (private/research/R2-capacity.md section 6). Live storage is never written,
// so turning a setting off restores the game's sizes on the next start and
// purchased expansions survive either way.
namespace psm::capacity
{
    // Called as early as possible: sizes only apply to storage built after the hook.
    void Start();
    // Log every inventory record and every storage the player has.
    void RequestDump();
    void Stop();
}
