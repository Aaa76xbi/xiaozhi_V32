#ifndef MCP_CONFIG_H
#define MCP_CONFIG_H

#include <string>

struct ToolOverride {
    bool        enabled     = true;
    std::string alias;        // 非空时 AI 看到此名称而非原始名
    std::string description;  // 非空时覆盖原始描述
    std::string note;         // 管理员备注（不发给 AI，仅 Web UI 显示）
};

class McpConfig {
public:
    static McpConfig& GetInstance() {
        static McpConfig instance;
        return instance;
    }

    // 读取指定工具的覆盖配置（无记录则返回默认值 enabled=true）
    ToolOverride GetOverride(const std::string& original_name);

    // 从 JSON 字符串批量导入（ConfigServer POST /api/tools 调用）
    // JSON 格式: {"tool_name":{"e":1,"a":"alias","d":"desc"}, ...}
    void FromJson(const std::string& json);

    // 返回所有已保存配置的 JSON（ConfigServer GET /api/config 调用）
    std::string ToJson();

private:
    McpConfig() = default;

    // NVS namespace: "mcp_cfg", key: "data"，存整个 JSON blob
    static constexpr const char* NS  = "mcp_cfg";
    static constexpr const char* KEY = "data";

    std::string LoadRaw();
    void        SaveRaw(const std::string& json);
};

#endif // MCP_CONFIG_H
