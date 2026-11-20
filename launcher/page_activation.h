#pragma once

namespace dcp {

// A page is drawn every frame it is open; it is entered on the first frame after one in which it was not drawn.
class PageActivation {
public:
    bool Entered(int frame) {
        const bool entered = frame != last_frame_ + 1;
        last_frame_ = frame;
        return entered;
    }

private:
    int last_frame_ = -2;
};

} // namespace dcp
