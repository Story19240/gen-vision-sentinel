CXX ?= g++
CXXFLAGS ?= -std=c++14 -O3 -Wall -Wextra -pedantic
OPENCV_PKG ?= opencv4
OPENCV_CFLAGS := $(shell pkg-config --cflags $(OPENCV_PKG) 2>/dev/null)
OPENCV_LIBS := $(shell pkg-config --libs $(OPENCV_PKG) 2>/dev/null)

BIN_DIR := bin
TARGET := $(BIN_DIR)/spark_offline
NO_OPENCV_TARGET := $(BIN_DIR)/spark_offline_no_opencv
CORE_SOURCES := json.cpp spark_detector.cpp offline_io.cpp synthesize.cpp
ALL_SOURCES := $(CORE_SOURCES) opencv_source.cpp main.cpp
HEADERS := json.hpp spark_detector.h offline_io.hpp opencv_source.hpp synthesize.hpp

.PHONY: all no-opencv clean

all: $(TARGET)

$(TARGET): $(ALL_SOURCES) $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $(ALL_SOURCES) $(OPENCV_LIBS) -o $@

no-opencv: $(NO_OPENCV_TARGET)

$(NO_OPENCV_TARGET): $(CORE_SOURCES) main.cpp $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DSPARK_WITHOUT_OPENCV $(CORE_SOURCES) main.cpp -o $@

clean:
	rm -rf $(BIN_DIR)
