"""Exercise the production OV transport against a synchronous ESP HTTP client fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


COMMON = Path(__file__).resolve().parents[2] / 'main/boards/common'

HTTP_HEADER = r'''
#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = 0x105;
enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_DELETE };
enum HttpEvent { HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_ON_DATA, HTTP_EVENT_DISCONNECTED };
struct esp_http_client_event_t { HttpEvent event_id; void* user_data; void* data; int data_len; };
struct esp_http_client_config_t {
    const char* url;
    int timeout_ms;
    bool disable_auto_redirect, skip_cert_common_name_check;
    int max_authorization_retries;
    const char* cert_pem;
    void* user_data;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
};
using esp_http_client_handle_t = struct esp_http_client*;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t, const char*);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char*, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
const char* esp_err_to_name(esp_err_t);
'''

DRIVER = r'''
#include "ov_http_client.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

extern const char public_cert[] asm("_binary_ov_gateway_cert_pem_start") = "public certificate fixture";
struct esp_http_client {
    esp_http_client_config_t config;
    std::string url;
    int timeout;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    std::map<std::string, std::string> headers;
    const char* data = nullptr;
    int size = 0, status = 0;
    bool connected = false;
};
struct Exchange {
    std::string path;
    esp_http_client_method_t method;
    int timeout;
    std::string content_type, body;
    int status;
    std::vector<std::string> chunks;
    bool drop = false, server_closes = false;
};
std::deque<Exchange> exchanges;
unsigned created = 0, freed = 0, handshakes = 0, performed = 0, handshake_logs = 0;

void LogConnection(const char*, const char*, unsigned count) { assert(count == ++handshake_logs); }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    assert(config->disable_auto_redirect && !config->skip_cert_common_name_check);
    assert(config->max_authorization_retries == -1 && config->cert_pem == public_cert);
    ++created;
    return new esp_http_client{*config, config->url, config->timeout_ms};
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(!c->data && c->size == 0);
    esp_http_client_event_t event{HTTP_EVENT_DISCONNECTED, c->config.user_data, nullptr, 0};
    c->config.event_handler(&event);
    ++freed;
    delete c;
    return ESP_OK;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char* v) { c->url = v; return ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t v) {
    c->method = v; return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int v) { c->timeout = v; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char* key, const char* value) {
    if (value) c->headers[key] = value;
    else c->headers.erase(key);
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char* v, int size) {
    c->data = v; c->size = size;
    if (!v && !c->headers.erase("Content-Type")) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    assert(!exchanges.empty());
    const auto e = std::move(exchanges.front()); exchanges.pop_front();
    ++performed;
    assert(c->url == "https://unit.test:19200" + e.path && c->method == e.method && c->timeout == e.timeout);
    assert(c->headers.at("Authorization") == "Bearer test-key");
    assert(e.content_type.empty() ? !c->headers.contains("Content-Type") :
                                    c->headers.at("Content-Type") == e.content_type);
    assert(c->size == e.body.size());
    assert(e.body.empty() ? !c->data : std::memcmp(c->data, e.body.data(), e.body.size()) == 0);
    if (!c->connected) {
        ++handshakes;
        c->connected = true;
        esp_http_client_event_t event{HTTP_EVENT_ON_CONNECTED, c->config.user_data, nullptr, 0};
        c->config.event_handler(&event);
    }
    for (auto chunk : e.chunks) {
        esp_http_client_event_t event{HTTP_EVENT_ON_DATA, c->config.user_data, chunk.data(), int(chunk.size())};
        c->config.event_handler(&event);
    }
    if (e.server_closes) c->connected = false;
    c->status = e.status;
    return e.drop ? ESP_FAIL : ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
const char* esp_err_to_name(esp_err_t) { return "ESP_FAIL"; }

int main() {
    {
        OvHttpClient http("https://unit.test:19200");
        auto jpeg = std::string("\xff\0\xd9", 3);
        exchanges = {
            {"/sessions", HTTP_METHOD_POST, 8000, "application/json", "{}", 201, {"a", "b"}},
            {"/sessions/1/frame?t=1", HTTP_METHOD_POST, 8000, "image/jpeg", jpeg, 200, {"ok"}},
            {"/sessions/1/frame?t=2", HTTP_METHOD_POST, 8000, "image/jpeg", jpeg, 200, {"ok"}},
            {"/sessions/1/results?after=0", HTTP_METHOD_GET, 1500, "", "", 200, {"[]"}},
            {"/sessions/1/text", HTTP_METHOD_POST, 8000, "application/json", "{}", 0, {"partial"}, true},
            {"/sessions/1", HTTP_METHOD_DELETE, 3000, "", "", 204, {}, false, true},
            {"/sessions", HTTP_METHOD_POST, 8000, "application/json", "{}", 201, {"ab", "cd", "ef"}},
            {"/sessions/2", HTTP_METHOD_DELETE, 3000, "", "", 204, {}},
        };
        auto started = http.Request("/sessions", HTTP_METHOD_POST, 8000, 2, "application/json", "{}");
        assert(started && started->status == 201 && started->body == "ab");
        for (const auto* p : {"/sessions/1/frame?t=1", "/sessions/1/frame?t=2"}) {
            auto sent = http.Request(p, HTTP_METHOD_POST, 8000, 2, "image/jpeg", jpeg);
            assert(sent && sent->status == 200 && sent->body == "ok");
        }
        assert(http.Request("/sessions/1/results?after=0", HTTP_METHOD_GET, 1500, 2)->body == "[]");
        assert(created == 1 && handshakes == 1 && performed == 4 && freed == 0);
        assert(!http.Request("/sessions/1/text", HTTP_METHOD_POST, 8000, 8, "application/json", "{}"));
        assert(performed == 5 && freed == 1);  // A possibly delivered POST is never replayed.
        auto stopped = http.Request("/sessions/1", HTTP_METHOD_DELETE, 3000, 8);
        assert(stopped && stopped->status == 204 && stopped->body.empty() && handshakes == 2);
        auto oversized = http.Request("/sessions", HTTP_METHOD_POST, 8000, 3, "application/json", "{}");
        assert(!oversized && oversized.error() == "OV HTTP 响应超过内存限制");
        assert(created == 2 && handshakes == 3 && freed == 2);  // Server close reopens the socket, not the handle.
        assert(http.Request("/sessions/2", HTTP_METHOD_DELETE, 3000, 8)->status == 204);
        assert(exchanges.empty() && created == 3 && handshakes == 4 && performed == 8);
        assert(!http.Request("https://other.test/resource", HTTP_METHOD_GET, 1500, 2));
    }
    OvHttpClient insecure("http://unit.test:19200");
    assert(!insecure.Request("/sessions", HTTP_METHOD_GET, 1500, 2));
    assert(created == 3 && freed == 3 && handshakes == handshake_logs);
}
'''


class OvHttpClientTests(unittest.TestCase):
    def test_reuse_disconnect_and_request_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'esp_http_client.h').write_text(HTTP_HEADER)
            (tmp / 'esp_log.h').write_text('void LogConnection(const char*, const char*, unsigned);\n'
                                          '#define ESP_LOGI(...) LogConnection(__VA_ARGS__)\n')
            (tmp / 'sdkconfig.h').write_text('#define CONFIG_MINI_BOT_OV_API_KEY "test-key"\n')
            (tmp / 'driver.cc').write_text(DRIVER)
            exe = tmp / 'ov_http_test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-I', str(tmp), '-I', str(COMMON),
                            str(COMMON / 'ov_http_client.cc'), str(tmp / 'driver.cc'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=5)
