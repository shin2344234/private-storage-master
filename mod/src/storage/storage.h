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
    // Whether keys are being held back from the game right now: the block is on
    // and a modifier some binding uses is down. Master Looter asks it from its
    // render and loot threads, so it is one atomic read, set by the poller.
    bool HidingKeys();

    // Whether the player was in free play on the last frame. Unknown when the
    // storage hooks are not in, since the frame tick is what reads it. NotFree
    // when no frame has run for a second, which is a load, a hang or shutdown.
    enum class Play { Unknown, Free, NotFree };
    Play PlayState();
}
