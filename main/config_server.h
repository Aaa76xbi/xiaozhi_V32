#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include <esp_http_server.h>
#include <string>

class ConfigServer {
public:
    static ConfigServer& GetInstance() {
        static ConfigServer instance;
        return instance;
    }

    void Start();
    void Stop();

private:
    ConfigServer() = default;
    ~ConfigServer() { Stop(); }

    httpd_handle_t server_ = nullptr;

    static esp_err_t HandleRoot(httpd_req_t* req);       // GET  /
    static esp_err_t HandleGetConfig(httpd_req_t* req);  // GET  /api/config
    static esp_err_t HandleSaveTools(httpd_req_t* req);  // POST /api/tools
    static esp_err_t HandleSaveHa(httpd_req_t* req);     // POST /api/ha
    static esp_err_t HandleSaveAuth(httpd_req_t* req);   // POST /api/auth
    static esp_err_t HandleReboot(httpd_req_t* req);     // POST /api/reboot

    // HTTP Basic Auth 验证，失败时自动回 401
    static bool CheckAuth(httpd_req_t* req);

};

#endif // CONFIG_SERVER_H
