#include "yolov8_trt.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cuda_runtime_api.h>
#include <opencv2/imgproc.hpp>

class TRTLogger : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::cout << "[TRT] " << msg << std::endl;
        }
    }
};

static TRTLogger g_logger;

YOLOv8TRT::YOLOv8TRT(const std::string& engine_path, const std::string& labels_path)
    : engine_path_(engine_path), labels_path_(labels_path) {
    loadLabels();
}

YOLOv8TRT::~YOLOv8TRT() {
    if (input_buf_dev_) cudaFree(input_buf_dev_);
    if (output_buf_dev_) cudaFree(output_buf_dev_);
    if (context_) delete context_;
    if (engine_) delete engine_;
    if (runtime_) delete runtime_;
}

void YOLOv8TRT::loadLabels() {
    if (labels_path_.empty()) return;
    std::ifstream file(labels_path_);
    if (!file.is_open()) {
        std::cerr << "无法打开标签文件: " << labels_path_ << std::endl;
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) class_names_.push_back(line);
    }
}

bool YOLOv8TRT::init() {
    std::ifstream file(engine_path_, std::ios::binary);
    if (!file.good()) {
        std::cerr << "无法找到 Engine 文件: " << engine_path_ << std::endl;
        return false;
    }

    file.seekg(0, file.end);
    size_t size = file.tellg();
    file.seekg(0, file.beg);
    std::vector<char> engine_data(size);
    file.read(engine_data.data(), size);
    file.close();

    runtime_ = nvinfer1::createInferRuntime(g_logger);
    if (!runtime_) {
        std::cerr << "创建 TensorRT Runtime 失败" << std::endl;
        return false;
    }

    engine_ = runtime_->deserializeCudaEngine(engine_data.data(), size);
    if (!engine_) {
        std::cerr << "反序列化 TensorRT Engine 失败" << std::endl;
        return false;
    }

    context_ = engine_->createExecutionContext();
    if (!context_) {
        std::cerr << "创建 ExecutionContext 失败" << std::endl;
        return false;
    }

    int nb_bindings = engine_->getNbBindings();
    for (int i = 0; i < nb_bindings; ++i) {
        if (engine_->bindingIsInput(i)) {
            input_index_ = i;
            auto dims = engine_->getBindingDimensions(i);
            input_h_ = dims.d[2];
            input_w_ = dims.d[3];
            input_size_ = 1 * 3 * input_h_ * input_w_ * sizeof(float);
        } else {
            output_index_ = i;
            auto dims = engine_->getBindingDimensions(i);
            num_classes_ = dims.d[1] - 4;
            num_anchors_ = dims.d[2];
            output_size_ = 1 * dims.d[1] * num_anchors_ * sizeof(float);
        }
    }

    if (input_index_ == -1 || output_index_ == -1) {
        std::cerr << "无法解析 Engine 输入/输出 binding" << std::endl;
        return false;
    }

    cudaMalloc(&input_buf_dev_, input_size_);
    cudaMalloc(&output_buf_dev_, output_size_);
    input_buf_host_.resize(1 * 3 * input_h_ * input_w_);
    output_buf_host_.resize((num_classes_ + 4) * num_anchors_);

    std::cout << "[YOLOv8TRT] 初始化成功! 输入尺寸: " << input_w_ << "x" << input_h_
              << ", 类别数: " << num_classes_ << ", Anchors: " << num_anchors_ << std::endl;
    return true;
}

void YOLOv8TRT::preprocess(const cv::Mat& src, cv::Mat& dst, float& scale, int& pad_x, int& pad_y) {
    int w = src.cols;
    int h = src.rows;
    scale = std::min((float)input_w_ / w, (float)input_h_ / h);
    int new_w = (int)(w * scale);
    int new_h = (int)(h * scale);
    pad_x = (input_w_ - new_w) / 2;
    pad_y = (input_h_ - new_h) / 2;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h));

    dst = cv::Mat(input_h_, input_w_, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(dst(cv::Rect(pad_x, pad_y, new_w, new_h)));
}

void YOLOv8TRT::nms(std::vector<Detection>& input_boxes, float nms_thresh, std::vector<Detection>& output_boxes) {
    std::sort(input_boxes.begin(), input_boxes.end(), [](const Detection& a, const Detection& b) {
        return a.confidence > b.confidence;
    });

    std::vector<bool> is_suppressed(input_boxes.size(), false);
    for (size_t i = 0; i < input_boxes.size(); ++i) {
        if (is_suppressed[i]) continue;
        output_boxes.push_back(input_boxes[i]);

        float area_i = (input_boxes[i].x2 - input_boxes[i].x1) * (input_boxes[i].y2 - input_boxes[i].y1);
        for (size_t j = i + 1; j < input_boxes.size(); ++j) {
            if (is_suppressed[j]) continue;
            if (input_boxes[i].class_id != input_boxes[j].class_id) continue;

            float xx1 = std::max(input_boxes[i].x1, input_boxes[j].x1);
            float yy1 = std::max(input_boxes[i].y1, input_boxes[j].y1);
            float xx2 = std::min(input_boxes[i].x2, input_boxes[j].x2);
            float yy2 = std::min(input_boxes[i].y2, input_boxes[j].y2);

            float inter_w = std::max(0.0f, xx2 - xx1);
            float inter_h = std::max(0.0f, yy2 - yy1);
            float inter_area = inter_w * inter_h;

            float area_j = (input_boxes[j].x2 - input_boxes[j].x1) * (input_boxes[j].y2 - input_boxes[j].y1);
            float iou = inter_area / (area_i + area_j - inter_area + 1e-6f);

            if (iou > nms_thresh) {
                is_suppressed[j] = true;
            }
        }
    }
}

std::vector<Detection> YOLOv8TRT::detect(const cv::Mat& bgr_img, float conf_thresh, float nms_thresh) {
    if (bgr_img.empty() || !context_) return {};

    float scale = 1.0f;
    int pad_x = 0, pad_y = 0;
    cv::Mat pre_img;
    preprocess(bgr_img, pre_img, scale, pad_x, pad_y);

    // BGR -> RGB & 归一化并转为 NCHW
    int channel_size = input_w_ * input_h_;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < input_h_; ++r) {
            const uchar* ptr = pre_img.ptr<uchar>(r);
            for (int col = 0; col < input_w_; ++col) {
                // c=0 -> R, c=1 -> G, c=2 -> B
                int bgr_c = 2 - c;
                input_buf_host_[c * channel_size + r * input_w_ + col] = ptr[col * 3 + bgr_c] / 255.0f;
            }
        }
    }

    cudaMemcpy(input_buf_dev_, input_buf_host_.data(), input_size_, cudaMemcpyHostToDevice);

    void* bindings[2];
    bindings[input_index_] = input_buf_dev_;
    bindings[output_index_] = output_buf_dev_;

    context_->executeV2(bindings);

    cudaMemcpy(output_buf_host_.data(), output_buf_dev_, output_size_, cudaMemcpyDeviceToHost);

    std::vector<Detection> candidates;
    for (int i = 0; i < num_anchors_; ++i) {
        float max_score = 0.0f;
        int max_cls = -1;
        for (int c = 0; c < num_classes_; ++c) {
            float score = output_buf_host_[(4 + c) * num_anchors_ + i];
            if (score > max_score) {
                max_score = score;
                max_cls = c;
            }
        }

        if (max_score >= conf_thresh) {
            float cx = output_buf_host_[0 * num_anchors_ + i];
            float cy = output_buf_host_[1 * num_anchors_ + i];
            float w = output_buf_host_[2 * num_anchors_ + i];
            float h = output_buf_host_[3 * num_anchors_ + i];

            float x1 = (cx - w * 0.5f - pad_x) / scale;
            float y1 = (cy - h * 0.5f - pad_y) / scale;
            float x2 = (cx + w * 0.5f - pad_x) / scale;
            float y2 = (cy + h * 0.5f - pad_y) / scale;

            x1 = std::max(0.0f, std::min((float)bgr_img.cols, x1));
            y1 = std::max(0.0f, std::min((float)bgr_img.rows, y1));
            x2 = std::max(0.0f, std::min((float)bgr_img.cols, x2));
            y2 = std::max(0.0f, std::min((float)bgr_img.rows, y2));

            Detection det;
            det.class_id = max_cls;
            det.confidence = max_score;
            det.x1 = x1;
            det.y1 = y1;
            det.x2 = x2;
            det.y2 = y2;
            if (max_cls < (int)class_names_.size()) {
                det.class_name = class_names_[max_cls];
            } else {
                det.class_name = std::to_string(max_cls);
            }
            candidates.push_back(det);
        }
    }

    std::vector<Detection> results;
    nms(candidates, nms_thresh, results);
    return results;
}
