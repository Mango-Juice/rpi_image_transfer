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

#define CLASS_NAME "epaper_tx"
#define DEVICE_NAME "epaper_tx"
#define MAX_IMAGE_SIZE (1920 * 1080)
#define TIMEOUT_MS 2000
#define MAX_RETRIES 3

struct image_header {
    u16 width;
    u16 height;
    u32 data_length;
    u16 header_checksum;
} __packed;

static const struct of_device_id epaper_tx_of_match[] = {
    { .compatible = "epaper,gpio-tx" },
    { }
};
MODULE_DEVICE_TABLE(of, epaper_tx_of_match);

static struct gpio_desc *clock_gpio;
static struct gpio_desc *data_gpio;
static struct gpio_desc *start_stop_gpio;
static struct gpio_desc *ack_gpio;
static struct gpio_desc *nack_gpio;

static dev_t dev_num;
static struct cdev tx_cdev;
static struct class *tx_class;
static struct device *tx_device;
static DEFINE_MUTEX(tx_mutex);
static DECLARE_WAIT_QUEUE_HEAD(response_waitqueue);

static volatile bool ack_received, nack_received;
static int ack_irq, nack_irq;

static irqreturn_t ack_irq_handler(int irq, void *dev_id) {
    ack_received = true;
    wake_up_interruptible(&response_waitqueue);
    return IRQ_HANDLED;
}

static irqreturn_t nack_irq_handler(int irq, void *dev_id) {
    nack_received = true;
    wake_up_interruptible(&response_waitqueue);
    return IRQ_HANDLED;
}

static void send_bit(int bit) {
    gpiod_set_value(data_gpio, bit ? 1 : 0);
    udelay(10);
    gpiod_set_value(clock_gpio, 1);
    udelay(20);
    gpiod_set_value(clock_gpio, 0);
    udelay(10);
}

static void send_byte(u8 byte) {
    for (int i = 0; i < 8; i++) {
        send_bit((byte >> i) & 1);
    }
}

static void send_start_signal(void) {
    gpiod_set_value(start_stop_gpio, 1);
    mdelay(5);
}

static void send_stop_signal(void) {
    gpiod_set_value(start_stop_gpio, 0);
    mdelay(5);
}

static int wait_for_response(void) {
    int ret = wait_event_timeout(response_waitqueue, 
                                ack_received || nack_received,
                                msecs_to_jiffies(TIMEOUT_MS));
    
    if (ret == 0) return -ETIMEDOUT;
    
    if (nack_received) {
        nack_received = false;
        return -ECOMM;
    }
    
    if (ack_received) {
        ack_received = false;
        return 0;
    }
    
    return -EIO;
}

static u16 calculate_header_checksum(struct image_header *h) {
    return (u16)(h->width + h->height + (h->data_length & 0xFFFF) + (h->data_length >> 16));
}

static int send_data_block(u8 *data, size_t length) {
    send_start_signal();
    
    for (size_t i = 0; i < length; i++) {
        send_byte(data[i]);
    }
    
    send_stop_signal();
    return wait_for_response();
}

static ssize_t tx_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *pos) {
    int ret;
    u8 *buffer;
    struct image_header header;
    u32 crc32_val;
    
    if (count < sizeof(header)) return -EINVAL;
    if (count > MAX_IMAGE_SIZE + sizeof(header)) return -EINVAL;
    
    if (!mutex_trylock(&tx_mutex)) return -EBUSY;
    
    buffer = kmalloc(count, GFP_KERNEL);
    if (!buffer) {
        mutex_unlock(&tx_mutex);
        return -ENOMEM;
    }
    
    if (copy_from_user(buffer, user_buffer, count)) {
        kfree(buffer);
        mutex_unlock(&tx_mutex);
        return -EFAULT;
    }
    
    memcpy(&header, buffer, sizeof(header));
    
    if (header.data_length != count - sizeof(header) || 
        header.data_length > MAX_IMAGE_SIZE) {
        kfree(buffer);
        mutex_unlock(&tx_mutex);
        return -EINVAL;
    }
    
    header.header_checksum = calculate_header_checksum(&header);
    memcpy(buffer, &header, sizeof(header));
    
    crc32_val = crc32(0, buffer + sizeof(header), header.data_length);
    
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        ack_received = nack_received = false;
        
        ret = send_data_block((u8*)&header, sizeof(header));
        if (ret) {
            if (ret == -ETIMEDOUT || ret == -ECOMM) continue;
            break;
        }
        
        ret = send_data_block(buffer + sizeof(header), header.data_length);
        if (ret) {
            if (ret == -ETIMEDOUT || ret == -ECOMM) continue;
            break;
        }
        
        ret = send_data_block((u8*)&crc32_val, sizeof(crc32_val));
        if (ret == 0) break;
    }
    
    kfree(buffer);
    mutex_unlock(&tx_mutex);
    
    return ret ? ret : count;
}

static int tx_open(struct inode *inode, struct file *file) {
    return 0;
}

static int tx_release(struct inode *inode, struct file *file) {
    return 0;
}

static const struct file_operations tx_fops = {
    .owner = THIS_MODULE,
    .open = tx_open,
    .release = tx_release,
    .write = tx_write,
};

static int epaper_tx_probe(struct platform_device *pdev) {
    int ret;
    
    clock_gpio = devm_gpiod_get(&pdev->dev, "clock", GPIOD_OUT_LOW);
    if (IS_ERR(clock_gpio)) return PTR_ERR(clock_gpio);
    
    data_gpio = devm_gpiod_get(&pdev->dev, "data", GPIOD_OUT_LOW);
    if (IS_ERR(data_gpio)) return PTR_ERR(data_gpio);
    
    start_stop_gpio = devm_gpiod_get(&pdev->dev, "start-stop", GPIOD_OUT_LOW);
    if (IS_ERR(start_stop_gpio)) return PTR_ERR(start_stop_gpio);
    
    ack_gpio = devm_gpiod_get(&pdev->dev, "ack", GPIOD_IN);
    if (IS_ERR(ack_gpio)) return PTR_ERR(ack_gpio);
    
    nack_gpio = devm_gpiod_get(&pdev->dev, "nack", GPIOD_IN);
    if (IS_ERR(nack_gpio)) return PTR_ERR(nack_gpio);
    
    ack_irq = gpiod_to_irq(ack_gpio);
    if (ack_irq < 0) return ack_irq;
    
    nack_irq = gpiod_to_irq(nack_gpio);
    if (nack_irq < 0) return nack_irq;
    
    ret = request_irq(ack_irq, ack_irq_handler, IRQF_TRIGGER_RISING, 
                      "epaper_tx_ack", NULL);
    if (ret) return ret;
    
    ret = request_irq(nack_irq, nack_irq_handler, IRQF_TRIGGER_RISING, 
                      "epaper_tx_nack", NULL);
    if (ret) {
        free_irq(ack_irq, NULL);
        return ret;
    }
    
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret) goto err_irq;
    
    cdev_init(&tx_cdev, &tx_fops);
    ret = cdev_add(&tx_cdev, dev_num, 1);
    if (ret) goto err_chrdev;
    
    tx_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(tx_class)) {
        ret = PTR_ERR(tx_class);
        goto err_cdev;
    }
    
    tx_device = device_create(tx_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(tx_device)) {
        ret = PTR_ERR(tx_device);
        goto err_class;
    }
    
    return 0;
    
err_class:
    class_destroy(tx_class);
err_cdev:
    cdev_del(&tx_cdev);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
err_irq:
    free_irq(nack_irq, NULL);
    free_irq(ack_irq, NULL);
    return ret;
}

static int epaper_tx_remove(struct platform_device *pdev) {
    device_destroy(tx_class, dev_num);
    class_destroy(tx_class);
    cdev_del(&tx_cdev);
    unregister_chrdev_region(dev_num, 1);
    free_irq(nack_irq, NULL);
    free_irq(ack_irq, NULL);
    return 0;
}

static struct platform_driver epaper_tx_driver = {
    .probe = epaper_tx_probe,
    .remove = epaper_tx_remove,
    .driver = {
        .name = "epaper-gpio-tx",
        .of_match_table = epaper_tx_of_match,
    },
};

module_platform_driver(epaper_tx_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("E-paper Team");
MODULE_DESCRIPTION("E-paper GPIO TX Driver - 5-pin Serial Protocol");
MODULE_VERSION("2.0");
