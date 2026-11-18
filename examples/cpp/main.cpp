#include "ets2la_depth/depth_reader.h"
#include <opencv2/opencv.hpp>
#include <cstdio>

int main() {
    ets2la_depth::DepthReader reader;
    ets2la_depth::DepthFrame frame;

    while (true) {
        if (!reader.get_frame(frame)) {
            continue;
        }

        cv::Mat rawHalf(int(frame.height), int(frame.width), CV_16F, frame.data.data(), frame.stride);
        cv::Mat raw;
        rawHalf.convertTo(raw, CV_32F);
        cv::Mat skyMask = raw <= -65503.0;
        cv::Mat negated = -raw;
        cv::Mat meters = (cv::min)((cv::max)(negated, 1.0), 200.0);
        cv::log(meters, meters);
        meters /= std::log(200.0);
        cv::Mat index;
        meters.convertTo(index, CV_8U, -255.0, 255.0);
        cv::Mat color;
        cv::applyColorMap(index, color, cv::COLORMAP_TURBO);
        color.setTo(cv::Scalar(0, 0, 0), skyMask);

        cv::imshow("ets2la_depth", color);
        cv::waitKey(1);
    }
    return 0;
}