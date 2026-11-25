#include "monitor_service.h"
#include "offline_io.hpp"
#include "synthesize.hpp"
#include "../../water/src/feishu_client.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

void set_member(
    spark::JsonValue& object,
    const std::string& key,
    spark::JsonValue value) {
    if (spark::JsonValue* current = object.find(key)) {
        *current = std::move(value);
    } else {
        object.as_object().emplace_back(key, std::move(value));
    }
}

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

// The relay integration test does not make network calls to Feishu. These
// stubs satisfy MonitorService's linker references while notifications are
// disabled in the generated test configuration.
FeishuClient::FeishuClient() = default;
FeishuClient::~FeishuClient() = default;
void FeishuClient::init(
    const std::string&,
    const std::string&,
    const std::string&,
    const std::string&,
    const std::string&) {}
void FeishuClient::send_card_async(
    const std::string&,
    const std::string&,
    const std::vector<FeishuField>&) {}

int main(int argc, char** argv) {
    try {
        if (argc < 5 || argc > 7) {
            std::cerr << "Usage: test_monitor_relay EXAMPLE_CONFIG RECORDINGS_DIR RELAY_PORT SEED [HOST] [HOLD_MS]\n";
            return 2;
        }
        const std::string recordings_dir = argv[2];
        const int relay_port = std::stoi(argv[3]);
        const std::int64_t seed = std::stoll(argv[4]);
        const std::string relay_host = argc >= 6 ? argv[5] : "127.0.0.1";
        const int hold_ms = argc >= 7 ? std::stoi(argv[6]) : 300;
        expect(hold_ms >= 300 && hold_ms <= 10000, "hold time must be 300~10000 ms");
        spark::ensure_output_directory(recordings_dir);

        spark::JsonValue config = spark::load_json_file(argv[1]);
        set_member(config, "calibration", spark::JsonValue::object({
            spark::JsonValue::Member("complete", spark::JsonValue::boolean(false)),
            spark::JsonValue::Member("frame_width", spark::JsonValue::integer(640)),
            spark::JsonValue::Member("frame_height", spark::JsonValue::integer(480))}));
        set_member(config, "relay_channel", spark::JsonValue::integer(1));
        set_member(config, "relay_trip_enabled", spark::JsonValue::boolean(true));
        set_member(config, "relay", spark::JsonValue::object({
            spark::JsonValue::Member("host", spark::JsonValue::string(relay_host)),
            spark::JsonValue::Member("port", spark::JsonValue::integer(relay_port))}));
        set_member(config, "feishu", spark::JsonValue::object({
            spark::JsonValue::Member("enabled", spark::JsonValue::boolean(false))}));
        set_member(config, "record_pre_sec", spark::JsonValue::number(0.2));
        set_member(config, "record_post_sec", spark::JsonValue::number(0.0));

        const std::string config_path = recordings_dir + "/relay_test_config.json";
        spark::write_json_file(config_path, config);
        spark::MonitorOptions options;
        options.config_path = config_path;
        options.recordings_dir = recordings_dir + "/events";
        options.web_port = 18081;
        spark::MonitorService service(options);
        service.initialize();

        constexpr double fps = 25.0;
        spark::SyntheticSequence sequence = spark::make_sequence(
            "blue_white_spark", 60, fps, seed);
        auto process = [&](spark::SyntheticFrame& frame, std::int64_t timestamp_ms) {
            cv::Mat view(
                frame.height, frame.width, CV_8UC3, frame.bgr.data());
            std::vector<uchar> jpeg;
            expect(cv::imencode(".jpg", view, jpeg), "JPEG encoding failed");
            return service.process_video_frame(view, jpeg, timestamp_ms, fps, fps);
        };

        process(sequence.frames.front(), 0);
        const std::string started = service.handle_api("/api/start_monitoring", "");
        expect(started.find("\"status\":\"ok\"") != std::string::npos,
            "failed to start monitoring: " + started);

        int event_count = 0;
        for (std::size_t index = 0; index < sequence.frames.size(); ++index) {
            const auto result = process(
                sequence.frames[index],
                static_cast<std::int64_t>(index * 1000.0 / fps));
            if (result.event_triggered) ++event_count;
        }
        expect(event_count == 1, "expected exactly one confirmed spark event");
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

        std::string status = service.status_json();
        expect(status.find("\"relay_connected\":true") != std::string::npos,
            "relay TCP client did not connect");
        expect(status.find("\"relay_energized\":true") != std::string::npos,
            "confirmed spark did not request Y1 energization");

        const std::string reset = service.handle_api("/api/relay_reset", "");
        expect(reset.find("\"status\":\"ok\"") != std::string::npos,
            "relay reset API failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        status = service.status_json();
        expect(status.find("\"relay_energized\":false") != std::string::npos,
            "relay reset did not clear the commanded state");

        const std::string disabled = service.handle_api(
            "/api/update_relay", "enabled=0");
        expect(disabled.find("\"status\":\"ok\"") != std::string::npos,
            "relay disable API failed");
        expect(service.status_json().find("\"relay_trip_enabled\":false") != std::string::npos,
            "relay disable did not update service state");

        service.shutdown();
        std::cout << "EVENT_COUNT=" << event_count << "\nMONITOR_RELAY_TEST_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MONITOR_RELAY_TEST_FAILED: " << error.what() << "\n";
        return 1;
    }
}
