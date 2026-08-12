#pragma once

#include "../core/Event.h"

namespace zb {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool inject(const InputEvent& ev) = 0;
};

} // namespace zb
