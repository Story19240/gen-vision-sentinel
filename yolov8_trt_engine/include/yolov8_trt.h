#pragma once
#include <string>
#include <vector>
#include <memory>
#include <NvInfer.h>
#include <opencv2/core.hpp>
#include "types.h"

class YOLOv8TRT {
public:
    YOLOv8TRT(const std::string& engine_path, const std::string& labels_path = "");
    ~YOLOv8TRT();

    bool init();
    std::vector<Detection> detect(const cv::Mat& bgr_img, float conf_thresh = 0.25f, float nms_thresh = 0.45f);

    int getInputWidth() const { return input_w_; }
    int getInputHeight() const { return input_h_; }

private:
    std::string engine_path_;
    std::string labels_path_;
    std::vector<std::string> class_names_;

    nvinfer1::IRuntime* runtime_ = nullptr;
    nvinfer1::ICudaEngine* engine_ = nullptr;
    nvinfer1::IExecutionContext* context_ = nullptr;

    void* input_buf_dev_ = nullptr;
    void* output_buf_dev_ = nullptr;
    std::vector<float> input_buf_host_;
    std::vector<float> output_buf_host_;

    int input_index_ = -1;
    int output_index_ = -1;
    size_t input_size_ = 0;
    size_t output_size_ = 0;

    int input_w_ = 640;
    int input_h_ = 640;
    int num_classes_ = 80;
    int num_anchors_ = 8400;

    void loadLabels();
    void preprocess(const cv::Mat& src, cv::Mat& dst, float& scale, int& pad_x, int& pad_y);
    void nms(std::vector<Detection>& input_boxes, float nms_thresh, std::vector<Detection>& output_boxes);
};
