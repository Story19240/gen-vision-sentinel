#include "monitor_service.h"

#include "offline_io.hpp"
#include "../../water/src/env_utils.h"
#include "../../water/src/feishu_client.h"
#include "../../water/src/relay_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>

namespace spark {
namespace {

JsonValue* member(JsonValue& object, const std::string& key) {
    return object.is_object() ? object.find(key) : nullptr;
}

const JsonValue* member(const JsonValue& object, const std::string& key) {
    return object.is_object() ? object.find(key) : nullptr;
}

void set_member(JsonValue& object, const std::string& key, JsonValue value) {
    if (JsonValue* current = member(object, key)) {
        *current = std::move(value);
    } else {
        object.as_object().emplace_back(key, std::move(value));
    }
}

std::string string_value(const JsonValue* value, const std::string& fallback = "") {
    return value && value->is_string() ? value->as_string() : fallback;
}

double number_value(const JsonValue* value, double fallback) {
    return value && value->is_number() ? value->as_double() : fallback;
}

bool bool_value(const JsonValue* value, bool fallback) {
    return value && value->is_boolean() ? value->as_bool() : fallback;
}

std::string parent_directory(const std::string& path) {
    const std::size_t position = path.find_last_of('/');
    return position == std::string::npos ? "." : path.substr(0, position);
}

JsonValue roi_json(int x1, int y1, int x2, int y2) {
    const JsonValue coordinates = JsonValue::array({
        JsonValue::integer(x1), JsonValue::integer(y1),
        JsonValue::integer(x2), JsonValue::integer(y2)});
    return JsonValue::object({
        JsonValue::Member("sample", coordinates),
        JsonValue::Member("synthetic", coordinates)});
}

std::string roi_status_json(const JsonValue& config, const char* name, int width, int height) {
    const ResolvedRoi roi = resolve_roi(member(config, name), width, height);
    if (!roi.valid) return "[0,0,0,0]";
    std::ostringstream out;
    out << '[' << roi.roi.x1 << ',' << roi.roi.y1 << ','
        << roi.roi.x2 << ',' << roi.roi.y2 << ']';
    return out.str();
}

std::string url_decode(const std::string& text) {
    std::string decoded;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            try {
                decoded += static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16));
                index += 2;
            } catch (...) {
                decoded += text[index];
            }
        } else {
            decoded += text[index] == '+' ? ' ' : text[index];
        }
    }
    return decoded;
}

std::string query_value(const std::string& query, const std::string& key) {
    const std::string prefix = key + "=";
    std::size_t position = query.find(prefix);
    if (position == std::string::npos) return "";
    position += prefix.size();
    const std::size_t end = query.find('&', position);
    return url_decode(query.substr(position, end == std::string::npos ? end : end - position));
}

int query_int(const std::string& query, const std::string& key, int fallback) {
    try {
        const std::string value = query_value(query, key);
        return value.empty() ? fallback : std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::string event_time_name(std::string& readable) {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&raw, &local);
    char base[32];
    char text[64];
    std::strftime(base, sizeof(base), "%Y%m%d_%H%M%S", &local);
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    readable = text;
    std::ostringstream name;
    name << "event_" << base << '_' << std::setfill('0') << std::setw(3) << milliseconds.count();
    return name.str();
}

std::string auto_monitor_url(int port) {
    std::string fallback;
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
        for (struct ifaddrs* item = addresses; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
            char host[NI_MAXHOST] = {0};
            const auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
            inet_ntop(AF_INET, &address->sin_addr, host, NI_MAXHOST);
            const std::string ip(host);
            if (ip.empty() || ip.find("127.") == 0 || ip.find("192.168.0.") == 0) continue;
            if (ip == "192.168.1.100") {
                fallback = ip;
                break;
            }
            if (fallback.empty()) fallback = ip;
        }
        freeifaddrs(addresses);
    }
    if (fallback.empty()) fallback = "192.168.1.100";
    return "http://" + fallback + ":" + std::to_string(port);
}

}  // namespace

MonitorService::MonitorService(MonitorOptions options)
    : options_(std::move(options)), config_path_(options_.config_path) {}

MonitorService::~MonitorService() {
    shutdown();
}

void MonitorService::initialize() {
    ensure_output_directory(options_.recordings_dir);
    load_initial_config();
    std::lock_guard<std::mutex> lock(mutex_);
    configure_feishu_locked();
    configure_relay_locked();
}

void MonitorService::load_initial_config() {
    std::ifstream configured(config_path_);
    if (configured.good()) {
        load_config_file(config_path_);
        return;
    }
    load_config_file("spark/spark_config.example.json");
    std::lock_guard<std::mutex> lock(mutex_);
    config_path_ = options_.config_path;
}

void MonitorService::load_config_file(const std::string& path) {
    JsonValue loaded = load_json_file(path);
    if (!loaded.is_object()) throw SparkError("spark monitor config root must be an object");
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(loaded);
    config_path_ = path;
    monitoring_ = false;
    resolution_dirty_ = true;
    ++detector_revision_;
}

void MonitorService::save_config_locked() const {
    ensure_output_directory(parent_directory(config_path_));
    const std::string temporary = config_path_ + ".tmp";
    write_json_file(temporary, config_);
    if (std::rename(temporary.c_str(), config_path_.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw SparkError("failed to replace spark monitor config: " + config_path_);
    }
}

void MonitorService::configure_feishu_locked() {
    const JsonValue* settings = member(config_, "feishu");
    const bool requested = bool_value(settings ? member(*settings, "enabled") : nullptr, false);
    std::string app_id = string_value(settings ? member(*settings, "app_id") : nullptr);
    std::string app_secret = string_value(settings ? member(*settings, "app_secret") : nullptr);
    std::string receive_id = string_value(settings ? member(*settings, "receive_id") : nullptr);
    std::string receive_type = string_value(
        settings ? member(*settings, "receive_id_type") : nullptr, "chat_id");
    feishu_web_url_ = string_value(settings ? member(*settings, "web_monitor_url") : nullptr, "auto");

    if (app_id.empty() || app_id.find("YOUR_") == 0) app_id = env_or("FEISHU_APP_ID");
    if (app_secret.empty() || app_secret.find("YOUR_") == 0) app_secret = env_or("FEISHU_APP_SECRET");
    if (receive_id.empty() || receive_id.find("YOUR_") == 0) receive_id = env_or("FEISHU_RECEIVE_ID");
    if (receive_type.empty()) receive_type = env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id");

    std::string actual_url = feishu_web_url_;
    if (actual_url.empty() || actual_url == "auto") actual_url = auto_monitor_url(options_.web_port);
    feishu_.reset();
    feishu_enabled_ = false;
    if (!requested) return;
    if (app_id.empty() || app_secret.empty() || receive_id.empty()) {
        std::cerr << "[spark] Feishu is enabled but credentials are incomplete; notifications disabled.\n";
        return;
    }
    feishu_ = std::make_shared<FeishuClient>();
    feishu_->init(app_id, app_secret, receive_id, receive_type, actual_url);
    feishu_enabled_ = true;
}

void MonitorService::set_relay_energized_locked(bool energized) {
    if (relay_ && relay_trip_enabled_) {
        relay_->set_channel(relay_channel_, energized);
        relay_energized_ = energized;
    } else {
        relay_energized_ = false;
    }
}

void MonitorService::configure_relay_locked() {
    set_relay_energized_locked(false);
    relay_.reset();
    relay_trip_enabled_ = bool_value(member(config_, "relay_trip_enabled"), false);
    relay_channel_ = static_cast<int>(number_value(member(config_, "relay_channel"), 1));
    if (relay_channel_ < 1 || relay_channel_ > 8) {
        std::cerr << "[spark] Invalid relay channel; hardware output disabled.\n";
        relay_trip_enabled_ = false;
        return;
    }
    if (!relay_trip_enabled_) return;

    const JsonValue* settings = member(config_, "relay");
    std::string host = string_value(settings ? member(*settings, "host") : nullptr);
    int port = static_cast<int>(number_value(settings ? member(*settings, "port") : nullptr, 0));
    if (host.empty()) host = env_or("RELAY_HOST", "192.168.0.7");
    if (port == 0) port = std::stoi(env_or("RELAY_PORT", "8234"));
    if (port < 1 || port > 65535) {
        std::cerr << "[spark] Invalid relay port; hardware output disabled.\n";
        relay_trip_enabled_ = false;
        return;
    }
    relay_ = std::make_shared<RelayClient>();
    relay_->init(host, port, true, relay_channel_);
}

void MonitorService::ensure_source_shape_locked(int width, int height) {
    if (width == source_width_ && height == source_height_) return;
    source_width_ = width;
    source_height_ = height;
    const JsonValue* calibration = member(config_, "calibration");
    const bool complete = bool_value(
        calibration ? member(*calibration, "complete") : nullptr, false);
    const int calibrated_width = static_cast<int>(number_value(
        calibration ? member(*calibration, "frame_width") : nullptr, 0));
    const int calibrated_height = static_cast<int>(number_value(
        calibration ? member(*calibration, "frame_height") : nullptr, 0));
    resolution_dirty_ = !complete || calibrated_width != width || calibrated_height != height;
    if (resolution_dirty_) monitoring_ = false;
}

void MonitorService::rebuild_detector_locked(double fps) {
    detector_ = std::make_unique<SparkDetector>(config_, fps > 0.0 ? fps : 25.0);
    detector_revision_ = 0;
}

void MonitorService::clear_rois_locked(int width, int height) {
    set_member(config_, "contact_roi", roi_json(0, 0, 0, 0));
    set_member(config_, "falling_roi", roi_json(0, 0, 0, 0));
    set_member(config_, "reference_roi", roi_json(0, 0, 0, 0));
    set_member(config_, "calibration", JsonValue::object({
        JsonValue::Member("complete", JsonValue::boolean(false)),
        JsonValue::Member("frame_width", JsonValue::integer(width)),
        JsonValue::Member("frame_height", JsonValue::integer(height))}));
    resolution_dirty_ = false;
    monitoring_ = false;
    set_relay_energized_locked(false);
    recent_frames_.clear();
    ++detector_revision_;
}

std::string MonitorService::api_ok(const std::string& message) {
    return "{\"status\":\"ok\",\"msg\":" + json_escape(message) + "}";
}

std::string MonitorService::api_error(const std::string& message) {
    return "{\"status\":\"error\",\"msg\":" + json_escape(message) + "}";
}

std::string MonitorService::recordings_dir() const {
    return options_.recordings_dir;
}

int MonitorService::web_port() const {
    return options_.web_port;
}

DetectionResult MonitorService::process_video_frame(
    const cv::Mat& frame,
    const std::vector<uchar>& jpeg,
    std::int64_t timestamp_ms,
    double detector_fps,
    double measured_fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_source_shape_locked(frame.cols, frame.rows);
    source_online_ = true;
    measured_fps_ = measured_fps;
    if (!detector_ || detector_revision_ != 0) rebuild_detector_locked(detector_fps);

    DetectionResult result;
    result.timestamp_ms = timestamp_ms;
    if (monitoring_) result = detector_->process_frame(
        frame.data, frame.cols, frame.rows, static_cast<int>(frame.step / frame.cols), timestamp_ms);
    latest_result_ = result;

    const JpegFrame current{timestamp_ms, jpeg};
    if (monitoring_) update_recent_locked(current);
    const bool already_recording = static_cast<bool>(active_recording_);
    if (result.event_triggered) handle_event_locked(result, frame, detector_fps);
    if (already_recording && active_recording_) append_post_frame_locked(current);
    if (active_recording_ && timestamp_ms >= active_recording_->post_end_ms) {
        finalize_recording_locked();
    }
    return result;
}

void MonitorService::update_recent_locked(const JpegFrame& frame) {
    const double pre_seconds = number_value(member(config_, "record_pre_sec"), 10.0);
    const std::size_t capacity = static_cast<std::size_t>(
        std::max(1.0, pre_seconds * detector_->fps())) + 2;
    recent_frames_.push_back(frame);
    while (recent_frames_.size() > capacity) recent_frames_.pop_front();
}

void MonitorService::append_post_frame_locked(const JpegFrame& frame) {
    active_recording_->frames.push_back(frame);
}

void MonitorService::handle_event_locked(
    const DetectionResult& result,
    const cv::Mat& frame,
    double fps) {
    EventInfo event;
    event.base_name = event_time_name(event.time_text);
    event.level = result.level;
    event.color_type = result.color_type;
    event.timestamp_ms = result.timestamp_ms;

    cv::Mat snapshot = frame.clone();
    const struct { const char* name; cv::Scalar color; } rois[] = {
        {"contact_roi", cv::Scalar(0, 0, 255)},
        {"falling_roi", cv::Scalar(0, 165, 255)},
        {"reference_roi", cv::Scalar(255, 0, 0)}};
    for (const auto& item : rois) {
        const ResolvedRoi roi = resolve_roi(member(config_, item.name), frame.cols, frame.rows);
        if (roi.valid) {
            cv::rectangle(snapshot,
                cv::Point(roi.roi.x1, roi.roi.y1), cv::Point(roi.roi.x2, roi.roi.y2),
                item.color, 3);
        }
    }
    cv::putText(snapshot, result.level + " / " + result.color_type,
        cv::Point(24, 48), cv::FONT_HERSHEY_SIMPLEX, 1.0,
        cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
    cv::imwrite(options_.recordings_dir + "/" + event.base_name + ".jpg", snapshot);

    if (!active_recording_) {
        auto job = std::make_unique<RecordingJob>();
        job->base_name = event.base_name;
        job->fps = fps > 0.0 ? fps : 25.0;
        job->post_end_ms = result.timestamp_ms + static_cast<std::int64_t>(
            number_value(member(config_, "record_post_sec"), 10.0) * 1000.0);
        job->frames.assign(recent_frames_.begin(), recent_frames_.end());
        event.clip_name = event.base_name + ".mp4";
        active_recording_ = std::move(job);
    }

    if (!relay_energized_) set_relay_energized_locked(true);
    send_spark_notification_locked(event, result);
    events_.push_back(event);
    if (events_.size() > 50) events_.erase(events_.begin());

    std::ofstream log(options_.recordings_dir + "/events.jsonl", std::ios::app);
    log << "{\"event\":\"" << event.base_name << "\",\"time\":\"" << event.time_text
        << "\",\"timestamp_ms\":" << result.timestamp_ms
        << ",\"level\":\"" << result.level << "\",\"color_type\":\"" << result.color_type
        << "\",\"candidate_duration_ms\":" << result.candidate_duration_ms
        << ",\"rel_saturation\":" << result.rel_saturation
        << ",\"rel_flicker\":" << result.rel_flicker
        << ",\"falling_bright_pixels\":" << result.falling_bright_pixels
        << ",\"relay_channel\":" << relay_channel_
        << ",\"relay_triggered\":" << (relay_energized_ ? "true" : "false") << "}\n";
}

void MonitorService::send_spark_notification_locked(
    const EventInfo& event,
    const DetectionResult& result) {
    if (!feishu_) return;
    const double cooldown = number_value(member(config_, "cooldown_sec"), 60.0);
    if (last_notification_ms_ >= 0 &&
        result.timestamp_ms - last_notification_ms_ < cooldown * 1000.0) return;
    last_notification_ms_ = result.timestamp_ms;
    std::ostringstream saturation;
    std::ostringstream flicker;
    saturation << std::fixed << std::setprecision(4) << result.rel_saturation;
    flicker << std::fixed << std::setprecision(4) << result.rel_flicker;
    std::vector<FeishuField> fields = {
        {"监控设备", "1#A 发电机碳刷 / 滑环"},
        {"事件等级", event.level + " / " + event.color_type},
        {"持续时间", std::to_string(result.candidate_duration_ms) + " ms"},
        {"相对饱和率", saturation.str()},
        {"相对闪烁", flicker.str()},
        {"下落亮像素", std::to_string(result.falling_bright_pixels)},
        {"现场抓拍", event.base_name + ".jpg"},
        {"保护动作", relay_energized_
            ? "Y" + std::to_string(relay_channel_) + " 通电指令已下发（告警锁存）"
            : "继电器联动未启用"}};
    feishu_->send_card_async("【疑似火花】1#A 碳刷监测触发", "orange", fields);
}

void MonitorService::finalize_recording_locked() {
    if (!active_recording_) return;
    RecordingJob job = std::move(*active_recording_);
    active_recording_.reset();
    recording_threads_.emplace_back(&MonitorService::recording_worker, this, std::move(job));
}

void MonitorService::recording_worker(RecordingJob job) {
    std::string error;
    if (job.frames.empty()) {
        error = "recording contains no frames";
    } else {
        cv::VideoWriter writer;
        bool opened = false;
        std::size_t written = 0;
        cv::Size size;
        for (const JpegFrame& frame : job.frames) {
            cv::Mat decoded = cv::imdecode(frame.bytes, cv::IMREAD_COLOR);
            if (decoded.empty()) continue;
            if (!opened) {
                size = decoded.size();
                writer.open(options_.recordings_dir + "/" + job.base_name + ".mp4",
                    cv::VideoWriter::fourcc('m', 'p', '4', 'v'), job.fps, size);
                if (!writer.isOpened()) {
                    error = "failed to open recording writer";
                    break;
                }
                opened = true;
            }
            if (decoded.size() == size) writer.write(decoded);
            ++written;
        }
        if (opened) writer.release();
        if (written == 0 && error.empty()) error = "recording contains no decodable frames";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error.empty()) {
        recording_error_ = error;
        return;
    }
    last_recording_ = job.base_name + ".mp4";
    for (EventInfo& event : events_) {
        if (event.clip_name == last_recording_) event.clip_ready = true;
    }
}

void MonitorService::finish_source() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_online_ = false;
    set_relay_energized_locked(false);
    finalize_recording_locked();
}

void MonitorService::shutdown() {
    std::vector<std::thread> workers;
    std::shared_ptr<RelayClient> relay_to_stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitoring_ = false;
        source_online_ = false;
        set_relay_energized_locked(false);
        relay_to_stop = std::move(relay_);
        finalize_recording_locked();
        workers.swap(recording_threads_);
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    relay_to_stop.reset();
}

std::string MonitorService::handle_api(const std::string& path, const std::string& query) {
    if (path == "/api/status") return status_json();
    if (path == "/api/load_history") {
        load_config_file(config_path_);
        std::lock_guard<std::mutex> lock(mutex_);
        const int width = source_width_;
        const int height = source_height_;
        source_width_ = 0;
        source_height_ = 0;
        ensure_source_shape_locked(width, height);
        configure_feishu_locked();
        configure_relay_locked();
        return api_ok("history_loaded");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (path == "/api/set_roi") {
        const std::string name = query_value(query, "name");
        if (name != "contact_roi" && name != "falling_roi" && name != "reference_roi") {
            return api_error("invalid_roi_name");
        }
        if (source_width_ <= 0 || source_height_ <= 0) return api_error("video_not_ready");
        const int x1 = query_int(query, "x1", 0);
        const int y1 = query_int(query, "y1", 0);
        const int x2 = query_int(query, "x2", 0);
        const int y2 = query_int(query, "y2", 0);
        if (x1 < 0 || y1 < 0 || x2 > source_width_ || y2 > source_height_ ||
            x2 - x1 < 8 || y2 - y1 < 8) return api_error("invalid_roi_bounds");
        if (resolution_dirty_) clear_rois_locked(source_width_, source_height_);
        set_member(config_, name, roi_json(x1, y1, x2, y2));
        JsonValue* calibration = member(config_, "calibration");
        if (calibration) set_member(*calibration, "complete", JsonValue::boolean(false));
        monitoring_ = false;
        set_relay_energized_locked(false);
        ++detector_revision_;
        save_config_locked();
        return api_ok("roi_saved");
    }
    if (path == "/api/clear_rois") {
        if (source_width_ <= 0 || source_height_ <= 0) return api_error("video_not_ready");
        clear_rois_locked(source_width_, source_height_);
        save_config_locked();
        return api_ok("rois_cleared");
    }
    if (path == "/api/start_monitoring") {
        if (source_width_ <= 0 || source_height_ <= 0) return api_error("video_not_ready");
        const JsonValue* calibration = member(config_, "calibration");
        const bool complete = bool_value(
            calibration ? member(*calibration, "complete") : nullptr, false);
        const int calibrated_width = static_cast<int>(number_value(
            calibration ? member(*calibration, "frame_width") : nullptr, 0));
        const int calibrated_height = static_cast<int>(number_value(
            calibration ? member(*calibration, "frame_height") : nullptr, 0));
        if (calibrated_width != source_width_ || calibrated_height != source_height_) {
            return api_error("recalibration_required: complete=" + std::to_string(complete) +
                ", calibrated=" + std::to_string(calibrated_width) + "x" +
                std::to_string(calibrated_height) + ", current=" +
                std::to_string(source_width_) + "x" + std::to_string(source_height_));
        }
        resolution_dirty_ = false;
        const char* names[] = {"contact_roi", "falling_roi", "reference_roi"};
        for (const char* name : names) {
            if (!resolve_roi(member(config_, name), source_width_, source_height_).valid) {
                return api_error(std::string("missing_or_invalid_") + name);
            }
        }
        set_member(config_, "calibration", JsonValue::object({
            JsonValue::Member("complete", JsonValue::boolean(true)),
            JsonValue::Member("frame_width", JsonValue::integer(source_width_)),
            JsonValue::Member("frame_height", JsonValue::integer(source_height_))}));
        monitoring_ = true;
        recent_frames_.clear();
        last_notification_ms_ = -1;
        ++detector_revision_;
        save_config_locked();
        return api_ok("monitoring_started");
    }
    if (path == "/api/enter_calibrate") {
        monitoring_ = false;
        set_relay_energized_locked(false);
        if (JsonValue* calibration = member(config_, "calibration")) {
            set_member(*calibration, "complete", JsonValue::boolean(false));
        }
        recent_frames_.clear();
        ++detector_revision_;
        save_config_locked();
        return api_ok("calibration_mode");
    }

    if (path == "/api/update_relay") {
        const std::string enabled = query_value(query, "enabled");
        if (enabled != "0" && enabled != "1") return api_error("invalid_relay_enabled");
        set_member(config_, "relay_trip_enabled", JsonValue::boolean(enabled == "1"));
        configure_relay_locked();
        save_config_locked();
        return api_ok("relay_settings_saved");
    }
    if (path == "/api/relay_reset") {
        set_relay_energized_locked(false);
        return api_ok("relay_reset_requested");
    }

    if (path == "/api/update_params") {
        if (!member(config_, "thresholds")) {
            set_member(config_, "thresholds", JsonValue::object({}));
        }
        JsonValue* thresholds = member(config_, "thresholds");
        const std::string saturation = query_value(query, "rel_saturation");
        const std::string flicker = query_value(query, "flicker");
        const std::string color = query_value(query, "color_match_ratio");
        const std::string confirm = query_value(query, "confirm_duration_ms");
        const std::string cooldown = query_value(query, "cooldown_sec");
        if (!saturation.empty()) set_member(*thresholds, "rel_saturation",
            JsonValue::number(std::max(0.0, std::stod(saturation))));
        if (!flicker.empty()) set_member(*thresholds, "flicker",
            JsonValue::number(std::max(0.0, std::stod(flicker))));
        if (!color.empty()) set_member(*thresholds, "color_match_ratio",
            JsonValue::number(std::max(0.0, std::min(1.0, std::stod(color)))));
        if (!confirm.empty()) set_member(config_, "confirm_duration_ms",
            JsonValue::integer(std::max<std::int64_t>(0, std::stoll(confirm))));
        if (!cooldown.empty()) set_member(config_, "cooldown_sec",
            JsonValue::number(std::max(0.0, std::stod(cooldown))));
        ++detector_revision_;
        save_config_locked();
        return api_ok("parameters_saved");
    }
    if (path == "/api/set_feishu_url") {
        std::string url = query_value(query, "url");
        if (url.empty()) url = "auto";
        if (!member(config_, "feishu")) set_member(config_, "feishu", JsonValue::object({}));
        set_member(*member(config_, "feishu"), "web_monitor_url", JsonValue::string(url));
        feishu_web_url_ = url;
        if (feishu_) {
            feishu_->set_web_monitor_url(
                url == "auto" ? auto_monitor_url(options_.web_port) : url);
        }
        save_config_locked();
        return api_ok("feishu_url_saved");
    }
    if (path == "/api/feishu_test") {
        if (!feishu_) return api_error("feishu_not_configured");
        feishu_->send_card_async("【测试】火花监测飞书联动", "blue", {
            {"监控设备", "1#A 发电机碳刷 / 滑环"},
            {"当前模式", monitoring_ ? "正式监控" : "标定"},
            {"保护动作", relay_trip_enabled_
                ? "确认异常时 Y" + std::to_string(relay_channel_) + " 通电锁存"
                : "继电器联动未启用"}});
        return api_ok("feishu_test_queued");
    }

    return api_error("unknown_api");
}

std::string MonitorService::status_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const JsonValue* thresholds = member(config_, "thresholds");
    const JsonValue* feishu = member(config_, "feishu");
    const JsonValue* calibration = member(config_, "calibration");
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "{\"mode\":\"" << (monitoring_ ? "MONITORING" : "CALIBRATING") << '"'
        << ",\"source_online\":" << (source_online_ ? "true" : "false")
        << ",\"frame_width\":" << source_width_
        << ",\"frame_height\":" << source_height_
        << ",\"fps\":" << measured_fps_
        << ",\"web_port\":" << options_.web_port
        << ",\"calibration_required\":" << (resolution_dirty_ ? "true" : "false")
        << ",\"calibration_debug\":{\"complete\":" << (bool_value(
            calibration ? member(*calibration, "complete") : nullptr, false) ? "true" : "false")
        << ",\"frame_width\":" << static_cast<int>(number_value(
            calibration ? member(*calibration, "frame_width") : nullptr, 0))
        << ",\"frame_height\":" << static_cast<int>(number_value(
            calibration ? member(*calibration, "frame_height") : nullptr, 0)) << '}'
        << ",\"relay_trip_enabled\":" << (relay_trip_enabled_ ? "true" : "false")
        << ",\"relay_channel\":" << relay_channel_
        << ",\"relay_connected\":" << (relay_ && relay_->is_connected() ? "true" : "false")
        << ",\"relay_energized\":" << (relay_energized_ ? "true" : "false")
        << ",\"rois\":{";
    const char* names[] = {"contact_roi", "falling_roi", "reference_roi"};
    for (int index = 0; index < 3; ++index) {
        if (index) out << ',';
        out << '"' << names[index] << "\":"
            << roi_status_json(config_, names[index], source_width_, source_height_);
    }
    out << "},\"thresholds\":{"
        << "\"rel_saturation\":" << number_value(
            thresholds ? member(*thresholds, "rel_saturation") : nullptr, 0.01)
        << ",\"flicker\":" << number_value(
            thresholds ? member(*thresholds, "flicker") : nullptr, 0.5)
        << ",\"color_match_ratio\":" << number_value(
            thresholds ? member(*thresholds, "color_match_ratio") : nullptr, 0.5)
        << "},\"confirm_duration_ms\":" << static_cast<std::int64_t>(
            number_value(member(config_, "confirm_duration_ms"), 200))
        << ",\"cooldown_sec\":" << number_value(member(config_, "cooldown_sec"), 60.0)
        << ",\"feishu\":{"
        << "\"enabled\":" << (feishu_ ? "true" : "false")
        << ",\"requested\":" << (bool_value(
            feishu ? member(*feishu, "enabled") : nullptr, false) ? "true" : "false")
        << ",\"web_monitor_url\":" << json_escape(feishu_web_url_) << "}"
        << ",\"metrics\":{"
        << "\"timestamp_ms\":" << latest_result_.timestamp_ms
        << ",\"rel_saturation\":" << latest_result_.rel_saturation
        << ",\"rel_flicker\":" << latest_result_.rel_flicker
        << ",\"color_match_ratio\":" << latest_result_.color_match_ratio
        << ",\"color_hit\":" << (latest_result_.color_hit ? "true" : "false")
        << ",\"color_type\":\"" << latest_result_.color_type << '"'
        << ",\"falling_bright_pixels\":" << latest_result_.falling_bright_pixels
        << ",\"falling_components\":" << latest_result_.falling_components
        << ",\"candidate\":" << (latest_result_.candidate ? "true" : "false")
        << ",\"candidate_duration_ms\":" << latest_result_.candidate_duration_ms
        << ",\"level\":\"" << latest_result_.level << "\"}"
        << ",\"recording_active\":" << (active_recording_ ? "true" : "false")
        << ",\"last_recording\":\"" << last_recording_ << '"'
        << ",\"recording_error\":" << json_escape(recording_error_)
        << ",\"events\":[";
    for (std::size_t offset = 0; offset < events_.size(); ++offset) {
        const EventInfo& event = events_[events_.size() - 1 - offset];
        if (offset) out << ',';
        out << "{\"name\":\"" << event.base_name << '"'
            << ",\"time\":\"" << event.time_text << '"'
            << ",\"level\":\"" << event.level << '"'
            << ",\"color_type\":\"" << event.color_type << '"'
            << ",\"timestamp_ms\":" << event.timestamp_ms
            << ",\"snapshot\":\"" << event.base_name << ".jpg\""
            << ",\"clip\":\"" << event.clip_name << '"'
            << ",\"clip_ready\":" << (event.clip_ready ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

}  // namespace spark
