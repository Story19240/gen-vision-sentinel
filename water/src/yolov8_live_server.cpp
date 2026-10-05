#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include "yolov8_trt.h"
#include "env_utils.h"

// 全局图像缓冲区，用于供 Web 服务器读取最新带框画面的 JPEG 字节
std::mutex g_frame_mutex;
std::vector<uchar> g_jpeg_buffer;
std::atomic<bool> g_running(true);
std::atomic<int> g_fps(0);

// 全自动枚举并打印系统所有有效网卡的访问 URL
void print_available_urls(int port) {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        std::cout << "  访问地址: http://0.0.0.0:" << port << std::endl;
        return;
    }

    std::cout << "\n========================================================" << std::endl;
    std::cout << "  >>> Web 实时视频监控服务已就绪! <<<" << std::endl;
    std::cout << "  在同局域网的电脑/手机浏览器中，打开以下任一地址即可：" << std::endl;

    bool found = false;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) { // 仅提取 IPv4
            char host[NI_MAXHOST];
            sockaddr_in* pAddr = (sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &(pAddr->sin_addr), host, NI_MAXHOST);
            std::string ifname = ifa->ifa_name;
            if (ifname != "lo") { // 排除回环 127.0.0.1
                found = true;
                std::string desc = ifname;
                if (ifname.find("eth") != std::string::npos) desc += " (工业有线网口)";
                else if (ifname.find("usb") != std::string::npos || ifname.find("l4tbr") != std::string::npos) desc += " (USB 调试通道)";
                else if (ifname.find("wlan") != std::string::npos) desc += " (无线 Wi-Fi)";
                
                std::cout << "    * [" << desc << "]  ->  http://" << host << ":" << port << std::endl;
            }
        }
    }
    if (!found) {
        std::cout << "    * [默认地址]  ->  http://127.0.0.1:" << port << std::endl;
    }
    std::cout << "========================================================\n" << std::endl;
    freeifaddrs(ifaddr);
}

// Web MJPEG 推流服务线程（监听指定端口）
void http_server_thread(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[Web Server] 无法创建 socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 自动监听所有网卡
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[Web Server] 绑定端口 " << port << " 失败" << std::endl;
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "[Web Server] 监听失败" << std::endl;
        close(server_fd);
        return;
    }

    // 自动扫描并打印各网卡真实访问地址
    print_available_urls(port);

    while (g_running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        // 启动独立线程向该客户端推送 MJPEG 流
        std::thread([client_fd]() {
            char req[1024];
            int n = recv(client_fd, req, sizeof(req) - 1, 0);
            if (n <= 0) {
                close(client_fd);
                return;
            }

            // 发送 MJPEG HTTP 响应头
            std::string header = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                "Connection: close\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Expires: 0\r\n\r\n";
            send(client_fd, header.c_str(), header.length(), 0);

            while (g_running) {
                std::vector<uchar> buf;
                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    buf = g_jpeg_buffer;
                }

                if (!buf.empty()) {
                    std::string part_header = 
                        "--frame\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: " + std::to_string(buf.size()) + "\r\n\r\n";
                    
                    if (send(client_fd, part_header.c_str(), part_header.length(), MSG_NOSIGNAL) <= 0) break;
                    if (send(client_fd, (char*)buf.data(), buf.size(), MSG_NOSIGNAL) <= 0) break;
                    if (send(client_fd, "\r\n", 2, MSG_NOSIGNAL) <= 0) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 约 30 FPS
            }
            close(client_fd);
        }).detach();
    }
    close(server_fd);
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    std::string engine_path = "/data/models/yolov8n_fp16.engine";
    std::string labels_path = "/data/workspace/yolov8_trt_rtsp/labels.txt";
    std::string rtsp_url = env_or("RTSP_URL", "rtsp://username:password@camera-host:554/path");

    std::cout << "[Init] 加载 YOLOv8 TensorRT 引擎: " << engine_path << std::endl;
    YOLOv8TRT detector(engine_path, labels_path);
    if (!detector.init()) {
        std::cerr << "初始化检测器失败!" << std::endl;
        return -1;
    }

    // 启动 Web 服务后台线程
    int port = 8080;
    std::thread web_thread(http_server_thread, port);
    web_thread.detach();

    // 启动硬件加速 GStreamer 拉流 Pipeline
    std::string pipe_str = "rtspsrc location=" + rtsp_url + 
                           " protocols=tcp latency=100 ! rtph265depay ! nvv4l2decoder ! nvvidconv ! "
                           "video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink max-buffers=1 drop=true";

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipe_str.c_str(), &error);
    if (!pipeline || error) {
        std::cerr << "GStreamer 启动失败: " << (error ? error->message : "未知错误") << std::endl;
        return -1;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::cout << "[Live] 正在从海康摄像头拉流并启动 YOLOv8 实时推理..." << std::endl;

    int frame_count = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    while (g_running) {
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        GstCaps* caps = gst_sample_get_caps(sample);
        GstStructure* s = gst_caps_get_structure(caps, 0);
        int width = 0, height = 0;
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);

        cv::Mat raw_frame(height, width, CV_8UC3, (char*)map.data);
        cv::Mat frame = raw_frame.clone();

        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);

        // 执行 TensorRT 推理
        auto t1 = std::chrono::high_resolution_clock::now();
        std::vector<Detection> results = detector.detect(frame, 0.25f, 0.45f);
        auto t2 = std::chrono::high_resolution_clock::now();
        float ms = std::chrono::duration<float, std::milli>(t2 - t1).count();

        frame_count++;
        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start_time).count();
        if (elapsed >= 1.0f) {
            g_fps = (int)(frame_count / elapsed);
            frame_count = 0;
            start_time = now;
        }

        // 绘制检测框和工业 OSD 看板
        cv::rectangle(frame, cv::Point(20, 20), cv::Point(600, 110), cv::Scalar(0, 0, 0), -1);
        cv::rectangle(frame, cv::Point(20, 20), cv::Point(600, 110), cv::Scalar(0, 255, 0), 2);
        std::string osd_title = "[REAL-TIME INFERENCE] JETSON AGX XAVIER";
        std::string osd_info = "FPS: " + std::to_string(g_fps.load()) + " | Inference: " + std::to_string((int)ms) + "ms | Targets: " + std::to_string(results.size());
        cv::putText(frame, osd_title, cv::Point(35, 55), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, osd_info, cv::Point(35, 90), cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 255, 255), 2);

        for (const auto& d : results) {
            cv::Rect box(cv::Point(d.x1, d.y1), cv::Point(d.x2, d.y2));
            cv::rectangle(frame, box, cv::Scalar(0, 255, 0), 2);

            std::string label = d.class_name + " " + std::to_string((int)(d.confidence * 100)) + "%";
            int baseLine = 0;
            cv::Size label_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &baseLine);
            float top_y = std::max(0.0f, d.y1 - label_size.height - 8);
            cv::rectangle(frame, cv::Rect(d.x1, top_y, label_size.width + 8, label_size.height + 8), cv::Scalar(0, 255, 0), -1);
            cv::putText(frame, label, cv::Point(d.x1 + 4, top_y + label_size.height + 1), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 2);
        }

        // 编码为 JPEG 并更新共享内存
        std::vector<uchar> jpeg_tmp;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
        cv::imencode(".jpg", frame, jpeg_tmp, params);

        {
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_jpeg_buffer = std::move(jpeg_tmp);
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return 0;
}
