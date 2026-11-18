# ets2la_depth

In-process frame depth capture for Euro Truck Simulator 2 / American Truck Simulator that shares the game's depth map through shared memory.

## Build

Requires CMake 3.15+ and MSVC (Visual Studio 2019+).
[MinHook](https://github.com/TsudaKageyu/minhook) is fetched automatically
at configure time.

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `ets2la_depth.dll` (put it in `<game>/bin/win_x64/plugins/`)

Python client: `pip install ./python` (see `python/README.md`).

## Reading frames

```python
from ets2la_depth import DepthReader

reader = DepthReader()
frame = reader.get_frame()
```

```cpp
#include "ets2la_depth/depth_reader.h"

ets2la_depth::DepthReader reader;
ets2la_depth::DepthFrame frame;
if (reader.get_frame(frame)) {}
```

* `get_frame` blocks for a frame newer than the last one this reader got.
  First call after connecting waits for a fresh frame.
* `get_latest_frame` is non-blocking.
* Zero-copy views stay valid for ~5 newer frames (ring of 6). Finish (or
  `.copy()`) within about 4 frame times, then check `valid()`.
* Pair with ets2la_capture frames by `timestamp_qpc`: `reader.get_frame_near(ts)`.
* Known issue: with TAA enabled, the depth map jiggles about +-1 px per frame.