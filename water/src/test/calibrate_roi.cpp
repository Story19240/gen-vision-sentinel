#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

// 单根管道标定结构
struct PipeBox {
    int id;
    std::string name;
    cv::Rect roi;
    float threshold;
};

static bool g_drawing = false;
static cv::Point g_pt1, g_pt2;
static cv::Rect g_temp_roi;

static void on_mouse(int event, int x, int y, int flags, void* userdata) {
    if (event == cv::EVENT_LBUTTONDOWN) {
        g_drawing = true;
        g_pt1 = cv::Point(x, y);
        g_pt2 = g_pt1;
        g_temp_roi = cv::Rect();
    } else if (event == cv::EVENT_MOUSEMOVE) {
        if (g_drawing) {
            g_pt2 = cv::Point(x, y);
            int x1 = std::min(g_pt1.x, g_pt2.x);
            int y1 = std::min(g_pt1.y, g_pt2.y);
            int w = std::abs(g_pt1.x - g_pt2.x);
            int h = std::abs(g_pt1.y - g_pt2.y);
            g_temp_roi = cv::Rect(x1, y1, w, h);
        }
    } else if (event == cv::EVENT_LBUTTONUP) {
        g_drawing = false;
        g_pt2 = cv::Point(x, y);
        int x1 = std::min(g_pt1.x, g_pt2.x);
        int y1 = std::min(g_pt1.y, g_pt2.y);
        int w = std::abs(g_pt1.x - g_pt2.x);
        int h = std::abs(g_pt1.y - g_pt2.y);
        if (w > 10 && h > 10) {
            g_temp_roi = cv::Rect(x1, y1, w, h);
            std::cout << "\n[鼠标画框] 已框选新区域: [" << x1 << ", " << y1 << ", " << (x1 + w) << ", " << (y1 + h) << "]" << std::endl;
            std::cout << "  👉 请按键盘数字键 [1 ~ 5] 绑定给对应管道；或按其他键取消" << std::endl;
        }
    }
}

int main(int argc, char** argv) {
    std::cout << "==========================================================" << std::endl;
    std::cout << "  发电机冷却水管口坐标交互式标定工具 (纯 C++ 原生实现)    " << std::endl;
    std::cout << "==========================================================" << std::endl;
    std::cout << "  操作指南：" << std::endl;
    std::cout << "    * [空格键]    : 暂停 / 播放 (暂停时方便用鼠标画框)" << std::endl;
    std::cout << "    * [鼠标左键]  : 在管口水流处拖拽画框" << std::endl;
    std::cout << "    * [数字 1 ~ 5]: 将刚画的框分配给 Pipe 1 ~ 5" << std::endl;
    std::cout << "    * [按键 S]    : 将当前 5 个坐标保存至 water_config.json" << std::endl;
    std::cout << "    * [按键 Q/ESC]: 导出标注效果图并退出" << std::endl;
    std::cout << "==========================================================" << std::endl;

    std::string video_path = (argc > 1) ? argv[1] : "../../videos/water.mp4";
    bool is_live_stream = (video_path.find("rtsp://") == 0 || video_path.find("http://") == 0 || video_path.find("https://") == 0);

    std::cout << "[输入源] " << (is_live_stream ? "网络实时摄像头流 (RTSP/HTTP)" : "本地视频文件") << ": " << video_path << std::endl;

    // 智能定位配置文件路径
    std::string config_path = "../../water_config.json";
    if (std::ifstream("../water_config.json").good()) config_path = "../water_config.json";
    else if (std::ifstream("water_config.json").good()) config_path = "water_config.json";

    cv::VideoCapture cap;
    if (is_live_stream) {
        // 使用 FFMPEG / 低延迟参数打开 RTSP 网络摄像头
        cap.open(video_path, cv::CAP_FFMPEG);
    } else {
        cap.open(video_path);
    }

    if (!cap.isOpened()) {
        std::cerr << "❌ 无法打开视频源: " << video_path << std::endl;
        std::cerr << "👉 提示: 若为 RTSP 摄像头，请检查 IP/端口/账号密码是否正确，网络是否可达。" << std::endl;
        return -1;
    }

    std::vector<PipeBox> pipes = {
        {1, "左侧主粗管(大流量)", cv::Rect(390, 580, 160, 170), 4.0f},
        {2, "中间细黄管(直流水)", cv::Rect(550, 500, 50, 100), 3.5f},
        {3, "中偏右银管(散流水)", cv::Rect(650, 530, 80, 70), 3.5f},
        {4, "右侧弯管口(直流水)", cv::Rect(720, 640, 60, 60), 3.5f},
        {5, "最右顺壁管(贴壁细流)", cv::Rect(790, 600, 40, 100), 2.5f}
    };

    std::string win_name = "Pipe ROI Calibration (Pure C++ Native)";
    cv::namedWindow(win_name, cv::WINDOW_NORMAL);
    cv::resizeWindow(win_name, 540, 960);
    cv::setMouseCallback(win_name, on_mouse, nullptr);

    bool paused = true; // 初始默认暂停，方便直接核对画框
    cv::Mat frame, display;

    // 读取首帧
    for (int retry = 0; retry < 10; ++retry) {
        if (cap.read(frame) && !frame.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (frame.empty()) {
        std::cerr << "❌ 视频源首帧读取失败!" << std::endl;
        return -1;
    }

    while (true) {
        if (!paused) {
            if (!cap.read(frame) || frame.empty()) {
                if (!is_live_stream) {
                    cap.set(cv::CAP_PROP_POS_FRAMES, 0); // 本地文件循环播放
                    cap.read(frame);
                } else {
                    std::cout << "[警告] 网络摄像头暂时无新数据，尝试重试..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }
        } else if (is_live_stream) {
            // RTSP 暂停时在后台清空缓冲区(grab)，避免取消暂停时产生几秒的画面堆积延迟
            cap.grab();
        }

        display = frame.clone();

        // 顶部半透明标题栏
        cv::Rect top_bar(0, 0, display.cols, 110);
        cv::Mat top_roi = display(top_bar);
        cv::Mat dark(110, display.cols, CV_8UC3, cv::Scalar(15, 15, 15));
        cv::addWeighted(dark, 0.75, top_roi, 0.25, 0, top_roi);

        cv::putText(display, "PIPE ROI CALIBRATION TOOL (C++)", cv::Point(25, 35),
                    cv::FONT_HERSHEY_SIMPLEX, 0.85, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

        std::string mode_str = paused ? "PAUSED: DRAG MOUSE TO DRAW NEW ROI" : "PLAYING VIDEO";
        cv::putText(display, "STATUS: " + mode_str, cv::Point(25, 68),
                    cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 220, 255), 2, cv::LINE_AA);

        cv::putText(display, "KEYS: SPACE=Pause | 1-5=Bind Pipe | S=Save Config | Q=Quit", cv::Point(25, 98),
                    cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);

        // 绘制 5 根管道现有框
        for (const auto& p : pipes) {
            cv::rectangle(display, p.roi, cv::Scalar(0, 230, 0), 2);

            std::stringstream ss;
            ss << "Pipe " << p.id << " [" << p.roi.x << "," << p.roi.y << "," 
               << (p.roi.x + p.roi.width) << "," << (p.roi.y + p.roi.height) << "]";

            int baseLine = 0;
            cv::Size t_size = cv::getTextSize(ss.str(), cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &baseLine);
            int ty = std::max(130, p.roi.y - 8);
            cv::rectangle(display, cv::Rect(p.roi.x, ty - t_size.height - 4, t_size.width + 8, t_size.height + 6), cv::Scalar(0, 230, 0), -1);
            cv::putText(display, ss.str(), cv::Point(p.roi.x + 4, ty), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
        }

        // 绘制正在拖拽中的新框
        if (g_temp_roi.width > 0 && g_temp_roi.height > 0) {
            cv::rectangle(display, g_temp_roi, cv::Scalar(0, 255, 255), 2);
            std::string temp_lbl = "New Box: [" + std::to_string(g_temp_roi.x) + "," + std::to_string(g_temp_roi.y) + "] Press 1~5";
            cv::putText(display, temp_lbl, cv::Point(g_temp_roi.x, std::max(130, g_temp_roi.y - 10)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
        }

        cv::imshow(win_name, display);

        int key = cv::waitKey(paused ? 50 : 30) & 0xFF;
        if (key == 'q' || key == 'Q' || key == 27) {
            cv::imwrite("../images/water_calibration_result.jpg", display);
            std::cout << "\n[标定退出] 当前带框标定图已保存至: ../images/water_calibration_result.jpg" << std::endl;
            break;
        } else if (key == 32) { // 空格键
            paused = !paused;
            std::cout << "[状态切换] " << (paused ? "已暂停视频" : "继续播放视频") << std::endl;
        } else if (key >= '1' && key <= '5') {
            int target_id = key - '0';
            if (g_temp_roi.width > 0 && g_temp_roi.height > 0) {
                for (auto& p : pipes) {
                    if (p.id == target_id) {
                        p.roi = g_temp_roi;
                        std::cout << "🎯 成功将 Pipe " << target_id << " (" << p.name << ") 更新为新坐标: ["
                                  << p.roi.x << ", " << p.roi.y << ", " << (p.roi.x + p.roi.width) << ", " << (p.roi.y + p.roi.height) << "]" << std::endl;
                        std::cout << "👉 请按 [S] 键保存到配置文件！" << std::endl;
                        g_temp_roi = cv::Rect();
                        break;
                    }
                }
            } else {
                std::cout << "[提示] 请先用鼠标在画面上拖拽拉出一个框，然后再按数字键 " << target_id << " 绑定！" << std::endl;
            }
        } else if (key == 's' || key == 'S') {
            // 将更新后的坐标写回 JSON 格式
            std::ofstream out(config_path);
            if (out.is_open()) {
                out << "{\n  \"device_name\": \"1号发电机机组 - 冷却水智能视觉防护系统\",\n";
                out << "  \"web_port\": 8080,\n  \"alarm_duration_sec\": 2.0,\n  \"recovery_duration_sec\": 2.0,\n";
                out << "  \"pipes\": [\n";
                for (size_t i = 0; i < pipes.size(); ++i) {
                    const auto& p = pipes[i];
                    out << "    {\n";
                    out << "      \"id\": " << p.id << ",\n";
                    out << "      \"name\": \"" << p.name << "\",\n";
                    out << "      \"roi\": [" << p.roi.x << ", " << p.roi.y << ", " 
                        << (p.roi.x + p.roi.width) << ", " << (p.roi.y + p.roi.height) << "],\n";
                    out << "      \"threshold\": " << p.threshold << ",\n";
                    out << "      \"enabled\": true\n";
                    out << "    }" << (i + 1 < pipes.size() ? "," : "") << "\n";
                }
                out << "  ]\n}\n";
                out.close();
                std::cout << "\n✅ 最新 5 根管口坐标已成功写入 ../water_config.json！" << std::endl;
            }
        }
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}
