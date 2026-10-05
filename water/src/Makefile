CXX = g++
CXXFLAGS = -std=c++14 -O3 -Wall -Wno-deprecated-declarations
INCLUDES = -I. -I/usr/include/opencv4 -I/usr/include/aarch64-linux-gnu
OPENCV_LIBS = -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio -lpthread
CURL_LIBS = -lcurl

TARGET_SERVER = water_monitor_server

all: $(TARGET_SERVER)
	@echo "=================================================="
	@echo "  [编译成功] 生产级监控总服务: $(TARGET_SERVER)"
	@echo "  启动指令: ./$(TARGET_SERVER)"
	@echo "  测试工具请进入: cd test && make"
	@echo "=================================================="

$(TARGET_SERVER): feishu_client.cpp water_flow_detector.cpp water_monitor_server.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(OPENCV_LIBS) $(CURL_LIBS)

clean:
	rm -f $(TARGET_SERVER) *.o
