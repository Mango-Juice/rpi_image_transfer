#ifndef SEND_EPAPER_DATA_H
#define SEND_EPAPER_DATA_H

#include <stdbool.h>
#include <stdint.h>

// Image header structure matching kernel driver
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t data_length;
    uint32_t header_checksum;
} __attribute__((packed)) image_header_t;

typedef struct
{
    int target_width;
    int target_height;
    bool use_dithering;
    bool invert_colors;
    int threshold;
} epaper_convert_options_t;

int epaper_open(const char *device_path);
void epaper_close(int fd);
bool epaper_send_image(int fd, const char *image_path);
bool epaper_send_image_resized(int fd, const char *image_path, int target_width, int target_height);
bool epaper_send_image_advanced(int fd, const char *image_path, const epaper_convert_options_t *options);

#endif