#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "water_flow_detector.h"
#include "feishu_client.h"
#include "env_utils.h"

// 全局运行控制与图像缓冲区
std::atomic<bool> g_running(true);
std::mutex g_frame_mutex;
std::vector<uchar> g_jpeg_buffer;
std::atomic<int> g_fps(0);

void signal_handler(int sig) {
    std::cout << "\n[系统信号] 收到退出信号 (" << sig << ")，正在平稳停止所有服务..." << std::endl;
    g_running = false;
}

// 极简高性能 HTTP/MJPEG 实时推流服务器 (8080 端口)
void run_http_mjpeg_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[Web推流] 创建 Socket 失败!" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[Web推流] 端口绑定失败 (端口 " << port << " 可能被占用)!" << std::endl;
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        close(server_fd);
        return;
    }

    std::cout << "[Web推流] MJPEG 实时推流服务器已就绪! 端口: " << port << std::endl;
    std::cout << "  在浏览器中打开: http://127.0.0.1:" << port << " 或开发板局域网 IP 查看实时监控大屏！" << std::endl;

    while (g_running) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        // 启动独立客户端连接处理线程
        std::thread([client_fd]() {
            char header_buf[1024];
            int h_len = snprintf(header_buf, sizeof(header_buf),
                "HTTP/1.0 200 OK\r\n"
                "Server: WaterFlowMonitor/1.0\r\n"
                "Connection: close\r\n"
                "Max-Age: 0\r\n"
                "Expires: 0\r\n"
                "Cache-Control: no-cache, private\r\n"
                "Pragma: no-cache\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=--myboundary\r\n\r\n");
            
            if (send(client_fd, header_buf, h_len, MSG_NOSIGNAL) < 0) {
                close(client_fd);
                return;
            }

            while (g_running) {
                std::vector<uchar> local_jpeg;
                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    local_jpeg = g_jpeg_buffer;
                }

                if (!local_jpeg.empty()) {
                    char frame_header[256];
                    int f_len = snprintf(frame_header, sizeof(frame_header),
                        "--myboundary\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %zu\r\n\r\n", local_jpeg.size());

                    if (send(client_fd, frame_header, f_len, MSG_NOSIGNAL) < 0) break;
                    if (send(client_fd, (const char*)local_jpeg.data(), local_jpeg.size(), MSG_NOSIGNAL) < 0) break;
                    if (send(client_fd, "\r\n", 2, MSG_NOSIGNAL) < 0) break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(40)); // 约 25 FPS 推流
            }

            close(client_fd);
        }).detach();
    }

    close(server_fd);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "==========================================================" << std::endl;
    std::cout << "  发电机冷却水 6 路独立断水监测系统 (纯 C++ 工业级引擎) " << std::endl;
    std::cout << "==========================================================" << std::endl;

    std::string config_path = "../water_config.json";
    std::string video_src = "../videos/water.mp4"; // 默认加载实测视频
    bool demo_simulate_cutoff = true; // 默认开启断水注入演习

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--rtsp") {
            video_src = env_or("RTSP_URL", "rtsp://username:password@camera-host:554/path");
            demo_simulate_cutoff = false;
        } else if (arg == "--no-sim") {
            demo_simulate_cutoff = false;
        } else if (arg.find(".mp4") != std::string::npos) {
            video_src = arg;
        } else if (arg.find(".json") != std::string::npos) {
            config_path = arg;
        }
    }

    // 1. 初始化纯 C++ 飞书告警客户端
    auto feishu = std::make_shared<FeishuClient>();
    feishu->init(env_or("FEISHU_APP_ID"),
                 env_or("FEISHU_APP_SECRET"),
                 env_or("FEISHU_RECEIVE_ID"),
                 env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id"),
                 env_or("FEISHU_WEB_MONITOR_URL", "http://192.168.55.1:8080"));

    // 2. 初始化纯 C++ 水流断流检测核心
    WaterFlowDetector detector;
    if (!detector.init(config_path)) {
        std::cerr << "初始化水流检测器失败: " << config_path << std::endl;
        return -1;
    }
    detector.set_feishu_client(feishu);

    // 3. 打开视频流
    std::cout << "正在打开视频输入源: " << video_src << std::endl;
    cv::VideoCapture cap;
    if (video_src.find("rtsp://") == 0) {
        // Jetson GStreamer H.265 / H.264 硬件硬解码管线
        std::string gst_pipe = "rtspsrc location=" + video_src + " protocols=tcp latency=100 ! "
                               "rtph265depay ! nvv4l2decoder ! nvvidconv ! video/x-raw,format=BGRx ! "
                               "videoconvert ! video/x-raw,format=BGR ! appsink drop=1";
        cap.open(gst_pipe, cv::CAP_GSTREAMER);
    } else {
        cap.open(video_src);
    }

    if (!cap.isOpened()) {
        std::cerr << "无法打开视频输入源: " << video_src << std::endl;
        return -1;
    }

    // 4. 启动后台 Web 实时推流服务 (8080 端口)
    std::thread http_thread(run_http_mjpeg_server, 8080);
    http_thread.detach();

    std::cout << "\n>>> 水流监测主循环启动中... 按 Ctrl+C 退出 <<<\n" << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    auto last_fps_time = std::chrono::steady_clock::now();

    cv::Mat frame;
    while (g_running) {
        if (!cap.read(frame) || frame.empty()) {
            if (video_src.find("rtsp://") != 0) {
                // 本地视频循环播放
                cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                continue;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed_sec = std::chrono::duration<double>(now - start_time).count();

        // 模拟断水注入演习 (用于零现场数据测试)：
        // 比如在播放第 3 秒 ~ 6 秒期间，人工注入 2 号水管断流
        if (demo_simulate_cutoff) {
            double cycle = fmod(elapsed_sec, 12.0); // 12秒一个循环
            if (cycle >= 3.0 && cycle < 7.0) {
                detector.simulate_pipe_cutoff(2, true);  // 模拟切断 2 号管
            } else {
                detector.simulate_pipe_cutoff(2, false); // 模拟恢复 2 号管
            }
        }

        // 核心步骤 1: 毫秒级动能计算与三级状态机更新
        detector.process_frame(frame, elapsed_sec);

        // 核心步骤 2: 计算实时 FPS
        frame_count++;
        auto fps_elapsed = std::chrono::duration<double>(now - last_fps_time).count();
        if (fps_elapsed >= 1.0) {
            g_fps = static_cast<int>(frame_count / fps_elapsed);
            frame_count = 0;
            last_fps_time = now;
        }

        // 核心步骤 3: 绘制管口彩色状态框与顶部工业看板
        cv::Mat display = frame.clone();
        detector.draw_overlay(display, static_cast<float>(g_fps));

        // 核心步骤 4: 极速 JPEG 编码送入 Web 广播缓冲区
        std::vector<uchar> buf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
        cv::imencode(".jpg", display, buf, params);

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_jpeg_buffer = std::move(buf);
        }

        // 控制回放速率 (约 30 FPS)
        if (video_src.find("rtsp://") != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    cap.release();
    std::cout << "[主服务] 水流监测服务已安全退出。" << std::endl;
    return 0;
}
