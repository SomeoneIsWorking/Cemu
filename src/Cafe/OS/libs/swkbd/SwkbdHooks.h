#pragma once

#include <string>

// The boundary through which a first-party runtime fills the software keyboard's text when a
// title shows it.
namespace SwkbdHooks {

// Asked when a title shows the keyboard on its own. An empty answer leaves the text empty, which
// is the behaviour when nothing is installed.
class InitialText {
  public:
    virtual ~InitialText() = default;
    virtual std::u16string Provide() = 0;
};

// Null until something registers; nullptr restores upstream behaviour.
void SetInitialText(InitialText* source);
InitialText* GetInitialText();

} // namespace SwkbdHooks
