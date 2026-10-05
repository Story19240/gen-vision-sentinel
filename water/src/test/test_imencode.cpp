#include <iostream>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

int main() {
    cv::Mat img = cv::Mat::zeros(100, 100, CV_8UC3);
    std::vector<uchar> buf;
    if (cv::imencode(".jpg", img, buf)) {
        std::cout << "imencode 成功! 大小: " << buf.size() << " 字节" << std::endl;
        return 0;
    }
    return -1;
}
