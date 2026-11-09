#include "opencv_source.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spark {
namespace {

std::string lower_extension(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string extension = path.substr(dot);
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension;
}

SyntheticFrame matrix_to_frame(const cv::Mat& matrix) {
    if (matrix.empty() || matrix.channels() < 3) {
        throw std::runtime_error("could not read a 3-channel BGR frame");
    }
    cv::Mat bgr;
    if (matrix.channels() == 3) {
        bgr = matrix;
    } else if (matrix.channels() == 4) {
        cv::cvtColor(matrix, bgr, cv::COLOR_BGRA2BGR);
    } else {
        throw std::runtime_error("unsupported image channel count: " +
            std::to_string(matrix.channels()));
    }
    if (!bgr.isContinuous()) {
        bgr = bgr.clone();
    }
    SyntheticFrame frame;
    frame.width = bgr.cols;
    frame.height = bgr.rows;
    frame.bgr.assign(
        bgr.data,
        bgr.data + static_cast<std::size_t>(bgr.total()) * bgr.channels());
    return frame;
}

void draw_roi(
    cv::Mat& image,
    const JsonValue* roi_config,
    const std::string& label,
    const cv::Scalar& color) {
    const ResolvedRoi roi = clip_roi(
        select_roi_profile(roi_config, image.cols, image.rows),
        image.cols,
        image.rows);
    if (!roi.valid) return;
    cv::rectangle(
        image,
        cv::Point(roi.roi.x1, roi.roi.y1),
        cv::Point(roi.roi.x2, roi.roi.y2),
        color,
        2);
    cv::putText(
        image,
        label,
        cv::Point(roi.roi.x1, std::max(16, roi.roi.y1 - 6)),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        color,
        2,
        cv::LINE_AA);
}

}  // namespace

bool is_image_path(const std::string& path) {
    const std::string extension = lower_extension(path);
    return extension == ".bmp" || extension == ".jpeg" || extension == ".jpg" ||
           extension == ".png" || extension == ".tif" || extension == ".tiff" ||
           extension == ".webp";
}

SyntheticFrame read_image_file(const std::string& path) {
    const cv::Mat matrix = cv::imread(path, cv::IMREAD_COLOR);
    if (matrix.empty()) throw std::runtime_error("could not read image: " + path);
    return matrix_to_frame(matrix);
}

std::pair<std::vector<SyntheticFrame>, double> read_video_file(
    const std::string& path,
    double fps_override) {
    cv::VideoCapture capture(path);
    if (!capture.isOpened()) {
        throw std::runtime_error("could not open video: " + path);
    }
    double fps = fps_override != 0.0
        ? fps_override
        : capture.get(cv::CAP_PROP_FPS);
    if (fps <= 0.0) fps = 25.0;

    std::vector<SyntheticFrame> frames;
    cv::Mat matrix;
    while (true) {
        if (!capture.read(matrix)) break;
        frames.push_back(matrix_to_frame(matrix));
    }
    capture.release();
    if (frames.empty()) {
        throw std::runtime_error("video contains no readable frames: " + path);
    }
    return {std::move(frames), fps};
}

void write_annotated(
    const std::string& path,
    const SyntheticFrame& frame,
    const DetectionResult& result,
    const JsonValue& config) {
    if (frame.width <= 0 || frame.height <= 0 || frame.bgr.size() !=
        static_cast<std::size_t>(frame.width) * frame.height * 3) {
        throw std::runtime_error("cannot annotate an invalid frame");
    }
    cv::Mat source(
        frame.height,
        frame.width,
        CV_8UC3,
        const_cast<std::uint8_t*>(frame.bgr.data()));
    cv::Mat image = source.clone();
    draw_roi(image, config.find("contact_roi"), "CONTACT", cv::Scalar(0, 220, 0));
    draw_roi(image, config.find("reference_roi"), "REFERENCE", cv::Scalar(255, 180, 0));
    draw_roi(image, config.find("falling_roi"), "FALLING", cv::Scalar(0, 165, 255));
    if (result.event_triggered) {
        std::string event_level = result.level;
        std::transform(event_level.begin(), event_level.end(), event_level.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::toupper(character));
            });
        const std::string event_text = "EVENT " + event_level + " " + result.color_type;
        cv::rectangle(image, cv::Point(0, 0), cv::Point(image.cols, 38), cv::Scalar(0, 0, 180), -1);
        cv::putText(
            image,
            event_text,
            cv::Point(12, 27),
            cv::FONT_HERSHEY_SIMPLEX,
            0.75,
            cv::Scalar(255, 255, 255),
            2,
            cv::LINE_AA);
    }
    if (!cv::imwrite(path, image)) {
        throw std::runtime_error("could not write annotated image: " + path);
    }
}

}  // namespace spark
