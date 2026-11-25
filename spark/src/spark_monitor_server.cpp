#include "monitor_service.h"
#include "../../water/src/env_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_running(true);
std::mutex g_jpeg_mutex;
std::vector<uchar> g_jpeg;
std::shared_ptr<spark::MonitorService> g_service;

struct ServerOptions {
    spark::MonitorOptions service;
    std::string input;
    std::string rtsp_url;
    bool once = false;
    bool no_delay = false;
};

void signal_handler(int) {
    g_running = false;
}

void print_usage() {
    std::cout <<
        "Usage: spark_monitor_server [--config FILE] [--port 8081]\n"
        "  [--recordings DIR] [--input VIDEO] [--rtsp URL]\n"
        "  [--once] [--no-delay]\n";
}

ServerOptions parse_options(int argc, char** argv) {
    ServerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&]() -> std::string {
            if (index + 1 >= argc) throw std::invalid_argument("missing value for " + argument);
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            print_usage();
            std::exit(0);
        } else if (argument == "--config") {
            options.service.config_path = next();
        } else if (argument == "--recordings") {
            options.service.recordings_dir = next();
        } else if (argument == "--port") {
            options.service.web_port = std::stoi(next());
        } else if (argument == "--input") {
            options.input = next();
        } else if (argument == "--rtsp") {
            options.rtsp_url = next();
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--no-delay") {
            options.no_delay = true;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.service.web_port < 1 || options.service.web_port > 65535) {
        throw std::invalid_argument("invalid web port");
    }
    return options;
}

bool send_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = send(fd, data, size, MSG_NOSIGNAL);
        if (written <= 0) return false;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool send_all(int fd, const std::string& data) {
    return send_all(fd, data.data(), data.size());
}

void send_response(
    int fd,
    const std::string& status,
    const std::string& content_type,
    const std::string& body) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\nContent-Type: " << content_type
           << "\r\nContent-Length: " << body.size()
           << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(fd, header.str());
    send_all(fd, body);
    close(fd);
}

std::string html_document() {
    char executable[4096] = {0};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    std::string directory = "spark/src/bin";
    if (length > 0) {
        std::string path(executable, static_cast<std::size_t>(length));
        directory = path.substr(0, path.find_last_of('/'));
    }
    const std::vector<std::string> paths = {
        "spark/src/monitor_studio.html",
        "monitor_studio.html",
        "../monitor_studio.html",
        directory + "/../monitor_studio.html"};
    for (const std::string& path : paths) {
        std::ifstream input(path);
        if (!input.good()) continue;
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }
    return "<!doctype html><title>Spark monitor</title><h1>monitor_studio.html not found</h1>";
}

std::string content_type(const std::string& path) {
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg") return "image/jpeg";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".png") return "image/png";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".mp4") return "video/mp4";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") return "application/json";
    return "application/octet-stream";
}

void http_client(int client_fd) {
    try {
        timeval timeout{};
        timeout.tv_sec = 4;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[8192] = {0};
        const int count = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (count <= 0) {
            close(client_fd);
            return;
        }
        std::istringstream request(std::string(buffer, static_cast<std::size_t>(count)));
        std::string method;
        std::string target;
        request >> method >> target;
        const std::size_t question = target.find('?');
        const std::string path = target.substr(0, question);
        const std::string query = question == std::string::npos ? "" : target.substr(question + 1);

        if (path.find("/api/") == 0) {
            send_response(client_fd, "200 OK", "application/json; charset=utf-8",
                g_service->handle_api(path, query));
            return;
        }
        if (path == "/" || path == "/index.html" || path == "/calibrate") {
            send_response(client_fd, "200 OK", "text/html; charset=utf-8", html_document());
            return;
        }
        if (path.find("/recordings/") == 0) {
            const std::string name = path.substr(std::strlen("/recordings/"));
            if (name.empty() || name.find("..") != std::string::npos ||
                name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                send_response(client_fd, "400 Bad Request", "text/plain", "invalid recording name");
                return;
            }
            const std::string file = g_service->recordings_dir() + "/" + name;
            std::ifstream input(file, std::ios::binary);
            if (!input.good()) {
                send_response(client_fd, "404 Not Found", "text/plain", "recording not found");
                return;
            }
            std::ostringstream content;
            content << input.rdbuf();
            send_response(client_fd, "200 OK", content_type(name), content.str());
            return;
        }
        if (path != "/video_feed") {
            send_response(client_fd, "404 Not Found", "text/plain", "not found");
            return;
        }

        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n";
        if (!send_all(client_fd, header)) return;
        while (g_running) {
            std::vector<uchar> jpeg;
            {
                std::lock_guard<std::mutex> lock(g_jpeg_mutex);
                jpeg = g_jpeg;
            }
            if (!jpeg.empty()) {
                const std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + std::to_string(jpeg.size()) + "\r\n\r\n";
                if (!send_all(client_fd, part)) break;
                if (!send_all(client_fd,
                        reinterpret_cast<const char*>(jpeg.data()), jpeg.size())) break;
                if (!send_all(client_fd, "\r\n", 2)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        close(client_fd);
    } catch (const std::exception& error) {
        send_response(client_fd, "500 Internal Server Error", "application/json",
            std::string("{\"status\":\"error\",\"msg\":") + spark::json_escape(error.what()) + "}");
    }
}

void http_worker(int port) {
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[spark] failed to create web socket\n";
        return;
    }
    int enabled = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(server_fd, 64) != 0) {
        std::cerr << "[spark] failed to listen on port " << port << "\n";
        close(server_fd);
        return;
    }
    std::cout << "[spark] Web Studio: http://192.168.1.100:" << port
              << " (USB: http://192.168.55.1:" << port << ")\n";
    while (g_running) {
        const int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        std::thread(http_client, client_fd).detach();
    }
    close(server_fd);
}

std::string configured_rtsp_url(const ServerOptions& options) {
    if (!options.rtsp_url.empty()) return options.rtsp_url;
    std::string url = env_or("SPARK_RTSP_URL");
    if (url.empty()) url = env_or("RTSP_URL");
    if (url.empty()) {
        std::ifstream input("/root/.rtsp_url");
        std::getline(input, url);
    }
    return url;
}

GstElement* open_rtsp_pipeline(const std::string& url, GstElement** sink) {
    const std::string description = "rtspsrc location=" + url +
        " protocols=tcp latency=100 ! rtph265depay ! nvv4l2decoder ! nvvidconv ! "
        "video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=sink max-buffers=1 drop=true";
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) g_error_free(error);
    if (!pipeline) return nullptr;
    *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!*sink || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(pipeline);
        return nullptr;
    }
    return pipeline;
}

bool pull_rtsp_frame(GstElement* sink, cv::Mat& frame) {
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
    if (!sample) return false;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstStructure* structure = gst_caps_get_structure(caps, 0);
    int width = 0;
    int height = 0;
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ) || width <= 0 || height <= 0) {
        gst_sample_unref(sample);
        return false;
    }
    cv::Mat view(height, width, CV_8UC3, reinterpret_cast<char*>(map.data));
    frame = view.clone();
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        const ServerOptions options = parse_options(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        gst_init(&argc, &argv);
        gst_debug_set_default_threshold(GST_LEVEL_NONE);

        g_service = std::make_shared<spark::MonitorService>(options.service);
        g_service->initialize();
        std::thread(http_worker, options.service.web_port).detach();

        cv::VideoCapture capture;
        GstElement* pipeline = nullptr;
        GstElement* sink = nullptr;
        double source_fps = 25.0;
        if (!options.input.empty()) {
            capture.open(options.input);
            if (!capture.isOpened()) throw spark::SparkError("failed to open input video");
            source_fps = capture.get(cv::CAP_PROP_FPS);
            if (source_fps <= 0.0 || source_fps != source_fps) source_fps = 25.0;
            std::cout << "[spark] offline input: " << options.input << "\n";
        } else {
            const std::string url = configured_rtsp_url(options);
            if (url.empty()) throw spark::SparkError("no input or RTSP URL configured");
            pipeline = open_rtsp_pipeline(url, &sink);
            if (!pipeline) throw spark::SparkError("failed to start RTSP pipeline");
            source_fps = std::stod(env_or("SPARK_FPS", "25"));
            std::cout << "[spark] RTSP source connected (credentials hidden)\n";
        }

        const auto started = std::chrono::steady_clock::now();
        auto fps_started = started;
        std::int64_t frame_index = 0;
        int fps_frames = 0;
        double measured_fps = 0.0;
        while (g_running) {
            cv::Mat frame;
            bool received = false;
            if (sink) {
                received = pull_rtsp_frame(sink, frame);
                if (!received) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
            } else {
                received = capture.read(frame) && !frame.empty();
                if (!received) {
                    g_service->finish_source();
                    if (options.once) break;
                    capture.release();
                    capture.open(options.input);
                    frame_index = 0;
                    continue;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const std::int64_t timestamp_ms = sink
                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()
                : static_cast<std::int64_t>(frame_index * 1000.0 / source_fps);
            ++frame_index;
            ++fps_frames;
            const double fps_elapsed = std::chrono::duration<double>(now - fps_started).count();
            if (fps_elapsed >= 0.5) {
                measured_fps = fps_frames / fps_elapsed;
                fps_started = now;
                fps_frames = 0;
            }
            std::vector<uchar> jpeg;
            cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85});
            {
                std::lock_guard<std::mutex> lock(g_jpeg_mutex);
                g_jpeg = jpeg;
            }
            const spark::DetectionResult result = g_service->process_video_frame(
                frame, jpeg, timestamp_ms, source_fps, measured_fps);
            if (result.event_triggered) {
                std::cout << "[spark] suspected spark: " << result.level << " / "
                          << result.color_type << " at " << timestamp_ms << " ms\n";
            }
            if (!sink && !options.no_delay) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(1000.0 / source_fps)));
            }
        }

        if (options.once) {
            g_service->finish_source();
            std::cout << "PLAYBACK_COMPLETE\n";
            while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (sink) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(sink);
            gst_object_unref(pipeline);
        }
        capture.release();
        g_service->shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[spark] " << error.what() << "\n";
        if (g_service) g_service->shutdown();
        return 2;
    }
}
