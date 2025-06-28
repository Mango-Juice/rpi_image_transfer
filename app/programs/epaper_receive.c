#include <receive_epaper_data.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -d, --device <path>     RX device path (default: /dev/epaper_rx)\n");
    printf("  -o, --output <file>     Output file path\n");
    printf("  -f, --format <format>   Output format: raw, pbm (default: pbm)\n");
    printf("  -t, --timeout <ms>      Receive timeout in milliseconds (default: 30000)\n");
    printf("  -v, --verbose           Verbose output\n");
    printf("  -h, --help             Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -o received.pbm\n", prog_name);
    printf("  %s -d /dev/epaper_rx -o image.raw -f raw -v\n", prog_name);
}

int main(int argc, char *argv[]) {
    const char *device_path = "/dev/epaper_rx";
    const char *output_path = "received_image.pbm";
    const char *format = "pbm";
    int timeout_ms = 30000;
    bool verbose = false;
    
    static struct option long_options[] = {
        {"device",  required_argument, 0, 'd'},
        {"output",  required_argument, 0, 'o'},
        {"format",  required_argument, 0, 'f'},
        {"timeout", required_argument, 0, 't'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "d:o:f:t:vh", long_options, NULL)) != -1) {
        switch (c) {
        case 'd':
            device_path = optarg;
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'f':
            format = optarg;
            if (strcmp(format, "raw") != 0 && strcmp(format, "pbm") != 0) {
                fprintf(stderr, "Error: Invalid format '%s'. Use 'raw' or 'pbm'\n", format);
                return 1;
            }
            break;
        case 't':
            timeout_ms = atoi(optarg);
            if (timeout_ms <= 0) {
                fprintf(stderr, "Error: Invalid timeout value\n");
                return 1;
            }
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        case '?':
            return 1;
        }
    }
    
    if (verbose) {
        printf("Opening RX device: %s\n", device_path);
    }
    
    int fd = epaper_rx_open(device_path);
    if (fd < 0) {
        fprintf(stderr, "Failed to open RX device\n");
        return 1;
    }
    
    epaper_image_t image;
    epaper_receive_options_t options = {
        .save_raw = false,
        .output_path = NULL,
        .verbose = verbose,
        .timeout_ms = timeout_ms
    };
    
    printf("Waiting for image data...\n");
    
    if (!epaper_receive_image_advanced(fd, &image, &options)) {
        fprintf(stderr, "Failed to receive image\n");
        epaper_rx_close(fd);
        return 1;
    }
    
    epaper_rx_close(fd);
    
    bool save_success = false;
    if (strcmp(format, "raw") == 0) {
        save_success = epaper_save_image_raw(&image, output_path);
    } else {
        save_success = epaper_save_image_pbm(&image, output_path);
    }
    
    if (save_success) {
        printf("Image saved successfully: %ux%u pixels, %zu bytes\n",
               image.width, image.height, image.data_size);
    } else {
        fprintf(stderr, "Failed to save image\n");
    }
    
    epaper_free_image(&image);
    
    return save_success ? 0 : 1;
}
