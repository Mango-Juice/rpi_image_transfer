#include <send_epaper_data.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options] <image_file>\n", prog_name);
    printf("Options:\n");
    printf("  -d, --device <path>     Device path (default: /dev/epaper_tx)\n");
    printf("  -w, --width <pixels>    Target width\n");
    printf("  -h, --height <pixels>   Target height\n");
    printf("  -t, --threshold <0-255> Threshold value (default: 128)\n");
    printf("  -D, --dither            Use Floyd-Steinberg dithering\n");
    printf("  -i, --invert            Invert colors\n");
    printf("  --help                  Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *device_path = "/dev/epaper_tx";
    const char *image_path = NULL;
    epaper_convert_options_t options = {0, 0, false, false, 128};
    
    static struct option long_options[] = {
        {"device",    required_argument, 0, 'd'},
        {"width",     required_argument, 0, 'w'},
        {"height",    required_argument, 0, 'h'},
        {"threshold", required_argument, 0, 't'},
        {"dither",    no_argument,       0, 'D'},
        {"invert",    no_argument,       0, 'i'},
        {"help",      no_argument,       0, '?'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:t:Di", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            device_path = optarg;
            break;
        case 'w':
            options.target_width = atoi(optarg);
            break;
        case 'h':
            options.target_height = atoi(optarg);
            break;
        case 't':
            options.threshold = atoi(optarg);
            break;
        case 'D':
            options.use_dithering = true;
            break;
        case 'i':
            options.invert_colors = true;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return 1;
    }
    
    image_path = argv[optind];
    
    int fd = epaper_open(device_path);
    if (fd < 0) {
        return 1;
    }
    
    bool success;
    if (options.target_width > 0 || options.target_height > 0 || 
        options.use_dithering || options.invert_colors || options.threshold != 128) {
        success = epaper_send_image_advanced(fd, image_path, &options);
    } else {
        success = epaper_send_image(fd, image_path);
    }
    
    epaper_close(fd);
    
    if (success) {
        printf("Image sent successfully!\n");
        return 0;
    } else {
        fprintf(stderr, "Failed to send image\n");
        return 1;
    }
}
