#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/crc32.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/timer.h>

#define CLASS_NAME "epaper_rx"
#define DEVICE_NAME "epaper_rx"
#define MAX_IMAGE_SIZE (1920 * 1080)
#define TIMEOUT_MS 2000

struct image_header {
    u16 width;
    u16 height;
    u32 data_length;
    u16 header_checksum;
} __packed;

static const struct of_device_id epaper_rx_of_match[] = {
    { .compatible = "epaper,gpio-rx" },
    { }
};
MODULE_DEVICE_TABLE(of, epaper_rx_of_match);

static struct gpio_desc *clock_gpio;
static struct gpio_desc *data_gpio;
static struct gpio_desc *start_stop_gpio;
static struct gpio_desc *ack_gpio;
static struct gpio_desc *nack_gpio;

static dev_t dev_num;
static struct cdev rx_cdev;
static struct class *rx_class;
static struct device *rx_device;
static DEFINE_MUTEX(rx_mutex);
static DECLARE_WAIT_QUEUE_HEAD(data_waitqueue);

static struct image_header header;
static u8 *image_buffer;
static bool image_ready;
static struct timer_list timeout_timer;

static int clock_irq, start_stop_irq;
static volatile int bit_count;
static volatile u8 current_byte;
static volatile bool receiving_data;
static volatile u32 byte_count;
static volatile u8 *data_ptr;
static volatile u32 expected_crc, received_crc;

static void send_ack(void) {
    gpiod_set_value(ack_gpio, 1);
    mdelay(10);
    gpiod_set_value(ack_gpio, 0);
}

static void send_nack(void) {
    gpiod_set_value(nack_gpio, 1);
    mdelay(10);
    gpiod_set_value(nack_gpio, 0);
}

static void timeout_handler(struct timer_list *t) {
    receiving_data = false;
    bit_count = 0;
    byte_count = 0;
    current_byte = 0;
    send_nack();
}

static void reset_rx_state(void) {
    del_timer_sync(&timeout_timer);
    receiving_data = false;
    bit_count = 0;
    byte_count = 0;
    current_byte = 0;
    data_ptr = NULL;
}

static u16 calculate_header_checksum(struct image_header *h) {
    return (u16)(h->width + h->height + (h->data_length & 0xFFFF) + (h->data_length >> 16));
}

static irqreturn_t clock_irq_handler(int irq, void *dev_id) {
    if (!receiving_data) return IRQ_HANDLED;
    
    int bit = gpiod_get_value(data_gpio);
    current_byte |= (bit << bit_count);
    bit_count++;
    
    if (bit_count == 8) {
        if (data_ptr) {
            *data_ptr = current_byte;
            data_ptr++;
        }
        byte_count++;
        bit_count = 0;
        current_byte = 0;
        
        mod_timer(&timeout_timer, jiffies + msecs_to_jiffies(TIMEOUT_MS));
    }
    
    return IRQ_HANDLED;
}

static irqreturn_t start_stop_irq_handler(int irq, void *dev_id) {
    if (gpiod_get_value(start_stop_gpio)) {
        reset_rx_state();
        receiving_data = true;
        byte_count = 0;
        data_ptr = (u8*)&header;
        timer_setup(&timeout_timer, timeout_handler, 0);
        mod_timer(&timeout_timer, jiffies + msecs_to_jiffies(TIMEOUT_MS));
    } else {
        del_timer_sync(&timeout_timer);
        receiving_data = false;
        
        if (byte_count < sizeof(header)) {
            send_nack();
            return IRQ_HANDLED;
        }
        
        if (byte_count == sizeof(header)) {
            u16 calc_checksum = calculate_header_checksum(&header);
            if (header.header_checksum != calc_checksum) {
                send_nack();
                return IRQ_HANDLED;
            }
            
            if (header.data_length > MAX_IMAGE_SIZE) {
                send_nack();
                return IRQ_HANDLED;
            }
            
            if (image_buffer) {
                kfree(image_buffer);
            }
            image_buffer = kmalloc(header.data_length + sizeof(u32), GFP_ATOMIC);
            if (!image_buffer) {
                send_nack();
                return IRQ_HANDLED;
            }
            
            send_ack();
            
            receiving_data = true;
            byte_count = 0;
            data_ptr = image_buffer;
            mod_timer(&timeout_timer, jiffies + msecs_to_jiffies(TIMEOUT_MS));
        } else if (byte_count == header.data_length + sizeof(u32)) {
            received_crc = *(u32*)(image_buffer + header.data_length);
            expected_crc = crc32(0, image_buffer, header.data_length);
            
            if (received_crc == expected_crc) {
                send_ack();
                image_ready = true;
                wake_up_interruptible(&data_waitqueue);
            } else {
                send_nack();
            }
        } else {
            send_nack();
        }
    }
    
    return IRQ_HANDLED;
}

static int rx_open(struct inode *inode, struct file *file) {
    if (!mutex_trylock(&rx_mutex)) {
        return -EBUSY;
    }
    return 0;
}

static int rx_release(struct inode *inode, struct file *file) {
    mutex_unlock(&rx_mutex);
    return 0;
}

static ssize_t rx_read(struct file *file, char __user *user_buffer, size_t count, loff_t *pos) {
    ssize_t bytes_read = 0;
    
    if (*pos == 0 && !image_ready) {
        if (file->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        if (wait_event_interruptible(data_waitqueue, image_ready)) {
            return -ERESTARTSYS;
        }
    }
    
    if (!image_ready || !image_buffer) {
        return 0;
    }
    
    size_t total_size = sizeof(header) + header.data_length;
    if (*pos >= total_size) {
        return 0;
    }
    
    size_t available = total_size - *pos;
    size_t to_copy = min(count, available);
    
    if (*pos < sizeof(header)) {
        size_t header_bytes = min(to_copy, sizeof(header) - *pos);
        if (copy_to_user(user_buffer, ((u8*)&header) + *pos, header_bytes)) {
            return -EFAULT;
        }
        bytes_read += header_bytes;
        *pos += header_bytes;
        to_copy -= header_bytes;
    }
    
    if (to_copy > 0 && *pos >= sizeof(header)) {
        size_t data_offset = *pos - sizeof(header);
        if (copy_to_user(user_buffer + bytes_read, image_buffer + data_offset, to_copy)) {
            return -EFAULT;
        }
        bytes_read += to_copy;
        *pos += to_copy;
    }
    
    return bytes_read;
}

static long rx_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case 0x1001: // Reset
        reset_rx_state();
        if (image_buffer) {
            kfree(image_buffer);
            image_buffer = NULL;
        }
        image_ready = false;
        return 0;
    case 0x1002: // Get status
        return image_ready ? 1 : 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations rx_fops = {
    .owner = THIS_MODULE,
    .open = rx_open,
    .release = rx_release,
    .read = rx_read,
    .unlocked_ioctl = rx_ioctl,
};

static int epaper_rx_probe(struct platform_device *pdev) {
    int ret;
    
    clock_gpio = devm_gpiod_get(&pdev->dev, "clock", GPIOD_IN);
    if (IS_ERR(clock_gpio)) {
        dev_err(&pdev->dev, "Failed to get clock GPIO: %ld\n", PTR_ERR(clock_gpio));
        return PTR_ERR(clock_gpio);
    }
    
    data_gpio = devm_gpiod_get(&pdev->dev, "data", GPIOD_IN);
    if (IS_ERR(data_gpio)) {
        dev_err(&pdev->dev, "Failed to get data GPIO: %ld\n", PTR_ERR(data_gpio));
        return PTR_ERR(data_gpio);
    }
    
    start_stop_gpio = devm_gpiod_get(&pdev->dev, "start-stop", GPIOD_IN);
    if (IS_ERR(start_stop_gpio)) {
        dev_err(&pdev->dev, "Failed to get start-stop GPIO: %ld\n", PTR_ERR(start_stop_gpio));
        return PTR_ERR(start_stop_gpio);
    }
    
    ack_gpio = devm_gpiod_get(&pdev->dev, "ack", GPIOD_OUT_LOW);
    if (IS_ERR(ack_gpio)) {
        dev_err(&pdev->dev, "Failed to get ack GPIO: %ld\n", PTR_ERR(ack_gpio));
        return PTR_ERR(ack_gpio);
    }
    
    nack_gpio = devm_gpiod_get(&pdev->dev, "nack", GPIOD_OUT_LOW);
    if (IS_ERR(nack_gpio)) {
        dev_err(&pdev->dev, "Failed to get nack GPIO: %ld\n", PTR_ERR(nack_gpio));
        return PTR_ERR(nack_gpio);
    }
    
    clock_irq = gpiod_to_irq(clock_gpio);
    if (clock_irq < 0) return clock_irq;
    
    start_stop_irq = gpiod_to_irq(start_stop_gpio);
    if (start_stop_irq < 0) return start_stop_irq;
    
    ret = request_irq(clock_irq, clock_irq_handler, IRQF_TRIGGER_RISING, 
                      "epaper_rx_clock", NULL);
    if (ret) return ret;
    
    ret = request_irq(start_stop_irq, start_stop_irq_handler, 
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, 
                      "epaper_rx_start_stop", NULL);
    if (ret) {
        free_irq(clock_irq, NULL);
        return ret;
    }
    
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret) goto err_irq;
    
    cdev_init(&rx_cdev, &rx_fops);
    ret = cdev_add(&rx_cdev, dev_num, 1);
    if (ret) goto err_chrdev;
    
    rx_class = class_create(CLASS_NAME);
    if (IS_ERR(rx_class)) {
        ret = PTR_ERR(rx_class);
        goto err_cdev;
    }
    
    rx_device = device_create(rx_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(rx_device)) {
        ret = PTR_ERR(rx_device);
        goto err_class;
    }
    
    pr_info("E-paper RX driver loaded successfully\n");
    return 0;
    
err_class:
    class_destroy(rx_class);
err_cdev:
    cdev_del(&rx_cdev);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
err_irq:
    free_irq(start_stop_irq, NULL);
    free_irq(clock_irq, NULL);
    return ret;
}

static void epaper_rx_remove(struct platform_device *pdev) {
    reset_rx_state();
    if (image_buffer) {
        kfree(image_buffer);
    }
    device_destroy(rx_class, dev_num);
    class_destroy(rx_class);
    cdev_del(&rx_cdev);
    unregister_chrdev_region(dev_num, 1);
    free_irq(start_stop_irq, NULL);
    free_irq(clock_irq, NULL);
    pr_info("E-paper RX driver unloaded\n");
}

static struct platform_driver epaper_rx_driver = {
    .probe = epaper_rx_probe,
    .remove = epaper_rx_remove,
    .driver = {
        .name = "epaper-gpio-rx",
        .of_match_table = epaper_rx_of_match,
    },
};

module_platform_driver(epaper_rx_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jeon Mingyu");
MODULE_DESCRIPTION("E-paper GPIO RX Driver - 5-pin Serial Protocol");
MODULE_VERSION("2.0");
