// ledc_channel_manager.h

#ifdef __cplusplus
extern "C" {
#endif

// 申请一个空闲通道，返回通道号（0~7/15），失败返回-1
int ledc_channel_alloc();
// 释放通道
void ledc_channel_free(int channel);

#ifdef __cplusplus
}
#endif