#include "send_epaper_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <math.h>
#include <errno.h>

// Define ECOMM if not available
#ifndef ECOMM
#define ECOMM 70
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int epaper_open(const char* device_path) {
    int fd = open(device_path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open device");
    }
    return fd;
}

void epaper_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

static unsigned char rgb_to_gray(const unsigned char *pixel, int channels) {
    if (channels == 1) {
        return pixel[0];
    } else if (channels >= 3) {
        return (unsigned char)(0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2]);
    } else {
        return pixel[0];
    }
}

static void resize_image(const unsigned char *src, int src_w, int src_h, int channels,
                        unsigned char *dst, int dst_w, int dst_h) {
    float x_ratio = (float)src_w / dst_w;
    float y_ratio = (float)src_h / dst_h;
    
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);
            
            if (src_x >= src_w) src_x = src_w - 1;
            if (src_y >= src_h) src_y = src_h - 1;
            
            for (int c = 0; c < channels; c++) {
                dst[(y * dst_w + x) * channels + c] = 
                    src[(src_y * src_w + src_x) * channels + c];
            }
        }
    }
}

static void apply_dithering(float *gray, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float old_pixel = gray[y * width + x];
            float new_pixel = old_pixel > 127.5f ? 255.0f : 0.0f;
            gray[y * width + x] = new_pixel;
            
            float quant_error = old_pixel - new_pixel;
            
            if (x + 1 < width)
                gray[y * width + (x + 1)] += quant_error * 7.0f / 16.0f;
            if (y + 1 < height) {
                if (x > 0)
                    gray[(y + 1) * width + (x - 1)] += quant_error * 3.0f / 16.0f;
                gray[(y + 1) * width + x] += quant_error * 5.0f / 16.0f;
                if (x + 1 < width)
                    gray[(y + 1) * width + (x + 1)] += quant_error * 1.0f / 16.0f;
            }
        }
    }
}

static bool send_with_progress(int fd, const unsigned char *data, size_t size) {
    const size_t chunk_size = 1024;
    
    for (size_t sent = 0; sent < size; sent += chunk_size) {
        size_t to_send = (sent + chunk_size > size) ? (size - sent) : chunk_size;
        
        ssize_t bytes_written = write(fd, data + sent, to_send);
        if (bytes_written != (ssize_t)to_send) {
            if (bytes_written < 0) {
                switch (errno) {
                case ETIMEDOUT:
                    fprintf(stderr, "\nError: Connection timeout at byte %zu/%zu\n", sent, size);
                    break;
                case ECOMM:
                    fprintf(stderr, "\nError: Communication error (NACK) at byte %zu/%zu\n", sent, size);
                    break;
                case EHOSTUNREACH:
                    fprintf(stderr, "\nError: Receiver not reachable at byte %zu/%zu\n", sent, size);
                    break;
                case ECONNREFUSED:
                    fprintf(stderr, "\nError: Connection refused at byte %zu/%zu\n", sent, size);
                    break;
                case ECONNRESET:
                    fprintf(stderr, "\nError: Connection reset by receiver at byte %zu/%zu\n", sent, size);
                    break;
                default:
                    fprintf(stderr, "\nWrite failed at byte %zu/%zu: %s\n", sent, size, strerror(errno));
                    break;
                }
            } else {
                fprintf(stderr, "\nError: Partial write at byte %zu/%zu (%zd/%zu bytes written)\n", 
                       sent, size, bytes_written, to_send);
            }
            return false;
        }
        
        int progress = (sent * 100) / size;
        printf("\rProgress: %d%% (%zu/%zu bytes)", progress, sent, size);
        fflush(stdout);
        usleep(1000);
    }
    printf("\n");
    return true;
}

bool epaper_send_image(int fd, const char* image_path) {
    return epaper_send_image_advanced(fd, image_path, NULL);
}
bool epaper_send_image_resized(int fd, const char *image_path, int target_width, int target_height) {
    epaper_convert_options_t options = {
        .target_width = target_width,
        .target_height = target_height,
        .use_dithering = false,
        .invert_colors = false,
        .threshold = 128
    };
    return epaper_send_image_advanced(fd, image_path, &options);
}

bool epaper_send_image_advanced(int fd, const char *image_path, const epaper_convert_options_t *options) {
    int width, height, channels;
    
    unsigned char *img = stbi_load(image_path, &width, &height, &channels, 0);
    if (img == NULL) {
        fprintf(stderr, "Error: Failed to load image %s\n", image_path);
        return false;
    }
    
    if (channels < 1 || channels > 4) {
        fprintf(stderr, "Error: Unsupported image format (%d channels)\n", channels);
        stbi_image_free(img);
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: Invalid image dimensions (%dx%d)\n", width, height);
        stbi_image_free(img);
        return false;
    }
    
    printf("Image loaded: %dx%d, %d channels\n", width, height, channels);
    
    unsigned char *processed_img = img;
    int final_width = width;
    int final_height = height;
    
    if (options && options->target_width > 0 && options->target_height > 0) {
        if (options->target_width > 10000 || options->target_height > 10000) {
            fprintf(stderr, "Error: Target dimensions too large (%dx%d)\n", 
                   options->target_width, options->target_height);
            stbi_image_free(img);
            return false;
        }
        
        final_width = options->target_width;
        final_height = options->target_height;
        
        if (final_width != width || final_height != height) {
            processed_img = malloc(final_width * final_height * channels);
            if (!processed_img) {
                stbi_image_free(img);
                return false;
            }
            
            resize_image(img, width, height, channels, processed_img, final_width, final_height);
            printf("Resized to: %dx%d\n", final_width, final_height);
        }
    }
    
    size_t mono_size = (final_width * final_height + 7) / 8;
    unsigned char *mono_buffer = malloc(mono_size);
    if (!mono_buffer) {
        if (processed_img != img) free(processed_img);
        stbi_image_free(img);
        return false;
    }
    memset(mono_buffer, 0, mono_size);
    
    printf("Converting to 1-bit monochrome (%zu bytes)...\n", mono_size);
    
    int threshold = (options && options->threshold >= 0 && options->threshold <= 255) ? 
                   options->threshold : 128;
    bool use_dithering = options ? options->use_dithering : false;
    bool invert = options ? options->invert_colors : false;
    
    if (use_dithering) {
        float *gray = malloc(final_width * final_height * sizeof(float));
        if (!gray) {
            free(mono_buffer);
            if (processed_img != img) free(processed_img);
            stbi_image_free(img);
            return false;
        }
        
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                unsigned char* pixel = processed_img + (y * final_width + x) * channels;
                gray[y * final_width + x] = rgb_to_gray(pixel, channels);
                if (invert) gray[y * final_width + x] = 255.0f - gray[y * final_width + x];
            }
        }
        
        apply_dithering(gray, final_width, final_height);
        
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                if (gray[y * final_width + x] < 127.5f) {
                    int byte_index = (y * final_width + x) / 8;
                    int bit_index = 7 - ((y * final_width + x) % 8);
                    mono_buffer[byte_index] |= (1 << bit_index);
                }
            }
        }
        
        free(gray);
    } else {
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                unsigned char* pixel = processed_img + (y * final_width + x) * channels;
                int avg = rgb_to_gray(pixel, channels);
                if (invert) avg = 255 - avg;
                
                if (avg < threshold) {
                    int byte_index = (y * final_width + x) / 8;
                    int bit_index = 7 - ((y * final_width + x) % 8);
                    mono_buffer[byte_index] |= (1 << bit_index);
                }
            }
        }
    }
    
    if (processed_img != img) free(processed_img);
    stbi_image_free(img);
    
    // Create image header for new protocol
    if (final_width > 65535 || final_height > 65535) {
        fprintf(stderr, "Error: Image dimensions too large for protocol (max 65535x65535)\n");
        free(mono_buffer);
        return false;
    }
    
    if (mono_size > 0xFFFFFFFF) {
        fprintf(stderr, "Error: Image data too large for protocol\n");
        free(mono_buffer);
        return false;
    }
    
    image_header_t header;
    header.width = (uint16_t)final_width;
    header.height = (uint16_t)final_height;
    header.data_length = (uint32_t)mono_size;
    header.header_checksum = 0; // Will be calculated by kernel driver
    
    // Create buffer for header + image data
    size_t total_size = sizeof(header) + mono_size;
    unsigned char *send_buffer = malloc(total_size);
    if (!send_buffer) {
        fprintf(stderr, "Error: Failed to allocate send buffer\n");
        free(mono_buffer);
        return false;
    }
    
    // Copy header and image data to send buffer
    memcpy(send_buffer, &header, sizeof(header));
    memcpy(send_buffer + sizeof(header), mono_buffer, mono_size);
    
    printf("Sending image: %dx%d, %zu bytes data\n", final_width, final_height, mono_size);
    printf("Total transmission: %zu bytes (header + data)\n", total_size);
    
    // Send everything at once - kernel driver will handle protocol details
    bool success = send_with_progress(fd, send_buffer, total_size);
    
    if (success) {
        printf("Successfully sent image\n");
    }
    
    free(send_buffer);
    free(mono_buffer);
    return success;
}