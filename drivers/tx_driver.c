#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
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

#define EPAPER_TX_MAGIC 'E'
#define EPAPER_TX_GET_STATUS    _IOR(EPAPER_TX_MAGIC, 1, struct tx_status_info)
#define EPAPER_TX_GET_STATS     _IOR(EPAPER_TX_MAGIC, 2, struct tx_statistics)
#define EPAPER_TX_RESET_STATS   _IO(EPAPER_TX_MAGIC, 3)
#define EPAPER_TX_RESET_STATE   _IO(EPAPER_TX_MAGIC, 4)

struct tx_status_info {
    bool transmission_active;
    bool handshake_complete;
    bool error_state;
    u8 last_seq_sent;
    int retry_count;
};

struct tx_statistics {
    u32 total_packets_sent;
    u32 total_bytes_sent;
    u32 total_retries;
    u32 successful_handshakes;
    u32 failed_handshakes;
    u32 timeouts;
    u32 nacks_received;
};

#define CLASS_NAME "epaper_tx"
#define DEVICE_NAME "epaper_tx"
#define BUFFER_SIZE 4096
#define MAX_RETRIES 5
#define TIMEOUT_MS 300
#define MAX_PACKET_DATA 31
#define DATA_PIN_COUNT 3
#define HANDSHAKE_SYN 0x16

/* GPIO pin definitions */
#define DATA_GPIO_0 17
#define DATA_GPIO_1 27
#define DATA_GPIO_2 22
#define CLOCK_GPIO  23
#define ACK_GPIO    24

struct tx_packet {
    u8 seq_num;
    u8 data_len;
    u8 data[MAX_PACKET_DATA];
    u32 crc32;
};

struct tx_state {
    bool transmission_active;
    u8 last_seq_sent;
    int retry_count;
    bool error_state;
    bool handshake_complete;
};

static dev_t dev_num;
static struct cdev tx_cdev;
static struct class *tx_class;
static struct device *tx_device;

static struct gpio_desc *data_gpio[DATA_PIN_COUNT];
static struct gpio_desc *clock_gpio;
static struct gpio_desc *ack_gpio;

static DEFINE_MUTEX(tx_mutex);
static DECLARE_WAIT_QUEUE_HEAD(ack_waitqueue);
static atomic_t ack_received = ATOMIC_INIT(0);
static atomic_t ack_status = ATOMIC_INIT(0);

static u8 sequence_number = 0;
static struct tx_state current_state = {0};
static struct tx_statistics tx_stats = {0};

/* Function declarations */
static void reset_tx_state(void);
static u32 calculate_crc32(struct tx_packet *packet);
static void send_3bit_data(u8 data);
static void send_byte(u8 byte);
static int wait_for_ack(void);
static int send_packet(struct tx_packet *packet);
static irqreturn_t ack_irq_handler(int irq, void *dev_id);
static int perform_handshake(void);
static int tx_open(struct inode *inode, struct file *filp);
static int tx_release(struct inode *inode, struct file *filp);
static ssize_t tx_write(struct file *filp, const char __user *buf, size_t count, loff_t *off);
static long tx_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int init_gpio_pins(void);
static void cleanup_gpio_pins(void);

static void reset_tx_state(void) {
    current_state.transmission_active = false;
    current_state.retry_count = 0;
    current_state.error_state = false;
    current_state.handshake_complete = false;
    atomic_set(&ack_received, 0);
}

static u32 calculate_crc32(struct tx_packet *packet) {
    u32 crc;
    u8 header[2];
    
    header[0] = packet->seq_num;
    header[1] = packet->data_len;
    crc = crc32(0, header, 2);
    
    if (packet->data_len > 0) {
        crc = crc32(crc, packet->data, packet->data_len);
    }
    
    return crc;
}

static void send_3bit_data(u8 data) {
    unsigned long flags;
    
    local_irq_save(flags);
    
    gpiod_set_value(data_gpio[0], (data >> 0) & 1);
    gpiod_set_value(data_gpio[1], (data >> 1) & 1);
    gpiod_set_value(data_gpio[2], (data >> 2) & 1);
    
    wmb();
    udelay(5);
    
    gpiod_set_value(clock_gpio, 1);
    udelay(5);
    gpiod_set_value(clock_gpio, 0);
    udelay(5);
    
    local_irq_restore(flags);
}

static void send_byte(u8 byte) {
    send_3bit_data(byte & 0x07);
    send_3bit_data((byte >> 3) & 0x07);
    send_3bit_data((byte >> 6) & 0x03);
}

static int wait_for_ack(void) {
    int ret;
    unsigned long timeout_jiffies = msecs_to_jiffies(TIMEOUT_MS);
    
    atomic_set(&ack_received, 0);
    
    ret = wait_event_timeout(ack_waitqueue, 
                           atomic_read(&ack_received), 
                           timeout_jiffies);
    if (ret == 0) {
        pr_warn("[epaper_tx] ACK timeout (seq: %d, retry: %d)\n", 
                current_state.last_seq_sent, current_state.retry_count);
        return -ETIMEDOUT;
    }
    
    smp_rmb();
    
    if (atomic_read(&ack_status)) {
        pr_debug("[epaper_tx] ACK received for seq %d\n", current_state.last_seq_sent);
        return 0;
    } else {
        pr_warn("[epaper_tx] NACK received for seq %d\n", current_state.last_seq_sent);
        return -ECOMM;
    }
}

static int send_packet(struct tx_packet *packet) {
    int ret;
    int last_error = -EIO;
    
    packet->crc32 = calculate_crc32(packet);
    current_state.last_seq_sent = packet->seq_num;
    current_state.retry_count = 0;
    current_state.transmission_active = true;
    
    while (current_state.retry_count < MAX_RETRIES) {
        pr_debug("[epaper_tx] Sending packet seq=%d, len=%d, crc=0x%08x, attempt=%d\n", 
                packet->seq_num, packet->data_len, packet->crc32, current_state.retry_count + 1);
        
        send_byte(packet->seq_num);
        send_byte(packet->data_len);
        
        for (int i = 0; i < packet->data_len; i++) {
            send_byte(packet->data[i]);
        }
        
        send_byte((packet->crc32) & 0xFF);
        send_byte((packet->crc32 >> 8) & 0xFF);
        send_byte((packet->crc32 >> 16) & 0xFF);
        send_byte((packet->crc32 >> 24) & 0xFF);
        
        ret = wait_for_ack();
        if (ret == 0) {
            tx_stats.total_packets_sent++;
            pr_info("[epaper_tx] Packet %d sent successfully after %d attempts\n", 
                   packet->seq_num, current_state.retry_count + 1);
            current_state.transmission_active = false;
            return 0;
        }
        
        last_error = ret;
        current_state.retry_count++;
        tx_stats.total_retries++;
        
        if (ret == -ETIMEDOUT) {
            tx_stats.timeouts++;
            pr_warn("[epaper_tx] Timeout on seq %d, retry %d/%d\n", 
                    packet->seq_num, current_state.retry_count, MAX_RETRIES);
        } else if (ret == -ECOMM) {
            tx_stats.nacks_received++;
            pr_warn("[epaper_tx] NACK on seq %d, retry %d/%d\n", 
                    packet->seq_num, current_state.retry_count, MAX_RETRIES);
        }
        
        if (current_state.retry_count < MAX_RETRIES) {
            usleep_range(50000 + current_state.retry_count * 10000, 
                        60000 + current_state.retry_count * 12000);
        }
    }
    
    if (last_error == -ETIMEDOUT) {
        pr_err("[epaper_tx] Packet %d failed: persistent timeout after %d retries\n", 
               packet->seq_num, MAX_RETRIES);
    } else if (last_error == -ECOMM) {
        pr_err("[epaper_tx] Packet %d failed: persistent NACK after %d retries\n", 
               packet->seq_num, MAX_RETRIES);
    } else {
        pr_err("[epaper_tx] Packet %d failed: unknown error %d after %d retries\n", 
               packet->seq_num, last_error, MAX_RETRIES);
    }
    
    current_state.transmission_active = false;
    current_state.error_state = true;
    return last_error;
}

static irqreturn_t ack_irq_handler(int irq, void *dev_id) {
    static unsigned long last_ack_time = 0;
    unsigned long current_time = jiffies;
    bool current_ack;
    
    if (time_before(current_time, last_ack_time + msecs_to_jiffies(2))) {
        return IRQ_HANDLED;
    }
    
    current_ack = gpiod_get_value(ack_gpio);
    
    atomic_set(&ack_status, current_ack ? 1 : 0);
    atomic_set(&ack_received, 1);
    
    smp_wmb();
    
    wake_up(&ack_waitqueue);
    last_ack_time = current_time;
    
    pr_debug("[epaper_tx] %s received\n", current_ack ? "ACK" : "NACK");
    return IRQ_HANDLED;
}

static int perform_handshake(void) {
    int retry, ret;
    int last_error = -ECONNREFUSED;
    
    for (retry = 0; retry < MAX_RETRIES; retry++) {
        pr_info("[epaper_tx] Starting handshake (attempt %d)\n", retry + 1);
        
        send_byte(HANDSHAKE_SYN);
        
        ret = wait_for_ack();
        if (ret == 0) {
            current_state.handshake_complete = true;
            tx_stats.successful_handshakes++;
            pr_info("[epaper_tx] Handshake successful\n");
            return 0;
        }
        
        last_error = ret;
        if (ret == -ETIMEDOUT) {
            pr_warn("[epaper_tx] Handshake timeout (attempt %d)\n", retry + 1);
        } else if (ret == -ECOMM) {
            pr_warn("[epaper_tx] Handshake NACK (attempt %d)\n", retry + 1);
        }
        
        usleep_range(80000, 120000);
    }
    
    tx_stats.failed_handshakes++;
    if (last_error == -ETIMEDOUT) {
        pr_err("[epaper_tx] Handshake failed: receiver not responding after %d attempts\n", MAX_RETRIES);
        return -EHOSTUNREACH;
    } else if (last_error == -ECOMM) {
        pr_err("[epaper_tx] Handshake failed: receiver rejected connection after %d attempts\n", MAX_RETRIES);
        return -ECONNREFUSED;
    } else {
        pr_err("[epaper_tx] Handshake failed: unknown error %d after %d attempts\n", last_error, MAX_RETRIES);
        return last_error;
    }
}

static int tx_open(struct inode *inode, struct file *filp) {
    if (!mutex_trylock(&tx_mutex)) {
        return -EBUSY;
    }
    
    reset_tx_state();
    pr_info("[epaper_tx] Device opened\n");
    return 0;
}

static int tx_release(struct inode *inode, struct file *filp) {
    mutex_unlock(&tx_mutex);
    return 0;
}

static ssize_t tx_write(struct file *filp, const char __user *buf, size_t count, loff_t *off) {
    struct tx_packet packet;
    u8 *user_data;
    size_t bytes_sent = 0;
    int ret;
    
    if (count == 0) return 0;
    if (count > BUFFER_SIZE) count = BUFFER_SIZE;
    
    user_data = kmalloc(count, GFP_KERNEL);
    if (!user_data) {
        return -ENOMEM;
    }
    
    if (current_state.error_state) {
        reset_tx_state();
    }
    
    if (copy_from_user(user_data, buf, count)) {
        kfree(user_data);
        return -EFAULT;
    }
    
    if (!current_state.handshake_complete) {
        pr_info("[epaper_tx] Performing handshake before data transfer\n");
        ret = perform_handshake();
        if (ret < 0) {
            kfree(user_data);
            return ret;
        }
    }
    
    pr_info("[epaper_tx] Starting transmission of %zu bytes\n", count);
    
    while (bytes_sent < count) {
        size_t chunk_size = min_t(size_t, count - bytes_sent, MAX_PACKET_DATA);
        
        packet.seq_num = sequence_number++;
        packet.data_len = chunk_size;
        memcpy(packet.data, user_data + bytes_sent, chunk_size);
        
        ret = send_packet(&packet);
        if (ret < 0) {
            pr_err("[epaper_tx] Failed to send packet at offset %zu\n", bytes_sent);
            reset_tx_state();
            kfree(user_data);
            return bytes_sent > 0 ? bytes_sent : ret;
        }
        
        bytes_sent += chunk_size;
    }
    
    tx_stats.total_bytes_sent += count;
    kfree(user_data);
    return count;
}

static long tx_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct tx_status_info status;
    
    switch (cmd) {
    case EPAPER_TX_GET_STATUS:
        status.transmission_active = current_state.transmission_active;
        status.handshake_complete = current_state.handshake_complete;
        status.error_state = current_state.error_state;
        status.last_seq_sent = current_state.last_seq_sent;
        status.retry_count = current_state.retry_count;
        
        if (copy_to_user((void __user *)arg, &status, sizeof(status))) {
            return -EFAULT;
        }
        break;
        
    case EPAPER_TX_GET_STATS:
        if (copy_to_user((void __user *)arg, &tx_stats, sizeof(tx_stats))) {
            return -EFAULT;
        }
        break;
        
    case EPAPER_TX_RESET_STATS:
        memset(&tx_stats, 0, sizeof(tx_stats));
        pr_info("[epaper_tx] Statistics reset\n");
        break;
        
    case EPAPER_TX_RESET_STATE:
        reset_tx_state();
        pr_info("[epaper_tx] State reset via ioctl\n");
        break;
        
    default:
        return -ENOTTY;
    }
    
    return 0;
}

static const struct file_operations tx_fops = {
    .owner = THIS_MODULE,
    .open = tx_open,
    .release = tx_release,
    .write = tx_write,
    .unlocked_ioctl = tx_ioctl,
};

static int init_gpio_pins(void) {
    int ret, i;
    int data_pins[DATA_PIN_COUNT] = {DATA_GPIO_0, DATA_GPIO_1, DATA_GPIO_2};
    
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        ret = gpio_request(data_pins[i], "epaper_data");
        if (ret) {
            pr_err("[epaper_tx] Failed to request data GPIO %d: %d\n", data_pins[i], ret);
            goto cleanup_data_gpios;
        }
        
        ret = gpio_direction_output(data_pins[i], 0);
        if (ret) {
            pr_err("[epaper_tx] Failed to set data GPIO %d as output: %d\n", data_pins[i], ret);
            gpio_free(data_pins[i]);
            goto cleanup_data_gpios;
        }
        
        data_gpio[i] = gpio_to_desc(data_pins[i]);
        if (!data_gpio[i]) {
            pr_err("[epaper_tx] Failed to get descriptor for data GPIO %d\n", data_pins[i]);
            gpio_free(data_pins[i]);
            ret = -ENODEV;
            goto cleanup_data_gpios;
        }
    }
    
    ret = gpio_request(CLOCK_GPIO, "epaper_clock");
    if (ret) {
        pr_err("[epaper_tx] Failed to request clock GPIO: %d\n", ret);
        goto cleanup_data_gpios;
    }
    
    ret = gpio_direction_output(CLOCK_GPIO, 0);
    if (ret) {
        pr_err("[epaper_tx] Failed to set clock GPIO as output: %d\n", ret);
        gpio_free(CLOCK_GPIO);
        goto cleanup_data_gpios;
    }
    
    clock_gpio = gpio_to_desc(CLOCK_GPIO);
    if (!clock_gpio) {
        pr_err("[epaper_tx] Failed to get descriptor for clock GPIO\n");
        gpio_free(CLOCK_GPIO);
        ret = -ENODEV;
        goto cleanup_data_gpios;
    }
    
    ret = gpio_request(ACK_GPIO, "epaper_ack");
    if (ret) {
        pr_err("[epaper_tx] Failed to request ACK GPIO: %d\n", ret);
        goto cleanup_clock_gpio;
    }
    
    ret = gpio_direction_input(ACK_GPIO);
    if (ret) {
        pr_err("[epaper_tx] Failed to set ACK GPIO as input: %d\n", ret);
        gpio_free(ACK_GPIO);
        goto cleanup_clock_gpio;
    }
    
    ack_gpio = gpio_to_desc(ACK_GPIO);
    if (!ack_gpio) {
        pr_err("[epaper_tx] Failed to get descriptor for ACK GPIO\n");
        gpio_free(ACK_GPIO);
        ret = -ENODEV;
        goto cleanup_clock_gpio;
    }
    
    pr_info("[epaper_tx] GPIO initialization successful\n");
    return 0;
    
cleanup_clock_gpio:
    if (clock_gpio) {
        gpio_free(CLOCK_GPIO);
        clock_gpio = NULL;
    }
cleanup_data_gpios:
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        if (data_gpio[i]) {
            int pin_num = desc_to_gpio(data_gpio[i]);
            gpio_free(pin_num);
            data_gpio[i] = NULL;
        }
    }
    return ret;
}

static void cleanup_gpio_pins(void) {
    int i;
    
    if (ack_gpio) {
        gpio_free(ACK_GPIO);
        ack_gpio = NULL;
    }
    
    if (clock_gpio) {
        gpio_free(CLOCK_GPIO);
        clock_gpio = NULL;
    }
    
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        if (data_gpio[i]) {
            int pin_num = desc_to_gpio(data_gpio[i]);
            gpio_free(pin_num);
            data_gpio[i] = NULL;
        }
    }
    
    pr_info("[epaper_tx] GPIO cleanup complete\n");
}

static int __init tx_driver_init(void) {
    int ret, irq;
    
    pr_info("[epaper_tx] Initializing TX driver\n");
    
    ret = init_gpio_pins();
    if (ret) {
        pr_err("[epaper_tx] GPIO initialization failed: %d\n", ret);
        return ret;
    }
    
    irq = gpiod_to_irq(ack_gpio);
    if (irq < 0) {
        pr_err("[epaper_tx] Failed to get ACK IRQ\n");
        ret = irq;
        goto cleanup_gpio;
    }
    
    ret = request_irq(irq, ack_irq_handler, 
                      IRQF_TRIGGER_RISING,
                      "epaper_ack", NULL);
    if (ret) {
        pr_err("[epaper_tx] Failed to request ACK IRQ\n");
        goto cleanup_gpio;
    }
    
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[epaper_tx] Failed to allocate device number\n");
        goto cleanup_irq;
    }
    
    cdev_init(&tx_cdev, &tx_fops);
    tx_cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&tx_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("[epaper_tx] Failed to add cdev\n");
        goto cleanup_chrdev;
    }
    
    tx_class = class_create(CLASS_NAME);
    if (IS_ERR(tx_class)) {
        pr_err("[epaper_tx] Failed to create class\n");
        ret = PTR_ERR(tx_class);
        goto cleanup_cdev;
    }
    
    tx_device = device_create(tx_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(tx_device)) {
        pr_err("[epaper_tx] Failed to create device\n");
        ret = PTR_ERR(tx_device);
        goto cleanup_class;
    }
    
    pr_info("[epaper_tx] Driver initialized successfully - /dev/%s created\n", DEVICE_NAME);
    return 0;
    
cleanup_class:
    class_destroy(tx_class);
cleanup_cdev:
    cdev_del(&tx_cdev);
cleanup_chrdev:
    unregister_chrdev_region(dev_num, 1);
cleanup_irq:
    free_irq(irq, NULL);
cleanup_gpio:
    cleanup_gpio_pins();
    return ret;
}

static void __exit tx_driver_exit(void) {
    int irq;
    
    device_destroy(tx_class, dev_num);
    class_destroy(tx_class);
    cdev_del(&tx_cdev);
    unregister_chrdev_region(dev_num, 1);
    
    irq = gpiod_to_irq(ack_gpio);
    if (irq > 0) {
        free_irq(irq, NULL);
    }
    
    cleanup_gpio_pins();
    
    pr_info("[epaper_tx] TX driver unloaded\n");
}

module_init(tx_driver_init);
module_exit(tx_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Programming Assignment");
MODULE_DESCRIPTION("E-paper TX driver for reliable image data transmission");
