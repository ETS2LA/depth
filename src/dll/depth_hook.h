#pragma once

#include <d3d11.h>
#include <dxgi.h>

bool StartDepthHook();
void StopDepthHook(bool process_detach = false);