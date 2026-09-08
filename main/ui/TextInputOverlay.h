#pragma once

#include "activities/TextInputActivity.h"
#include "input/touch_input.h"

#include <functional>
#include <memory>

namespace ui {

class TextInputOverlay {
public:
    TextInputOverlay();
    ~TextInputOverlay();

    TextInputOverlay(const TextInputOverlay &) = delete;
    TextInputOverlay &operator=(const TextInputOverlay &) = delete;

    void open(const TextInputOptions &options,
              TextInputCallback on_submit,
              std::function<void()> on_cancel = {});
    bool visible() const;
    bool handle_touch(const TouchEvent &event);
    bool handle_key(int key);
    void draw();
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace ui
