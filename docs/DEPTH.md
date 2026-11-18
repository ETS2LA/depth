# Depth data

* Pixels are IEEE half floats (DXGI_FORMAT_R16_FLOAT, format 54), view space z in
  meters, negative in front of the camera. Distance = -value. Sky / no geometry =
  -65504 (`kSkyValue`).
* Size = the game's render resolution = ceil8(window size x r_scale_x / r_scale_y).
  It is NOT the window size when resolution scaling is not 100 %. Map pixels of
  another image with `map_from_frame_pixel`.
* Precision (half float): about 6 cm at 100 m and 0.5 m at 1 km.

## How the texture is found
The plugin hooks OMSetRenderTargets. The largest pass with 4+ render targets whose
first target is RGBA16F is the G-buffer, the first single target pass after it with
a same sized R16_FLOAT target is the linear depth map. At Present the plugin copies
that texture.

## Latency
The depth texture is complete long before the game finishes the frame. The plugin
copies it right after the depth pass into a shared texture, and the worker thread
reads it back immediately. When the game calls Present(), the frame only has to be
stamped with that Present time and published. A depth frame is therefore usually
available sooner after Present() than the color frame of ets2la_capture.

## Syncing with ets2la_capture
Every depth frame carries `timestamp_qpc` (QPC at the start of the Present hook)
and a frame index. The capture plugin stamps the same moment of the same Present
call, so use `get_frame_near(capture_frame.timestamp_qpc)`.

## Use with ets2la_capture
```cpp
#include "ets2la_capture/frame_reader.h"
#include "ets2la_depth/depth_reader.h"

ets2la_capture::FrameReader capture;
ets2la_depth::DepthReader depth_capture;
ets2la_capture::Frame color;
ets2la_depth::DepthFrame depth;

if (capture.get_frame(color) && depth_capture.get_frame_near(depth, color.timestamp_qpc)) {
    uint32_t dx, dy;
    depth.map_from_frame_pixel(color.width, color.height, x, y, dx, dy);
    float meters = depth.meters_at(dx, dy);
}
```

```python
from ets2la_capture import FrameReader
from ets2la_depth import DepthReader

capture = FrameReader()
depth_capture = DepthReader()

while True:
    color = capture.get_frame()
    if color is None:
        continue
    depth = depth_capture.get_frame_near(color.timestamp_qpc)
    if depth is None:
        continue
    dx, dy = depth.map_from_frame_pixel(color.width, color.height, x, y)
    meters = depth.meters_at(dx, dy)
```