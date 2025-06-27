#include "receive_epaper_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

int epaper_rx_open(const char* device_path) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open RX device");
    }
    return fd;
}

void epaper_rx_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

static bool wait_for_data(int fd, int timeout_ms) {
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0
    };
    
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        perror("Poll failed");
        return false;
    }
    if (ret == 0) {
        fprintf(stderr, "Timeout waiting for data\n");
        return false;
    }
    
    return (pfd.revents & POLLIN) != 0;
}

static bool read_exact(int fd, void *buffer, size_t size, int timeout_ms) {
    unsigned char *buf = (unsigned char *)buffer;
    size_t total_read = 0;
    
    while (total_read < size) {
        if (!wait_for_data(fd, timeout_ms)) {
            return false;
        }
        
        ssize_t bytes_read = read(fd, buf + total_read, size - total_read);
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("Read failed");
            return false;
        }
        if (bytes_read == 0) {
            fprintf(stderr, "Unexpected EOF\n");
            return false;
        }
        
        total_read += bytes_read;
    }
    
    return true;
}

bool epaper_receive_image(int fd, epaper_image_t *image) {
    epaper_receive_options_t default_options = {
        .save_raw = false,
        .output_path = NULL,
        .verbose = false,
        .timeout_ms = 30000
    };
    return epaper_receive_image_advanced(fd, image, &default_options);
}

bool epaper_receive_image_advanced(int fd, epaper_image_t *image, const epaper_receive_options_t *options) {
    if (!image) {
        fprintf(stderr, "Error: Invalid image pointer\n");
        return false;
    }
    
    memset(image, 0, sizeof(epaper_image_t));
    
    int timeout = options ? options->timeout_ms : 30000;
    bool verbose = options ? options->verbose : false;
    
    if (verbose) {
        printf("Waiting for image dimensions...\n");
    }
    
    uint32_t net_width, net_height;
    if (!read_exact(fd, &net_width, sizeof(net_width), timeout) ||
        !read_exact(fd, &net_height, sizeof(net_height), timeout)) {
        fprintf(stderr, "Failed to read image dimensions\n");
        return false;
    }
    
    image->width = ntohl(net_width);
    image->height = ntohl(net_height);
    
    if (image->width == 0 || image->height == 0 || 
        image->width > 10000 || image->height > 10000) {
        fprintf(stderr, "Invalid image dimensions: %ux%u\n", image->width, image->height);
        return false;
    }
    
    image->data_size = (image->width * image->height + 7) / 8;
    image->data = malloc(image->data_size);
    if (!image->data) {
        fprintf(stderr, "Failed to allocate memory for image data\n");
        return false;
    }
    
    if (verbose) {
        printf("Receiving image: %ux%u (%zu bytes)\n", 
               image->width, image->height, image->data_size);
    }
    
    size_t received = 0;
    while (received < image->data_size) {
        if (!wait_for_data(fd, timeout)) {
            free(image->data);
            image->data = NULL;
            return false;
        }
        
        ssize_t bytes_read = read(fd, image->data + received, image->data_size - received);
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("Read failed");
            free(image->data);
            image->data = NULL;
            return false;
        }
        if (bytes_read == 0) {
            fprintf(stderr, "Unexpected EOF during image data reception\n");
            free(image->data);
            image->data = NULL;
            return false;
        }
        
        received += bytes_read;
        
        if (verbose && received % 1024 == 0) {
            int progress = (received * 100) / image->data_size;
            printf("\rProgress: %d%% (%zu/%zu bytes)", progress, received, image->data_size);
            fflush(stdout);
        }
    }
    
    if (verbose) {
        printf("\nImage received successfully!\n");
    }
    
    if (options && options->save_raw && options->output_path) {
        epaper_save_image_raw(image, options->output_path);
    }
    
    return true;
}

bool epaper_save_image_raw(const epaper_image_t *image, const char *filename) {
    if (!image || !image->data || !filename) {
        return false;
    }
    
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to open output file");
        return false;
    }
    
    uint32_t net_width = htonl(image->width);
    uint32_t net_height = htonl(image->height);
    
    if (fwrite(&net_width, sizeof(net_width), 1, fp) != 1 ||
        fwrite(&net_height, sizeof(net_height), 1, fp) != 1 ||
        fwrite(image->data, 1, image->data_size, fp) != image->data_size) {
        perror("Failed to write to file");
        fclose(fp);
        return false;
    }
    
    fclose(fp);
    printf("Raw image saved to %s\n", filename);
    return true;
}

bool epaper_save_image_pbm(const epaper_image_t *image, const char *filename) {
    if (!image || !image->data || !filename) {
        return false;
    }
    
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to open output file");
        return false;
    }
    
    fprintf(fp, "P4\n");
    fprintf(fp, "# Generated by epaper receive API\n");
    fprintf(fp, "%u %u\n", image->width, image->height);
    
    if (fwrite(image->data, 1, image->data_size, fp) != image->data_size) {
        perror("Failed to write image data");
        fclose(fp);
        return false;
    }
    
    fclose(fp);
    printf("PBM image saved to %s\n", filename);
    return true;
}

void epaper_free_image(epaper_image_t *image) {
    if (image) {
        if (image->data) {
            free(image->data);
            image->data = NULL;
        }
        image->width = 0;
        image->height = 0;
        image->data_size = 0;
    }
}
