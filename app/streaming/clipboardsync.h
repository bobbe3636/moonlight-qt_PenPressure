#pragma once

#include "SDL_compat.h"

#include <memory>

class NvComputer;

// Text copied on one PC pastes on the other (Apollo; Windows). This PC's clipboard goes to the
// host when a stream window gets the focus (and when it changes while one has it); the host's
// comes back when the focus goes to another app here. Each screen's window talks to its own
// host instance, and all of them share which clipboard content is already on both sides, so the
// same text isn't sent again. Text a password manager marks as private stays here.
class ClipboardSync
{
public:
    explicit ClipboardSync(NvComputer* computer);
    ~ClipboardSync();

    // The stream window (on the main thread, after it exists)
    void setWindow(SDL_Window* window);

    void focusGained();
    void focusLost();

    // This PC's clipboard changed (the window's clipboard listener)
    void clipboardChanged();

    struct Shared;

private:
    void pushToHost();

    std::shared_ptr<Shared> m_Shared;
    void* m_Hwnd = nullptr;
};
