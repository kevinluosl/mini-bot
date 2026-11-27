#pragma once

#include <esp_http_client.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

struct OvHttpReply {
    int status = 0;
    std::string body;
};

// 只由同一个观察任务串行使用。HTTP 会话 ID 变化不需要重建 TLS 连接。
class OvHttpClient {
public:
    explicit OvHttpClient(std::string_view base_url) : base_url_(base_url) {}
    OvHttpClient(const OvHttpClient&) = delete;
    OvHttpClient& operator=(const OvHttpClient&) = delete;

    std::expected<OvHttpReply, std::string> Request(
        std::string_view path, esp_http_client_method_t method, int timeout_ms, size_t reply_limit,
        const char* content_type = nullptr, std::string_view body = {});

private:
    struct ResponseBuffer {
        OvHttpReply reply;
        size_t limit;
        bool too_large = false;
    };
    static esp_err_t OnEvent(esp_http_client_event_t* event);

    const std::string base_url_;
    ResponseBuffer* response_ = nullptr;
    unsigned connections_ = 0;
    std::unique_ptr<esp_http_client, decltype(&esp_http_client_cleanup)> client_{nullptr, esp_http_client_cleanup};
};
