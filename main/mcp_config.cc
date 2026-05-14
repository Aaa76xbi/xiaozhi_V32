#include "mcp_config.h"
#include "settings.h"
#include <cJSON.h>

std::string McpConfig::LoadRaw() {
    Settings s(NS);
    return s.GetString(KEY, "{}");
}

void McpConfig::SaveRaw(const std::string& json) {
    Settings s(NS, true);
    s.SetString(KEY, json);
}

ToolOverride McpConfig::GetOverride(const std::string& original_name) {
    ToolOverride result;
    std::string raw = LoadRaw();
    cJSON* root = cJSON_Parse(raw.c_str());
    if (!root) return result;

    cJSON* item = cJSON_GetObjectItem(root, original_name.c_str());
    if (item) {
        cJSON* e = cJSON_GetObjectItem(item, "e");
        cJSON* a = cJSON_GetObjectItem(item, "a");
        cJSON* d = cJSON_GetObjectItem(item, "d");
        cJSON* n = cJSON_GetObjectItem(item, "n");
        if (e) result.enabled     = cJSON_IsTrue(e);
        if (a && a->valuestring)  result.alias       = a->valuestring;
        if (d && d->valuestring)  result.description = d->valuestring;
        if (n && n->valuestring)  result.note        = n->valuestring;
    }
    cJSON_Delete(root);
    return result;
}

void McpConfig::FromJson(const std::string& json) {
    // 验证是合法 JSON 对象后直接存储
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return;

    // 只保留工具名 → {e,a,d} 结构，去掉其他字段
    cJSON* clean = cJSON_CreateObject();
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) continue;
        cJSON* entry = cJSON_CreateObject();
        cJSON* e = cJSON_GetObjectItem(item, "e");
        cJSON* a = cJSON_GetObjectItem(item, "a");
        cJSON* d = cJSON_GetObjectItem(item, "d");
        cJSON* n = cJSON_GetObjectItem(item, "n");
        cJSON_AddBoolToObject(entry, "e", e ? cJSON_IsTrue(e) : true);
        cJSON_AddStringToObject(entry, "a", (a && a->valuestring) ? a->valuestring : "");
        cJSON_AddStringToObject(entry, "d", (d && d->valuestring) ? d->valuestring : "");
        cJSON_AddStringToObject(entry, "n", (n && n->valuestring) ? n->valuestring : "");
        cJSON_AddItemToObject(clean, item->string, entry);
    }
    cJSON_Delete(root);

    char* str = cJSON_PrintUnformatted(clean);
    SaveRaw(std::string(str));
    cJSON_free(str);
    cJSON_Delete(clean);
}

std::string McpConfig::ToJson() {
    return LoadRaw();
}
