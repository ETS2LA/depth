from ets2la_capture import FrameReader
from ets2la_depth import DepthReader

capture = FrameReader()
depth_capture = DepthReader()

while True:
    color = capture.get_frame(timeout_ms=1000)
    if color is None:
        print("failed to get capture frame")
        continue
    depth = depth_capture.get_frame_near(color.timestamp_qpc)
    if depth is None:
        print("no depth frame for this capture frame")
        continue
    offset_ms = depth_capture.ticks_to_ms(depth.timestamp_qpc - color.timestamp_qpc)
    x, y = color.width // 2, color.height // 2
    dx, dy = depth.map_from_frame_pixel(color.width, color.height, x, y)
    print(f"offset {offset_ms:+.3f} ms, distance at screen center {depth.meters_at(int(dx), int(dy)):.1f} m")