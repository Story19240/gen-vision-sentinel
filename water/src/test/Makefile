CXX = g++
CXXFLAGS = -std=c++14 -O3 -Wall -Wno-deprecated-declarations
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0)

INCLUDES = -I. -I.. -I/usr/include/opencv4 -I/usr/include/aarch64-linux-gnu $(GST_CFLAGS)
LIBS = $(GST_LIBS) -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lcurl -lpthread

TARGETS = water_flow_analyzer

all: $(TARGETS)
	@echo "=================================================="
	@echo "  [编译成功] 动能截断分析 + 纯 C++ 飞书联动已就绪: ./water_flow_analyzer"
	@echo "=================================================="

water_flow_analyzer: water_flow_analyzer.cpp ../feishu_client.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGETS) *.o
