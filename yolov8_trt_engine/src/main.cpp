#include <iostream>
#include <chrono>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "yolov8_trt.h"

int main(int argc, char** argv) {
    std::cout << "======================================================" << std::endl;
    std::cout << "  YOLOv8 + TensorRT FP16 极速目标检测 (Jetson AGX)   " << std::endl;
    std::cout << "======================================================" << std::endl;

    std::string engine_path = "/data/models/yolov8n_fp16.engine";
    std::string labels_path = "/data/workspace/DeepStream-Yolo/labels.txt";
    std::string img_path = (argc > 1) ? argv[1] : "";

    YOLOv8TRT detector(engine_path, labels_path);
    if (!detector.init()) {
        std::cerr << "初始化检测器失败!" << std::endl;
        return -1;
    }

    // 准备测试图像
    cv::Mat test_img;
    if (!img_path.empty()) {
        std::cout << "正在读取测试图像: " << img_path << std::endl;
        test_img = cv::imread(img_path);
        if (test_img.empty()) {
            std::cerr << "读取图像失败: " << img_path << std::endl;
            return -1;
        }
    } else {
        std::cout << "未提供图像，生成 1920x1080 仿真测试画面进行基准测速..." << std::endl;
        test_img = cv::Mat(1080, 1920, CV_8UC3, cv::Scalar(40, 40, 40));
        cv::rectangle(test_img, cv::Rect(300, 300, 200, 400), cv::Scalar(0, 255, 0), -1);
        cv::circle(test_img, cv::Point(800, 500), 100, cv::Scalar(0, 0, 255), -1);
    }

    // 执行检测
    auto t1 = std::chrono::high_resolution_clock::now();
    std::vector<Detection> results = detector.detect(test_img, 0.25f, 0.45f);
    auto t2 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t2 - t1).count();

    std::cout << "------------------------------------------------------" << std::endl;
    std::cout << "  端到端耗时: " << ms << " ms" << std::endl;
    std::cout << "  检测到目标数量: " << results.size() << std::endl;
    std::cout << "------------------------------------------------------" << std::endl;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& d = results[i];
        std::cout << "  [" << (i + 1) << "] 类别: " << d.class_name
                  << " (id: " << d.class_id << ")"
                  << ", 置信度: " << (d.confidence * 100.0f) << "%"
                  << ", 坐标: [" << (int)d.x1 << ", " << (int)d.y1 << ", " << (int)d.x2 << ", " << (int)d.y2 << "]"
                  << std::endl;

        // 画检测框
        cv::Rect box(cv::Point(d.x1, d.y1), cv::Point(d.x2, d.y2));
        cv::rectangle(test_img, box, cv::Scalar(0, 255, 0), 3);

        // 标签文字背景
        std::string label = d.class_name + " " + std::to_string((int)(d.confidence * 100)) + "%";
        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.8, 2, &baseLine);
        float top_y = std::max(0.0f, d.y1 - label_size.height - 10);
        cv::rectangle(test_img, cv::Rect(d.x1, top_y, label_size.width + 10, label_size.height + 10), cv::Scalar(0, 255, 0), -1);
        cv::putText(test_img, label, cv::Point(d.x1 + 5, top_y + label_size.height + 3), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 0), 2);
    }

    std::string out_path = "annotated_result.jpg";
    cv::imwrite(out_path, test_img);
    std::cout << "已将画框检测结果成功保存至: " << out_path << std::endl;
    std::cout << "======================================================" << std::endl;
    return 0;
}
