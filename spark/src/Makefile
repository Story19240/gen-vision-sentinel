CXX ?= g++
CXXFLAGS ?= -std=c++14 -O3 -Wall -Wextra -pedantic
OPENCV_PKG ?= opencv4
OPENCV_CFLAGS := $(shell pkg-config --cflags $(OPENCV_PKG) 2>/dev/null)
OPENCV_LIBS := $(shell pkg-config --libs $(OPENCV_PKG) 2>/dev/null)

BIN_DIR := bin
TARGET := $(BIN_DIR)/spark_offline
NO_OPENCV_TARGET := $(BIN_DIR)/spark_offline_no_opencv
MONITOR_TARGET := $(BIN_DIR)/spark_monitor_server
MONITOR_RELAY_TEST_TARGET := $(BIN_DIR)/test_monitor_relay
CORE_SOURCES := json.cpp spark_detector.cpp offline_io.cpp synthesize.cpp
ALL_SOURCES := $(CORE_SOURCES) opencv_source.cpp main.cpp
HEADERS := json.hpp spark_detector.h offline_io.hpp opencv_source.hpp synthesize.hpp
MONITOR_OPENCV_CFLAGS := $(if $(strip $(OPENCV_CFLAGS)),$(OPENCV_CFLAGS),-I/usr/include/opencv4)
MONITOR_OPENCV_LIBS := $(if $(strip $(OPENCV_LIBS)),$(OPENCV_LIBS),-lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio)
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
MONITOR_SOURCES := spark_monitor_server.cpp monitor_service.cpp json.cpp spark_detector.cpp offline_io.cpp ../../water/src/feishu_client.cpp ../../water/src/relay_client.cpp
MONITOR_RELAY_TEST_SOURCES := test_monitor_relay.cpp monitor_service.cpp json.cpp spark_detector.cpp offline_io.cpp synthesize.cpp ../../water/src/relay_client.cpp

.PHONY: all monitor test-monitor-relay no-opencv clean

all: $(TARGET)

$(TARGET): $(ALL_SOURCES) $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $(ALL_SOURCES) $(OPENCV_LIBS) -o $@

no-opencv: $(NO_OPENCV_TARGET)

$(NO_OPENCV_TARGET): $(CORE_SOURCES) main.cpp $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DSPARK_WITHOUT_OPENCV $(CORE_SOURCES) main.cpp -o $@

monitor: $(MONITOR_TARGET)

$(MONITOR_TARGET): $(MONITOR_SOURCES) monitor_service.h monitor_studio.html
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I. $(MONITOR_OPENCV_CFLAGS) $(GST_CFLAGS) $(MONITOR_SOURCES) $(MONITOR_OPENCV_LIBS) $(GST_LIBS) -lcurl -lpthread -o $@

test-monitor-relay: $(MONITOR_RELAY_TEST_TARGET)

$(MONITOR_RELAY_TEST_TARGET): $(MONITOR_RELAY_TEST_SOURCES) monitor_service.h
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I. $(MONITOR_OPENCV_CFLAGS) $(MONITOR_RELAY_TEST_SOURCES) $(MONITOR_OPENCV_LIBS) -lpthread -o $@

clean:
	rm -rf $(BIN_DIR)
