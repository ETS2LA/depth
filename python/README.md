# ets2la_depth

Lock-free reader for the `ets2la_depth.dll` shared-memory.

## Installation

```powershell
pip install ./python
```

## Usage

```python
from ets2la_depth import DepthReader

reader = DepthReader()
frame = reader.get_frame(timeout_ms=1000)  # frame.pixels: (H, W) float16, negative meters, sky = -65504
meters = frame.to_meters()  # float32 positive, sky = inf
near = reader.get_frame_near(capture_frame.timestamp_qpc)  # match an ets2la_capture frame
```

* `get_frame(copy=False)` returns a zero-copy view into shared memory. Check `frame.is_valid()` after use, if it is `False` the writer lapped you, discard.
* `get_latest_frame()` is non-blocking.
* `frame.age_ms()` is the time since the game presented the frame.
* `reader.stats()` shows published / dropped frames.
* `reader.connected()` reports whether a capture DLL is currently attached.