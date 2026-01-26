#include "otto_emoji_display.h"

#include <esp_log.h>

#include <cstring>

#include "assets/lang_config.h"
#include "display/lvgl_display/emoji_collection.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "otto_emoji_gif.h"

#define TAG "OttoEmojiDisplay"
OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
    InitializeOttoEmojis();
    SetupChatLabel();
    SetupPreviewImage();
}

void OttoEmojiDisplay::SetupPreviewImage() {
    DisplayLockGuard lock(this);
    lv_obj_set_size(preview_image_, width_ , height_ );
}

void OttoEmojiDisplay::InitializeOttoEmojis() {
    ESP_LOGI(TAG, "初始化Otto GIF表情");

    auto otto_emoji_collection = std::make_shared<EmojiCollection>();

    // 中性/平静类表情 -> staticstate
    otto_emoji_collection->AddEmoji("staticstate", new LvglRawImage((void*)staticstate.data, staticstate.data_size));
    otto_emoji_collection->AddEmoji("neutral", new LvglRawImage((void*)staticstate.data, staticstate.data_size));
    otto_emoji_collection->AddEmoji("relaxed", new LvglRawImage((void*)staticstate.data, staticstate.data_size));
    otto_emoji_collection->AddEmoji("sleepy", new LvglRawImage((void*)staticstate.data, staticstate.data_size));
    otto_emoji_collection->AddEmoji("idle", new LvglRawImage((void*)staticstate.data, staticstate.data_size));

    // 积极/开心类表情 -> happy
    otto_emoji_collection->AddEmoji("happy", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("laughing", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("funny", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("loving", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("confident", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("winking", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("cool", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("delicious", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("kissy", new LvglRawImage((void*)happy.data, happy.data_size));
    otto_emoji_collection->AddEmoji("silly", new LvglRawImage((void*)happy.data, happy.data_size));

    // 悲伤类表情 -> sad
    otto_emoji_collection->AddEmoji("sad", new LvglRawImage((void*)sad.data, sad.data_size));
    otto_emoji_collection->AddEmoji("crying", new LvglRawImage((void*)sad.data, sad.data_size));

    // 愤怒类表情 -> anger
    otto_emoji_collection->AddEmoji("anger", new LvglRawImage((void*)anger.data, anger.data_size));
    otto_emoji_collection->AddEmoji("angry", new LvglRawImage((void*)anger.data, anger.data_size));

    // 惊讶类表情 -> scare
    otto_emoji_collection->AddEmoji("scare", new LvglRawImage((void*)scare.data, scare.data_size));
    otto_emoji_collection->AddEmoji("surprised", new LvglRawImage((void*)scare.data, scare.data_size));
    otto_emoji_collection->AddEmoji("shocked", new LvglRawImage((void*)scare.data, scare.data_size));

    // 思考/困惑类表情 -> buxue
    otto_emoji_collection->AddEmoji("buxue", new LvglRawImage((void*)buxue.data, buxue.data_size));
    otto_emoji_collection->AddEmoji("thinking", new LvglRawImage((void*)buxue.data, buxue.data_size));
    otto_emoji_collection->AddEmoji("confused", new LvglRawImage((void*)buxue.data, buxue.data_size));
    otto_emoji_collection->AddEmoji("embarrassed", new LvglRawImage((void*)buxue.data, buxue.data_size));

    // 将表情集合添加到主题中
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");

    if (light_theme != nullptr) {
        light_theme->set_emoji_collection(otto_emoji_collection);
    }
    if (dark_theme != nullptr) {
        dark_theme->set_emoji_collection(otto_emoji_collection);
    }

    // 设置默认表情为staticstate
    SetEmotion("staticstate");

    ESP_LOGI(TAG, "Otto GIF表情初始化完成");
}

void OttoEmojiDisplay::SetupChatLabel() {
    DisplayLockGuard lock(this);

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }

    chat_message_label_ = lv_label_create(container_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, width_ * 0.9);                        // 限制宽度为屏幕宽度的 90%
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);            
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);  // 设置文本居中对齐
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    SetTheme(LvglThemeManager::GetInstance().GetTheme("dark"));
    
    // 设置状态栏布局为无布局(手动定位),并调整电量图标位置
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    
    if (status_bar_) {
        lv_obj_set_size(status_bar_, LV_HOR_RES, text_font->line_height * 2 + 10);
        lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
        lv_obj_set_style_pad_top(status_bar_, 10, 0);
        lv_obj_set_style_pad_bottom(status_bar_, 1, 0);
    }
    
    // 针对圆形屏幕调整位置 (参考sensecap-watcher)
    // 电量图标: 居中靠右一点 (右偏移约1.5个图标宽度)
    if (battery_label_) {
        lv_obj_align(battery_label_, LV_ALIGN_TOP_MID, 1.5 * icon_font->line_height, 0);
    }
    // 网络图标: 居中靠左一点
    if (network_label_) {
        lv_obj_align(network_label_, LV_ALIGN_TOP_MID, -1.5 * icon_font->line_height, 0);
    }
    // 静音图标: 最右侧(如果需要的话)
    if (mute_label_) {
        lv_obj_align(mute_label_, LV_ALIGN_TOP_MID, 3.0 * icon_font->line_height, 0);
    }
    
    // 状态文本: 底部居中
    if (status_label_) {
        lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_grow(status_label_, 0);
        lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    
    // 通知标签: 底部居中
    if (notification_label_) {
        lv_obj_align(notification_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    
    // 低电量提示: 底部居中偏上
    if (low_battery_popup_) {
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -20);
        if (low_battery_label_) {
            lv_obj_set_width(low_battery_label_, LV_HOR_RES * 0.75);
            lv_label_set_long_mode(low_battery_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }
    }
}

LV_FONT_DECLARE(OTTO_ICON_FONT);
// void OttoEmojiDisplay::SetStatus(const char* status) {
//     auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
//     auto text_font = lvgl_theme->text_font()->font();
//     DisplayLockGuard lock(this);
//     if (!status) {
//         ESP_LOGE(TAG, "SetStatus: status is nullptr");
//         return;
//     }

//     if (strcmp(status, Lang::Strings::LISTENING) == 0) {
//         lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
//         lv_label_set_text(status_label_, "\xEF\x84\xB0");  // U+F130 麦克风图标
//         lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
//         lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
//         lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
//         return;
//     } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
//         lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
//         lv_label_set_text(status_label_, "\xEF\x80\xA8");  // U+F028 说话图标
//         lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
//         lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
//         lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
//         return;
//     } else if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
//         lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
//         lv_label_set_text(status_label_, "\xEF\x83\x81");  // U+F0c1 连接图标
//         lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
//         return;
//     } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
//         lv_obj_set_style_text_font(status_label_, text_font, 0);
//         lv_label_set_text(status_label_, "");
//         lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
//         return;
//     }

//     lv_obj_set_style_text_font(status_label_, text_font, 0);
//     lv_label_set_text(status_label_, status);
//     lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
//     lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
//     lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
// }

void OttoEmojiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    // 设置图片源并显示预览图片
    lv_image_set_src(preview_image_, img_dsc);
    lv_image_set_rotation(preview_image_, -900);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 1.0
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}