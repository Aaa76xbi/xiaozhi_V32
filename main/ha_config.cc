#include "ha_config.h"
#include "my_home_device.h"
#include "settings.h"
#include <cJSON.h>

#define NS "ha_cfg"

std::string HaConfig::Get(const std::string& nvs_key, const std::string& default_val) {
    Settings s(NS);
    return s.GetString(nvs_key, default_val);
}

void HaConfig::Set(const std::string& nvs_key, const std::string& value) {
    Settings s(NS, true);
    s.SetString(nvs_key, value);
}

// HA 实例 A
std::string HaConfig::ha_old_url()         { return Get("ha_old_url", HA_OLD_URL); }
std::string HaConfig::ha_old_token()       { return Get("ha_old_tok", HA_OLD_TOKEN); }
std::string HaConfig::entity_main_switch() { return Get("e_main_sw",  ENTITY_MAIN_SWITCH); }
std::string HaConfig::entity_tv()          { return Get("e_tv",       ENTITY_TV); }
std::string HaConfig::entity_gas_valve()   { return Get("e_gas",      ENTITY_GAS_VALVE); }
std::string HaConfig::entity_water_valve() { return Get("e_water",    ENTITY_WATER_VALVE); }

// HA 实例 B
std::string HaConfig::ha_new_url()         { return Get("ha_new_url", HA_NEW_URL); }
std::string HaConfig::ha_new_token()       { return Get("ha_new_tok", HA_NEW_TOKEN); }
std::string HaConfig::entity_smart_plug()  { return Get("e_plug",     ENTITY_SMART_PLUG); }
std::string HaConfig::entity_door_sensor() { return Get("e_door",     ENTITY_DOOR_SENSOR); }
std::string HaConfig::entity_curtain_1()   { return Get("e_curtain1", ENTITY_CURTAIN_1); }
std::string HaConfig::entity_curtain_2()   { return Get("e_curtain2", ENTITY_CURTAIN_2); }
std::string HaConfig::entity_speaker()     { return Get("e_speaker",  ENTITY_SPEAKER); }
std::string HaConfig::entity_speaker_tts() { return Get("e_spk_tts",  ENTITY_SPEAKER_TTS); }
std::string HaConfig::entity_speaker_cmd() { return Get("e_spk_cmd",  ENTITY_SPEAKER_CMD); }

// 摄像头基础字段
std::string HaConfig::ha_camera_url()    { return Get("ha_cam_url", HA_CAMERA_URL); }
std::string HaConfig::ha_camera_token()  { return Get("ha_cam_tok", HA_CAMERA_TOKEN); }
std::string HaConfig::ha_camera_entity() { return Get("ha_cam_ent", HA_CAMERA_ENTITY); }
std::string HaConfig::ha_camera_motion_sensor() {
    return Get("ha_cam_mot", HA_CAMERA_MOTION_SENSOR);
}

// 派生 URL（运行时组合，不单独存 NVS）
std::string HaConfig::ha_camera_ptz_url() {
    return ha_camera_url() + "/api/services/onvif/ptz";
}
std::string HaConfig::ha_camera_proxy_url() {
    return ha_camera_url() + "/api/camera_proxy/" + ha_camera_entity() + "?width=640";
}
std::string HaConfig::ha_camera_proxy_small_url() {
    return ha_camera_url() + "/api/camera_proxy/" + ha_camera_entity() + "?width=600";
}
std::string HaConfig::ha_camera_motion_url() {
    return ha_camera_url() + "/api/states/" + ha_camera_motion_sensor();
}

std::vector<HaConfig::CustomDevice> HaConfig::GetCustomDevices() {
    std::string raw = Get("custom_devs", "[]");
    std::vector<CustomDevice> result;
    cJSON* arr = cJSON_Parse(raw.c_str());
    if (!arr) return result;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        auto gs = [&](const char* k) -> std::string {
            cJSON* v = cJSON_GetObjectItem(item, k);
            return (v && v->valuestring) ? v->valuestring : "";
        };
        CustomDevice d;
        d.id     = gs("id");
        d.name   = gs("name");
        d.entity = gs("entity");
        d.ha     = gs("ha");
        d.domain = gs("domain");
        if (!d.id.empty() && !d.entity.empty()) result.push_back(d);
    }
    cJSON_Delete(arr);
    return result;
}

void HaConfig::SaveCustomDevices(const std::vector<CustomDevice>& devices) {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& d : devices) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",     d.id.c_str());
        cJSON_AddStringToObject(obj, "name",   d.name.c_str());
        cJSON_AddStringToObject(obj, "entity", d.entity.c_str());
        cJSON_AddStringToObject(obj, "ha",     d.ha.c_str());
        cJSON_AddStringToObject(obj, "domain", d.domain.c_str());
        cJSON_AddItemToArray(arr, obj);
    }
    char* str = cJSON_PrintUnformatted(arr);
    Set("custom_devs", str);
    cJSON_free(str);
    cJSON_Delete(arr);
}

std::string HaConfig::ToJson() {
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "ha_old_url", ha_old_url().c_str());
    cJSON_AddStringToObject(root, "ha_old_tok", ha_old_token().c_str());
    cJSON_AddStringToObject(root, "e_main_sw",  entity_main_switch().c_str());
    cJSON_AddStringToObject(root, "e_tv",       entity_tv().c_str());
    cJSON_AddStringToObject(root, "e_gas",      entity_gas_valve().c_str());
    cJSON_AddStringToObject(root, "e_water",    entity_water_valve().c_str());
    cJSON_AddStringToObject(root, "ha_new_url", ha_new_url().c_str());
    cJSON_AddStringToObject(root, "ha_new_tok", ha_new_token().c_str());
    cJSON_AddStringToObject(root, "e_plug",     entity_smart_plug().c_str());
    cJSON_AddStringToObject(root, "e_door",     entity_door_sensor().c_str());
    cJSON_AddStringToObject(root, "e_curtain1", entity_curtain_1().c_str());
    cJSON_AddStringToObject(root, "e_curtain2", entity_curtain_2().c_str());
    cJSON_AddStringToObject(root, "e_speaker",  entity_speaker().c_str());
    cJSON_AddStringToObject(root, "e_spk_tts",  entity_speaker_tts().c_str());
    cJSON_AddStringToObject(root, "e_spk_cmd",  entity_speaker_cmd().c_str());
    cJSON_AddStringToObject(root, "ha_cam_url", ha_camera_url().c_str());
    cJSON_AddStringToObject(root, "ha_cam_tok", ha_camera_token().c_str());
    cJSON_AddStringToObject(root, "ha_cam_ent", ha_camera_entity().c_str());
    cJSON_AddStringToObject(root, "ha_cam_mot", ha_camera_motion_sensor().c_str());

    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
