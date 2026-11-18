from ets2la_depth import DepthReader
import cv2

reader = DepthReader()

while True:
    frame = reader.get_frame()
    if frame is None:
        continue

    cv2.imshow("ets2la_depth", frame.to_colormap())
    cv2.waitKey(1)