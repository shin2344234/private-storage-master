#pragma once

// Opening storage from anywhere, in the game's own warehouse screen.
//
// An open posts the stage chart UIControl events a real chest sends, with a
// stage id no real stage uses, plus the IngameMenu phase and the chart's input
// block. The mod owns the close: Esc or B sends "Close" to the stage id, which
// the mod answers with the chart's close events. The packets match a natural
// chest open, checked against captures of the game's own.
namespace psm::storage
{
    bool Start();
    void Stop();

    bool Ready();                 // hooks in, keys live
    bool KeyWindowFound();
    int  OpenStorage();           // index of the storage the mod has open, or -1
    // Master Looter calls this every frame its menu has the mouse, so rebinding
    // Ctrl+F1 there does not also open Private Storage. It lapses on its own.
    void PauseInput(unsigned ms);
}
