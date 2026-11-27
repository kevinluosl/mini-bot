#include "ov_http_client.h"

#include <esp_log.h>
#include <sdkconfig.h>

#include <limits>
#include <utility>

extern const char ov_gateway_cert_pem_start[] asm("_binary_ov_gateway_cert_pem_start");

esp_err_t OvHttpClient::OnEvent(esp_http_client_event_t* event) {
    auto& self = *static_cast<OvHttpClient*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        ++self.connections_;
        ESP_LOGI("OvWatch", "OV HTTPS 已连接 count=%u", self.connections_);
    }
    auto* response = self.response_;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || !response || response->too_large) {
        return ESP_OK;
    }
    const auto length = static_cast<size_t>(event->data_len);
    if (length > response->limit - response->reply.body.size()) {
        response->too_large = true;
        return ESP_OK;
    }
    response->reply.body.append(static_cast<const char*>(event->data), length);
    return ESP_OK;
}

std::expected<OvHttpReply, std::string> OvHttpClient::Request(
    std::string_view path, esp_http_client_method_t method, int timeout_ms, size_t reply_limit,
    const char* content_type, std::string_view body) {
    // 只允许向固定的 HTTPS 网关发送凭据；调用者只能改变网关下的资源路径。
    if (!base_url_.starts_with("https://") || !path.starts_with('/')) {
        return std::unexpected("OV 网关必须使用 HTTPS 和固定地址");
    }
    if (body.size() > static_cast<size_t>(std::numeric_limits<int>::max()) || (!body.empty() && !content_type)) {
        return std::unexpected("OV HTTP 请求正文无效");
    }
    std::string url = base_url_;
    url.append(path);
    if (!client_) {
        esp_http_client_config_t config{};
        config.url = url.c_str();
        config.timeout_ms = timeout_ms;
        // 保留主机名/IP 校验，不跟随重定向、不自动尝试其他认证方式。
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.cert_pem = ov_gateway_cert_pem_start;
        config.user_data = this;
        config.event_handler = OnEvent;
        client_.reset(esp_http_client_init(&config));
        if (!client_) return std::unexpected("创建 OV HTTP 客户端失败");
        const std::string authorization = std::string("Bearer ") + CONFIG_MINI_BOT_OV_API_KEY;
        if (esp_http_client_set_header(client_.get(), "Authorization", authorization.c_str()) != ESP_OK) {
            client_.reset();
            return std::unexpected("设置 OV HTTP 鉴权失败");
        }
    } else if (esp_http_client_set_url(client_.get(), url.c_str()) != ESP_OK) {
        client_.reset();
        return std::unexpected("设置 OV HTTP 路径失败");
    }
    if (esp_http_client_set_method(client_.get(), method) != ESP_OK ||
        esp_http_client_set_timeout_ms(client_.get(), timeout_ms) != ESP_OK ||
        (!body.empty() && (esp_http_client_set_header(client_.get(), "Content-Type", content_type) != ESP_OK ||
                          esp_http_client_set_post_field(client_.get(), body.data(),
                                                         static_cast<int>(body.size())) != ESP_OK))) {
        client_.reset();
        return std::unexpected("设置 OV HTTP 请求失败");
    }
    ResponseBuffer response{{}, reply_limit};
    response_ = &response;
    const auto err = esp_http_client_perform(client_.get());
    response_ = nullptr;
    // SDK 不拥有 POST 正文。同步发送后立即解绑，清除 Content-Type，避免下一次 GET/DELETE 复用旧正文。
    const auto clear = esp_http_client_set_post_field(client_.get(), nullptr, 0);
    // 无正文请求本来就没有 Content-Type，IDF 清理缺失的头字段时返回 NOT_FOUND。
    const bool cleared = clear == ESP_OK || clear == ESP_ERR_NOT_FOUND;
    // perform 会读完整响应并按 HTTP/1.1 保持连接。错误时丢弃可能失步的客户端；
    // 下一次正常调度再重连，当前 POST 不重放（网关可能已经收到）。
    if (err != ESP_OK || !cleared || response.too_large) {
        client_.reset();
        if (err != ESP_OK || !cleared) return std::unexpected(esp_err_to_name(err != ESP_OK ? err : clear));
        return std::unexpected("OV HTTP 响应超过内存限制");
    }
    response.reply.status = esp_http_client_get_status_code(client_.get());
    return std::move(response.reply);
}
