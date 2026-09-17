#pragma once

// Opening storage from anywhere, in the game's own warehouse screen.
//
// An open posts the stage chart UIControl events a real chest sends, with a
// stage id no real stage uses, plus the IngameMenu phase and the chart's input
// block. The mod owns the close: Esc or B sends "Close" to the stage id, which
// the mod answers with the chart's close events. private/research/R3 section 8
// and R4 have the evidence.
namespace psm::storage
{
    bool Start();
    void Stop();
}
