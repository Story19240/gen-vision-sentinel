CXX = g++
CXXFLAGS = -std=c++14 -O3 -Wall -Wno-deprecated-declarations
INCLUDES = -I/usr/local/cuda/include -I/usr/include/opencv4 -I/usr/include/aarch64-linux-gnu -Iinclude
LIBS = -L/usr/local/cuda/lib64 -lcudart -lnvinfer -lopencv_core -lopencv_imgproc -lopencv_imgcodecs

SRCS = src/yolov8_trt.cpp src/main.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = yolov8_app

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
