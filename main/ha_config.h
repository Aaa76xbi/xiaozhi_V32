#ifndef HA_CONFIG_H
#define HA_CONFIG_H

#include <string>
#include <vector>

// 运行时 HA 配置，从 NVS 读取，默认值回退到编译时宏
// 通过 ConfigServer Web UI 修改，重启后生效
class HaConfig {
public:
    static HaConfig& GetInstance() {
        static HaConfig instance;
        return instance;
    }

    // HA 实例 A（老设备 / 外网）
    std::string ha_old_url();
    std::string ha_old_token();
    std::string entity_main_switch();
    std::string entity_tv();
    std::string entity_gas_valve();
    std::string entity_water_valve();

    // HA 实例 B（新设备 / 本地）
    std::string ha_new_url();
    std::string ha_new_token();
    std::string entity_smart_plug();
    std::string entity_door_sensor();
    std::string entity_curtain_1();
    std::string entity_curtain_2();
    std::string entity_speaker();
    std::string entity_speaker_tts();
    std::string entity_speaker_cmd();

    // 摄像头（基础 URL 不含 /api 后缀）
    std::string ha_camera_url();
    std::string ha_camera_token();
    std::string ha_camera_entity();
    std::string ha_camera_motion_sensor();

    // 摄像头派生 URL（动态组合，调用时计算）
    std::string ha_camera_ptz_url();
    std::string ha_camera_proxy_url();
    std::string ha_camera_proxy_small_url();
    std::string ha_camera_motion_url();

    // 保存单个键值到 NVS（NVS key 即 JSON key，供 ConfigServer 调用）
    void Set(const std::string& nvs_key, const std::string& value);

    // 返回所有配置的 JSON，key 为 NVS key（供 Web UI）
    std::string ToJson();

    // 自定义设备（动态增删，重启后自动注册为 MCP 工具）
    struct CustomDevice {
        std::string id;      // 短 ID（字母/数字/下划线），工具名 = "control_" + id
        std::string name;    // 中文显示名（如"空调"）
        std::string entity;  // HA entity_id
        std::string ha;      // "old"=外网旧HA  "new"=本地新HA
        std::string domain;  // "switch"|"cover"|"climate"|"light"|"fan" 等
    };
    std::vector<CustomDevice> GetCustomDevices();
    void SaveCustomDevices(const std::vector<CustomDevice>& devices);

private:
    HaConfig() = default;
    std::string Get(const std::string& nvs_key, const std::string& default_val);
};

#endif // HA_CONFIG_H
