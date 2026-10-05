#include <iostream>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include "../env_utils.h"

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    std::string rtsp_url = env_or("RTSP_URL", "rtsp://username:password@camera-host:554/path");
    std::string pipe_str = "rtspsrc location=" + rtsp_url + " protocols=tcp latency=100 ! rtph264depay ! nvv4l2decoder ! nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink max-buffers=1 drop=true";

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipe_str.c_str(), &error);
    if (!pipeline || error) {
        std::cerr << "Pipeline 启动失败: " << (error ? error->message : "未知错误") << std::endl;
        return -1;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::cout << "正在连接海康 RTSP 摄像头并拉流..." << std::endl;

    for (int i = 0; i < 5; ++i) {
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (sample) {
            GstCaps* caps = gst_sample_get_caps(sample);
            GstStructure* s = gst_caps_get_structure(caps, 0);
            int width = 0, height = 0;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            gst_buffer_map(buffer, &map, GST_MAP_READ);

            cv::Mat frame(height, width, CV_8UC3, (char*)map.data);
            std::cout << "获取第 " << (i + 1) << " 帧成功! 尺寸: " << width << "x" << height << ", 字节数: " << map.size << std::endl;

            if (i == 4) {
                cv::imwrite("/data/workspace/yolov8_trt_rtsp/live_frame.jpg", frame);
                std::cout << "第 5 帧已保存至 live_frame.jpg" << std::endl;
            }

            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
        } else {
            std::cerr << "获取 sample 失败" << std::endl;
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return 0;
}
