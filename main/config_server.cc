#include "config_server.h"
#include "mcp_config.h"
#include "board.h"
#include "display.h"
#include <esp_netif.h>
#include "ha_config.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <string>
#include <cstring>

static const char* TAG = "ConfigServer";

#define AUTH_NS  "cfg_auth"
#define AUTH_KEY_USER "user"
#define AUTH_KEY_PASS "pass"
#define DEFAULT_USER "admin"
#define DEFAULT_PASS "admin123"

// ─────────────────────────────────────────────────────────────
// 嵌入式 HTML 页面（约 12KB，存 .rodata/Flash，不占 RAM）
// ─────────────────────────────────────────────────────────────
static const char PAGE_HTML[] =
"<!DOCTYPE html><html lang=\"zh\"><head>"
"<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>小智 AI 配置</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:sans-serif;background:#f0f2f5;color:#222}"
"h1{background:#1677ff;color:#fff;padding:14px 20px;font-size:18px}"
".tabs{display:flex;background:#fff;border-bottom:2px solid #1677ff}"
".tab{padding:10px 20px;cursor:pointer;font-size:14px;border:none;background:none;color:#555}"
".tab.active{color:#1677ff;border-bottom:2px solid #1677ff;margin-bottom:-2px;font-weight:bold}"
".pane{display:none;padding:16px}"
".pane.active{display:block}"
"table{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 4px #0001}"
"th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #f0f0f0;font-size:13px}"
"th{background:#fafafa;color:#888;font-weight:normal}"
"input[type=text],input[type=password]{width:100%;border:1px solid #d9d9d9;border-radius:4px;padding:5px 8px;font-size:13px}"
"select{width:100%;border:1px solid #d9d9d9;border-radius:4px;padding:5px 8px;font-size:13px;background:#fff}"
"input[type=checkbox]{width:18px;height:18px;cursor:pointer}"
".btn{padding:8px 20px;border:none;border-radius:4px;cursor:pointer;font-size:14px}"
".btn-primary{background:#1677ff;color:#fff}"
".btn-success{background:#52c41a;color:#fff}"
".btn-danger{background:#ff4d4f;color:#fff}"
".btn-sm{padding:4px 10px;font-size:12px}"
".bar{display:flex;justify-content:flex-end;gap:8px;padding:12px 0 4px}"
".bar-left{display:flex;justify-content:space-between;align-items:center;padding:8px 0}"
".msg{padding:10px;border-radius:4px;margin-bottom:12px;display:none}"
".msg.ok{background:#f6ffed;border:1px solid #b7eb8f;color:#389e0d}"
".msg.err{background:#fff2f0;border:1px solid #ffccc7;color:#cf1322}"
".section{background:#fff;border-radius:8px;box-shadow:0 1px 4px #0001;padding:16px;margin-bottom:16px}"
".section h3{font-size:14px;color:#888;margin-bottom:12px;border-bottom:1px solid #f0f0f0;padding-bottom:8px}"
".form-row{display:flex;align-items:center;margin-bottom:10px;gap:8px}"
".form-row label{flex:0 0 200px;font-size:13px;color:#555}"
".form-row input{flex:1}"
".note-col{color:#999;font-size:12px}"
"</style></head><body>"
"<h1>小智 AI 后台配置</h1>"
"<div class=\"tabs\">"
"<button class=\"tab active\" onclick=\"showTab('tools',this)\">MCP 工具</button>"
"<button class=\"tab\" onclick=\"showTab('ha',this)\">HA 配置</button>"
"<button class=\"tab\" onclick=\"showTab('auth',this)\">修改密码</button>"
"</div>"

// ── 工具管理面板 ──
"<div id=\"pane-tools\" class=\"pane active\">"
"<div id=\"msg-tools\" class=\"msg\"></div>"
"<div class=\"bar\">"
"<button class=\"btn btn-primary btn-sm\" onclick=\"saveTools()\">保存工具配置</button>"
"<button class=\"btn btn-danger btn-sm\" onclick=\"reboot()\">保存并重启</button>"
"</div>"
"<table><thead><tr>"
"<th style=\"width:40px\">启用</th>"
"<th style=\"width:220px\">原始工具名</th>"
"<th style=\"width:160px\">别名（AI看到的）</th>"
"<th>描述覆盖（留空用原始）</th>"
"<th style=\"width:200px\">管理员备注</th>"
"</tr></thead><tbody id=\"tool-rows\"></tbody></table>"
"</div>"

// ── HA 配置面板 ──
"<div id=\"pane-ha\" class=\"pane\">"
"<div id=\"msg-ha\" class=\"msg\"></div>"
"<div class=\"section\"><h3>旧设备（外网 HA）</h3><div id=\"ha-old\"></div></div>"
"<div class=\"section\"><h3>新设备（本地 HA）</h3><div id=\"ha-new\"></div></div>"
"<div class=\"section\"><h3>摄像头</h3><div id=\"ha-cam\"></div></div>"
// 自定义设备
"<div class=\"section\">"
"<div class=\"bar-left\">"
"<h3 style=\"margin:0;border:none;padding:0\">自定义设备（保存并重启后自动生成 MCP 工具）</h3>"
"<button class=\"btn btn-success btn-sm\" onclick=\"addDevice()\">+ 新增设备</button>"
"</div>"
"<table style=\"margin-top:10px\"><thead><tr>"
"<th>设备ID</th><th>显示名称</th><th>实体 ID（entity_id）</th>"
"<th>HA实例</th><th>服务域名</th><th style=\"width:60px\">操作</th>"
"</tr></thead><tbody id=\"dev-rows\"></tbody></table>"
"<p style=\"font-size:12px;color:#aaa;margin-top:8px\">"
"设备ID只能含字母/数字/下划线，工具名自动为 control_设备ID。重启后可在MCP工具页看到新工具。"
"</p></div>"
"<div class=\"bar\">"
"<button class=\"btn btn-primary\" onclick=\"saveHa()\">保存 HA 配置</button>"
"<button class=\"btn btn-danger\" onclick=\"reboot()\">保存并重启</button>"
"</div></div>"

// ── 密码面板 ──
"<div id=\"pane-auth\" class=\"pane\">"
"<div id=\"msg-auth\" class=\"msg\"></div>"
"<div class=\"section\"><h3>修改登录密码</h3>"
"<div class=\"form-row\"><label>用户名</label><input id=\"auth-user\" type=\"text\"></div>"
"<div class=\"form-row\"><label>新密码</label><input id=\"auth-pass\" type=\"password\"></div>"
"<div class=\"form-row\"><label>确认密码</label><input id=\"auth-pass2\" type=\"password\"></div>"
"<div class=\"bar\"><button class=\"btn btn-primary\" onclick=\"saveAuth()\">保存密码</button></div>"
"</div></div>"

"<script>"
// ── 工具管理员备注（内置工具中文说明）──
"const TOOL_NOTES={"
"'self.get_device_status':'获取设备实时状态（音量/亮度/电量/网络等）',"
"'self.audio_speaker.set_volume':'设置音箱音量（0-100）',"
"'self.screen.set_brightness':'设置屏幕亮度（0-100）',"
"'self.screen.set_theme':'切换屏幕主题 light/dark',"
"'self.ws2812.set_color':'设置LED灯带颜色（R/G/B各0-255）',"
"'self.ws2812.on':'打开LED灯效/灯带/灯箱',"
"'self.ws2812.off':'关闭LED灯效/灯带/灯箱',"
"'self.get_system_info':'获取系统信息（内存/芯片/版本）',"
"'self.reboot':'重启设备',"
"'self.upgrade_firmware':'从指定URL升级固件',"
"'self.screen.get_info':'获取屏幕分辨率（宽/高/是否单色）',"
"'self.screen.snapshot':'截图并上传到指定URL',"
"'self.screen.preview_image':'在屏幕上预览图片',"
"'self.assets.set_download_url':'设置资源文件下载地址',"
"'control_home_device':'控制家电：插座/门磁/电视/水阀/气阀/总闸',"
"'control_ha_camera':'控制客厅摄像头云台方向（上/下/左/右）',"
"'describe_ha_camera':'让AI查看并描述摄像头画面，entity_id留空=主摄像头，可指定其他摄像头实体ID',"
"'show_camera_on_screen':'把摄像头画面截图显示到屏幕，entity_id留空=主摄像头，可指定其他摄像头实体ID',"
"'control_curtain':'控制窗帘（打开/关闭/停止/设置开合度）',"
"'control_speaker':'控制小米音箱播放（播放/暂停/切歌/音量）',"
"'speaker_say':'让小米音箱朗读文字或执行语音指令',"
"'call_emergency':'紧急求救，拨打救援电话',"
"'sleep_mode':'睡眠模式：检查并关闭所有设备',"
"'set_reminder':'设置提醒/闹钟（需提供内容和延迟秒数）',"
"'list_reminders':'查询所有待触发的提醒列表',"
"'cancel_reminder':'取消指定提醒（按内容关键词或编号）',"
"'station_call_connect':'连接总台语音对讲',"
"'station_call_play_reply':'播放总台留言',"
"'disconnect_station':'断开总台通话',"
"'self.camera.take_photo':'用设备自带摄像头拍照并让AI描述'"
"};"

// HA 字段标签映射
"const HA_LABELS={"
"ha_old_url:'旧 HA URL（外网，含/api）',"
"ha_old_tok:'旧 HA Token',"
"e_main_sw:'总闸实体 ID',"
"e_tv:'电视实体 ID',"
"e_gas:'气阀实体 ID',"
"e_water:'水阀实体 ID',"
"ha_new_url:'新 HA URL（本地，含/api）',"
"ha_new_tok:'新 HA Token',"
"e_plug:'智能插座实体 ID',"
"e_door:'门磁实体 ID',"
"e_curtain1:'窗帘1实体 ID',"
"e_curtain2:'窗帘2实体 ID',"
"e_speaker:'音箱播放实体 ID',"
"e_spk_tts:'音箱TTS实体 ID',"
"e_spk_cmd:'音箱指令实体 ID',"
"ha_cam_url:'摄像头 HA URL（无/api后缀）',"
"ha_cam_tok:'摄像头 Token',"
"ha_cam_ent:'摄像头实体 ID',"
"ha_cam_mot:'人体检测传感器实体 ID'"
"};"
"const HA_OLD=['ha_old_url','ha_old_tok','e_main_sw','e_tv','e_gas','e_water'];"
"const HA_NEW=['ha_new_url','ha_new_tok','e_plug','e_door','e_curtain1','e_curtain2','e_speaker','e_spk_tts','e_spk_cmd'];"
"const HA_CAM=['ha_cam_url','ha_cam_tok','ha_cam_ent','ha_cam_mot'];"

"let cfg={};"
"function showTab(id,el){"
"document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
"document.querySelectorAll('.pane').forEach(p=>p.classList.remove('active'));"
"el.classList.add('active');"
"document.getElementById('pane-'+id).classList.add('active');}"

"function showMsg(id,ok,txt){"
"const el=document.getElementById('msg-'+id);"
"el.className='msg '+(ok?'ok':'err');"
"el.textContent=txt;el.style.display='block';"
"setTimeout(()=>el.style.display='none',4000);}"

"function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"

// 加载配置
"async function load(){"
"const r=await fetch('/api/config');cfg=await r.json();"
"renderTools();renderHa();renderDevices();}"

// 渲染工具表格（含管理员备注列）
"function renderTools(){"
"const tb=document.getElementById('tool-rows');"
"tb.innerHTML='';"
"const tools=cfg.tools||[];"
"const overrides=cfg.tool_overrides||{};"
"tools.forEach(t=>{"
"const ov=overrides[t.name]||{};"
"const en=ov.e!==undefined?ov.e:true;"
"const alias=ov.a||'';"
"const desc=ov.d||'';"
"const note=ov.n||'';"
"const notePh=TOOL_NOTES[t.name]||'';"
"const tr=document.createElement('tr');"
"tr.innerHTML="
"'<td><input type=\"checkbox\" data-name=\"'+t.name+'\" class=\"cb-en\"'+(en?' checked':'')+'></td>'"
"+'<td style=\"font-family:monospace;font-size:11px\">'+escHtml(t.name)+'</td>'"
"+'<td><input type=\"text\" class=\"inp-alias\" value=\"'+escHtml(alias)+'\"></td>'"
"+'<td><input type=\"text\" class=\"inp-desc\" placeholder=\"'+escHtml(t.description||'')+'\"></td>'"
"+'<td><input type=\"text\" class=\"inp-note note-col\" placeholder=\"'+escHtml(notePh)+'\"></td>';"
"tr.querySelector('.inp-desc').value=desc;"
"tr.querySelector('.inp-note').value=note;"
"tr.querySelector('.cb-en').dataset.name=t.name;"
"tb.appendChild(tr);});}"

// 渲染 HA 固定字段
"function renderHa(){"
"const ha=cfg.ha||{};"
"['old','new','cam'].forEach(sec=>{"
"const keys=sec==='old'?HA_OLD:sec==='new'?HA_NEW:HA_CAM;"
"const el=document.getElementById('ha-'+sec);"
"el.innerHTML=keys.map(k=>"
"'<div class=\"form-row\"><label>'+HA_LABELS[k]+'</label>'"
"+'<input type=\"text\" id=\"ha-'+k+'\" value=\"'+escHtml(ha[k]||'')+'\"></div>'"
").join('');});}"

// 渲染自定义设备表格
"function renderDevices(){"
"const devs=(cfg.ha&&cfg.ha.custom_devices)||[];"
"const tb=document.getElementById('dev-rows');"
"tb.innerHTML='';"
"devs.forEach((d,i)=>{"
"const haOpts=['old','new'].map(v=>'<option value=\"'+v+'\"'+(d.ha===v?' selected':'')+'>'+( v==='old'?'外网旧HA':'本地新HA')+'</option>').join('');"
"const domList=['switch','cover','climate','light','fan','media_player','camera','camera_ptz'];"
"const domOpts=domList.map(v=>'<option value=\"'+v+'\"'+(d.domain===v?' selected':'')+'>'+v+'</option>').join('');"
"const tr=document.createElement('tr');"
"tr.innerHTML="
"'<td><input type=\"text\" class=\"d-id\" value=\"'+escHtml(d.id||'')+'\" placeholder=\"唯一ID\"></td>'"
"+'<td><input type=\"text\" class=\"d-name\" value=\"'+escHtml(d.name||'')+'\" placeholder=\"如空调\"></td>'"
"+'<td><input type=\"text\" class=\"d-entity\" value=\"'+escHtml(d.entity||'')+'\" placeholder=\"climate.xxx\"></td>'"
"+'<td><select class=\"d-ha\">'+haOpts+'</select></td>'"
"+'<td><select class=\"d-domain\">'+domOpts+'</select></td>'"
"+'<td><button class=\"btn btn-danger btn-sm\" onclick=\"deleteDevice(this)\">删除</button></td>';"
"tb.appendChild(tr);});}"

// 新增一行自定义设备
"function addDevice(){"
"const tb=document.getElementById('dev-rows');"
"const domList=['switch','cover','climate','light','fan','media_player','camera','camera_ptz'];"
"const domOpts=domList.map(v=>'<option value=\"'+v+'\">'+v+'</option>').join('');"
"const tr=document.createElement('tr');"
"tr.innerHTML="
"'<td><input type=\"text\" class=\"d-id\" placeholder=\"唯一ID\"></td>'"
"+'<td><input type=\"text\" class=\"d-name\" placeholder=\"如空调\"></td>'"
"+'<td><input type=\"text\" class=\"d-entity\" placeholder=\"climate.xxx\"></td>'"
"+'<td><select class=\"d-ha\"><option value=\"new\">本地新HA</option><option value=\"old\">外网旧HA</option></select></td>'"
"+'<td><select class=\"d-domain\">'+domOpts+'</select></td>'"
"+'<td><button class=\"btn btn-danger btn-sm\" onclick=\"deleteDevice(this)\">删除</button></td>';"
"tb.appendChild(tr);}"

// 删除自定义设备行
"function deleteDevice(btn){btn.closest('tr').remove();}"

// 保存工具配置（含管理员备注）
"async function saveTools(){"
"const overrides={};"
"document.querySelectorAll('#tool-rows tr').forEach(tr=>{"
"const cb=tr.querySelector('.cb-en');if(!cb)return;"
"const name=cb.dataset.name;"
"const alias=tr.querySelector('.inp-alias').value.trim();"
"const desc=tr.querySelector('.inp-desc').value.trim();"
"const noteEl=tr.querySelector('.inp-note');"
"const notePh=TOOL_NOTES[name]||'';"
"const note=noteEl.value.trim()||notePh;"
"overrides[name]={e:cb.checked,a:alias,d:desc,n:note};});"
"const r=await fetch('/api/tools',{method:'POST',body:JSON.stringify(overrides)});"
"showMsg('tools',r.ok,r.ok?'保存成功，重启后生效':'保存失败: '+r.status);}"

// 保存 HA 配置（含自定义设备）
"async function saveHa(){"
"const ha={};"
"[...HA_OLD,...HA_NEW,...HA_CAM].forEach(k=>{"
"ha[k]=document.getElementById('ha-'+k).value.trim();});"
"const devRows=document.querySelectorAll('#dev-rows tr');"
"ha.custom_devices=[];"
"devRows.forEach(tr=>{"
"const id=tr.querySelector('.d-id').value.trim();"
"const name=tr.querySelector('.d-name').value.trim();"
"const entity=tr.querySelector('.d-entity').value.trim();"
"const ha_inst=tr.querySelector('.d-ha').value;"
"const domain=tr.querySelector('.d-domain').value;"
"if(id&&entity)ha.custom_devices.push({id,name,entity,ha:ha_inst,domain});});"
"const r=await fetch('/api/ha',{method:'POST',body:JSON.stringify(ha)});"
"showMsg('ha',r.ok,r.ok?'保存成功，重启后生效':'保存失败: '+r.status);}"

// 保存密码
"async function saveAuth(){"
"const user=document.getElementById('auth-user').value.trim();"
"const pass=document.getElementById('auth-pass').value;"
"const pass2=document.getElementById('auth-pass2').value;"
"if(!user||!pass){showMsg('auth',false,'用户名和密码不能为空');return;}"
"if(pass!==pass2){showMsg('auth',false,'两次密码不一致');return;}"
"const r=await fetch('/api/auth',{method:'POST',body:JSON.stringify({user,pass})});"
"showMsg('auth',r.ok,r.ok?'密码已修改，下次登录生效':'保存失败: '+r.status);}"

// 重启
"async function reboot(){"
"if(!confirm('确定要重启设备吗？'))return;"
"await fetch('/api/reboot',{method:'POST'});"
"showMsg('tools',true,'设备正在重启…');}"

"load();"
"</script></body></html>";

// ─────────────────────────────────────────────────────────────
// 工具函数
// ─────────────────────────────────────────────────────────────

static std::string ReadBody(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 8192) return "";
    std::string buf(len, '\0');
    int received = httpd_req_recv(req, buf.data(), len);
    if (received <= 0) return "";
    buf.resize(received);
    return buf;
}

bool ConfigServer::CheckAuth(httpd_req_t* req) {
    // 读取保存的用户名密码
    Settings s(AUTH_NS);
    std::string saved_user = s.GetString(AUTH_KEY_USER, DEFAULT_USER);
    std::string saved_pass = s.GetString(AUTH_KEY_PASS, DEFAULT_PASS);

    // 获取 Authorization 头
    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0) goto unauthorized;

    {
        std::string header(header_len + 1, '\0');
        if (httpd_req_get_hdr_value_str(req, "Authorization", header.data(), header.size()) != ESP_OK)
            goto unauthorized;
        header.resize(header_len);  // 去掉 std::string 末尾多余的 \0，否则 base64 解码失败

        // 必须以 "Basic " 开头
        if (header.rfind("Basic ", 0) != 0) goto unauthorized;

        std::string b64 = header.substr(6);
        // base64 解码
        size_t olen = 0;
        std::string decoded(b64.size(), '\0');
        int ret = mbedtls_base64_decode(
            (unsigned char*)decoded.data(), decoded.size(), &olen,
            (const unsigned char*)b64.data(), b64.size());
        if (ret != 0) goto unauthorized;
        decoded.resize(olen);

        // user:pass 格式
        auto sep = decoded.find(':');
        if (sep == std::string::npos) goto unauthorized;
        std::string user = decoded.substr(0, sep);
        std::string pass = decoded.substr(sep + 1);

        if (user == saved_user && pass == saved_pass) return true;
    }

unauthorized:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"XiaoZhi Config\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

// ─────────────────────────────────────────────────────────────
// HTTP 处理函数
// ─────────────────────────────────────────────────────────────

esp_err_t ConfigServer::HandleRoot(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ConfigServer::HandleGetConfig(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;

    // 合并 tools 列表 + tool_overrides + ha 配置
    cJSON* root = cJSON_CreateObject();

    // tools: 从 McpServer 获取工具信息列表
    cJSON* tools_arr = cJSON_Parse(McpServer::GetInstance().GetAllToolsInfoJson().c_str());
    if (tools_arr) cJSON_AddItemToObject(root, "tools", tools_arr);

    // tool_overrides: 从 McpConfig 获取
    cJSON* overrides = cJSON_Parse(McpConfig::GetInstance().ToJson().c_str());
    if (overrides) cJSON_AddItemToObject(root, "tool_overrides", overrides);

    // ha: 固定字段 + 自定义设备列表
    cJSON* ha = cJSON_Parse(HaConfig::GetInstance().ToJson().c_str());
    if (ha) {
        cJSON* devs_arr = cJSON_CreateArray();
        for (const auto& d : HaConfig::GetInstance().GetCustomDevices()) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "id",     d.id.c_str());
            cJSON_AddStringToObject(obj, "name",   d.name.c_str());
            cJSON_AddStringToObject(obj, "entity", d.entity.c_str());
            cJSON_AddStringToObject(obj, "ha",     d.ha.c_str());
            cJSON_AddStringToObject(obj, "domain", d.domain.c_str());
            cJSON_AddItemToArray(devs_arr, obj);
        }
        cJSON_AddItemToObject(ha, "custom_devices", devs_arr);
        cJSON_AddItemToObject(root, "ha", ha);
    }

    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ConfigServer::HandleSaveTools(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;
    std::string body = ReadBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    McpConfig::GetInstance().FromJson(body);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t ConfigServer::HandleSaveHa(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;
    std::string body = ReadBody(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }
    auto& ha = HaConfig::GetInstance();

    // 处理自定义设备数组
    cJSON* custom = cJSON_GetObjectItem(root, "custom_devices");
    if (cJSON_IsArray(custom)) {
        std::vector<HaConfig::CustomDevice> devices;
        cJSON* d = nullptr;
        cJSON_ArrayForEach(d, custom) {
            auto gs = [&](const char* k) -> std::string {
                cJSON* v = cJSON_GetObjectItem(d, k);
                return (v && v->valuestring) ? v->valuestring : "";
            };
            HaConfig::CustomDevice dev;
            dev.id     = gs("id");
            dev.name   = gs("name");
            dev.entity = gs("entity");
            dev.ha     = gs("ha");
            dev.domain = gs("domain");
            if (!dev.id.empty() && !dev.entity.empty()) devices.push_back(dev);
        }
        ha.SaveCustomDevices(devices);
    }

    // 处理普通字符串字段
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!item->string || strcmp(item->string, "custom_devices") == 0) continue;
        if (item->valuestring) ha.Set(item->string, item->valuestring);
    }
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t ConfigServer::HandleSaveAuth(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_OK;
    }
    cJSON* user_j = cJSON_GetObjectItem(root, "user");
    cJSON* pass_j = cJSON_GetObjectItem(root, "pass");
    if (user_j && user_j->valuestring && pass_j && pass_j->valuestring) {
        Settings s(AUTH_NS, true);
        s.SetString(AUTH_KEY_USER, user_j->valuestring);
        s.SetString(AUTH_KEY_PASS, pass_j->valuestring);
    }
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t ConfigServer::HandleReboot(httpd_req_t* req) {
    if (!CheckAuth(req)) return ESP_OK;
    httpd_resp_sendstr(req, "rebooting");
    esp_restart();
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────

void ConfigServer::Start() {
    if (server_) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",           HTTP_GET,  HandleRoot,       nullptr },
        { "/api/config", HTTP_GET,  HandleGetConfig,  nullptr },
        { "/api/tools",  HTTP_POST, HandleSaveTools,  nullptr },
        { "/api/ha",     HTTP_POST, HandleSaveHa,     nullptr },
        { "/api/auth",   HTTP_POST, HandleSaveAuth,   nullptr },
        { "/api/reboot", HTTP_POST, HandleReboot,     nullptr },
    };
    for (const auto& r : routes) {
        httpd_register_uri_handler(server_, &r);
    }
    // 获取本机 IP 并打印日志 + 显示到屏幕
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[20];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "Config server: http://%s", ip_str);

        // 在设备屏幕上显示 IP（4 秒后消失）
        auto* display = Board::GetInstance().GetDisplay();
        if (display) {
            std::string msg = "配置后台:\nhttp://";
            msg += ip_str;
            display->ShowNotification(msg.c_str(), 8000);
        }
    } else {
        ESP_LOGI(TAG, "Config server started on port 80");
    }
}

void ConfigServer::Stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
