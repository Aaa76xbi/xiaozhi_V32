// ledc_channel_manager.c
#include <stdbool.h>
#define LEDC_CHANNEL_MAX 8 // 

static bool ledc_channel_used[LEDC_CHANNEL_MAX] = {false};

int ledc_channel_alloc() {
    for (int i = 0; i < LEDC_CHANNEL_MAX; ++i) {
        if (!ledc_channel_used[i]) {
            ledc_channel_used[i] = true;
            return i;
        }
    }
    return -1;
}

void ledc_channel_free(int channel) {
    if (channel >= 0 && channel < LEDC_CHANNEL_MAX) {
        ledc_channel_used[channel] = false;
    }
}