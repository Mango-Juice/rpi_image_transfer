#ifndef // Image header structure matching kernel driver
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint32_t data_length;
    uint16_t header_checksum;
} __attribute__((packed)) image_header_t;
_EPAPER_DATA_H
#define RECEIVE_EPAPER_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    uint32_t width;
    uint32_t height;
    unsigned char *data;
    size_t data_size;
} epaper_image_t;

typedef struct
{
    bool save_raw;
    const char *output_path;
    bool verbose;
    int timeout_ms;
} epaper_receive_options_t;

int epaper_rx_open(const char *device_path);
void epaper_rx_close(int fd);
bool epaper_receive_image(int fd, epaper_image_t *image);
bool epaper_receive_image_advanced(int fd, epaper_image_t *image, const epaper_receive_options_t *options);
bool epaper_save_image_raw(const epaper_image_t *image, const char *filename);
bool epaper_save_image_pbm(const epaper_image_t *image, const char *filename);
void epaper_free_image(epaper_image_t *image);

#endif
