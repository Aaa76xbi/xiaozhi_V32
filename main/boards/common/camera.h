#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <stdexcept>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string& question) = 0;
    // 从外部URL下载JPEG并发送给AI视觉服务器描述画面（子类可覆写）
    virtual std::string ExplainFromUrl(const std::string& image_url, const std::string& image_token, const std::string& question) {
        throw std::runtime_error("ExplainFromUrl not supported by this camera");
    }
};

#endif // CAMERA_H
