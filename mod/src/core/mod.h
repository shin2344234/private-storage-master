#pragma once
#include <Windows.h>

namespace psm::Mod
{
    void Initialize(HMODULE module);
    void Shutdown(bool processExiting);
}
