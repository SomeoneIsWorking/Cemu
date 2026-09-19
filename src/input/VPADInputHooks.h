#pragma once

#include <cstddef>
#include <cstdint>

// The one boundary through which a first-party runtime supplies gamepad input.
//
// It exists because an agent has to be able to drive the product, not only
// launch it and read what came out. A run nobody can press a button in never
// leaves the title screen, and every measurement taken from it describes an
// unattended screen rather than the game.
//
// The injection lands where a real controller's state would have landed:
// after the configured mappings have composed the hold mask and before press
// and release edges are derived from it. An injected press therefore produces
// the same edges a physical press does, and everything downstream -- the VPAD
// HLE, the title -- is the shipping path unchanged.
namespace VPADInputHooks {

// The Wii U gamepad hold bits, as the emulated controller composes them.
// Duplicated here so a consumer needs this header alone; the values are
// checked against the emulator's own enum where that enum is defined, so a
// change there fails the build rather than silently pressing the wrong
// button.
enum Button : uint32_t {
    kButtonA = 0x8000,
    kButtonB = 0x4000,
    kButtonX = 0x2000,
    kButtonY = 0x1000,
    kButtonL = 0x0020,
    kButtonR = 0x0010,
    kButtonZL = 0x0080,
    kButtonZR = 0x0040,
    kButtonPlus = 0x0008,
    kButtonMinus = 0x0004,
    kButtonUp = 0x0200,
    kButtonDown = 0x0100,
    kButtonLeft = 0x0800,
    kButtonRight = 0x0400,
};

// What to hold this read. Sticks are in the emulated controller's own units:
// -1 to 1 on each axis, with y positive upwards.
struct Injection {
    uint32_t holdMask{0};
    float leftStickX{0.0f};
    float leftStickY{0.0f};
    float rightStickX{0.0f};
    float rightStickY{0.0f};
};

// Asked once per gamepad read. Returning false means this player is not
// driven and the physical controllers decide, which is the behaviour when
// nothing is installed at all.
class Source {
  public:
    virtual ~Source() = default;
    virtual bool Poll(std::size_t playerIndex, Injection& injection) = 0;
};

// Null until something registers. Passing nullptr restores upstream
// behaviour, and is the state of any build that installs no runtime.
void SetSource(Source* source);
Source* GetSource();

} // namespace VPADInputHooks
