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
#include <linux/kfifo.h>
#include <linux/crc32.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/timer.h>
#include <linux/poll.h>

#define CLASS_NAME "epaper_rx"
#define DEVICE_NAME "epaper_rx"
#define BUFFER_SIZE 4096
#define FIFO_SIZE 1024
#define MAX_PACKET_DATA 31
#define DATA_PIN_COUNT 3
#define STATE_TIMEOUT_MS 500
#define HANDSHAKE_SYN 0x16

enum rx_state {
    RX_IDLE,
    RX_SEQ_NUM,
    RX_DATA_LEN,
    RX_DATA,
    RX_CRC32,
    RX_STATE_MAX
};

struct rx_packet {
    u8 seq_num;
    u8 data_len;
    u8 data[MAX_PACKET_DATA];
    u32 crc32;
};

struct rx_state_info {
    enum rx_state current_state;
    u8 expected_seq;
    bool error_detected;
    int bytes_received_in_packet;
    int bit_position;
    u8 current_byte;
    unsigned long last_clock_time;
    int crc_byte_count;
    struct timer_list state_timer;
};

static dev_t dev_num;
static struct cdev rx_cdev;
static struct class *rx_class;
static struct device *rx_device;
static struct platform_device *pdev_global;

static struct gpio_desc *data_gpio[DATA_PIN_COUNT];
static struct gpio_desc *clock_gpio;
static struct gpio_desc *ack_gpio;

static DEFINE_MUTEX(rx_mutex);
static DECLARE_WAIT_QUEUE_HEAD(data_waitqueue);
static DEFINE_SPINLOCK(rx_state_lock);

static struct rx_state_info rx_state = {
    .current_state = RX_IDLE,
    .expected_seq = 0,
    .error_detected = false,
    .bytes_received_in_packet = 0,
    .bit_position = 0,
    .current_byte = 0,
    .last_clock_time = 0,
    .crc_byte_count = 0
};
static struct rx_packet current_packet;
static u8 data_index = 0;

static DECLARE_KFIFO(rx_fifo, u8, FIFO_SIZE);

static struct timer_list state_timeout_timer;

/* Function declarations */
static void reset_rx_state(void);
static void force_state_reset(const char *reason);
static void state_timeout_callback(struct timer_list *t);
static void update_state_timer(void);
static bool verify_crc32(struct rx_packet *packet);
static void send_ack(bool success);
static void send_handshake_ack(void);
static u8 read_3bit_data(void);
static void process_3bit_data(u8 data);
static irqreturn_t clock_irq_handler(int irq, void *dev_id);
static int rx_open(struct inode *inode, struct file *filp);
static int rx_release(struct inode *inode, struct file *filp);
static ssize_t rx_read(struct file *filp, char __user *buf, size_t count, loff_t *off);
static __poll_t rx_poll(struct file *filp, poll_table *wait);
static int init_gpio(struct platform_device *pdev);
static int rx_probe(struct platform_device *pdev);
static void rx_remove(struct platform_device *pdev);

static void reset_rx_state(void) {
    rx_state.current_state = RX_IDLE;
    rx_state.error_detected = false;
    rx_state.bytes_received_in_packet = 0;
    rx_state.bit_position = 0;
    rx_state.current_byte = 0;
    rx_state.crc_byte_count = 0;
    data_index = 0;
    
    del_timer(&state_timeout_timer);
    
    pr_debug("[epaper_rx] RX state reset\n");
}

static void force_state_reset(const char *reason) {
    pr_warn("[epaper_rx] Force reset: %s\n", reason);
    rx_state.error_detected = true;
    reset_rx_state();
}

static void state_timeout_callback(struct timer_list *t) {
    unsigned long flags;
    
    pr_warn("[epaper_rx] State machine timeout - resetting to IDLE\n");
    
    spin_lock_irqsave(&rx_state_lock, flags);
    reset_rx_state();
    spin_unlock_irqrestore(&rx_state_lock, flags);
}

static void update_state_timer(void) {
    mod_timer(&state_timeout_timer, jiffies + msecs_to_jiffies(STATE_TIMEOUT_MS));
}

static bool verify_crc32(struct rx_packet *packet) {
    u32 calculated_crc;
    u8 header[2];
    
    header[0] = packet->seq_num;
    header[1] = packet->data_len;
    calculated_crc = crc32(0, header, 2);
    
    if (packet->data_len > 0) {
        calculated_crc = crc32(calculated_crc, packet->data, packet->data_len);
    }
    
    pr_debug("[epaper_rx] CRC32 verification: calculated=0x%08x, received=0x%08x\n", 
             calculated_crc, packet->crc32);
    
    return calculated_crc == packet->crc32;
}

static void send_ack(bool success) {
    pr_debug("[epaper_rx] Sending %s for seq %d\n", 
             success ? "ACK" : "NACK", current_packet.seq_num);
    
    gpiod_set_value(ack_gpio, success ? 1 : 0);
    usleep_range(4000, 6000);
    gpiod_set_value(ack_gpio, 0);
    usleep_range(800, 1200);
}

static void send_handshake_ack(void) {
    pr_info("[epaper_rx] Sending SYN-ACK\n");
    gpiod_set_value(ack_gpio, 1);
    usleep_range(4000, 6000);
    gpiod_set_value(ack_gpio, 0);
    usleep_range(800, 1200);
}

static u8 read_3bit_data(void) {
    u8 data = 0;
    for (int i = 0; i < DATA_PIN_COUNT; i++) {
        if (gpiod_get_value(data_gpio[i])) {
            data |= (1 << i);
        }
    }
    return data;
}

static void process_3bit_data(u8 data) {
    static int consecutive_invalid_count = 0;
    const int MAX_CONSECUTIVE_INVALID = 10;
    
    if (data > 7) {
        consecutive_invalid_count++;
        if (consecutive_invalid_count >= MAX_CONSECUTIVE_INVALID) {
            force_state_reset("Too many consecutive invalid 3-bit data");
            consecutive_invalid_count = 0;
        }
        return;
    }
    
    consecutive_invalid_count = 0;
    
    rx_state.current_byte |= (data << rx_state.bit_position);
    rx_state.bit_position += 3;
    
    if (rx_state.bit_position >= 8) {
        u8 byte = rx_state.current_byte & 0xFF;
        rx_state.current_byte >>= 8;
        rx_state.bit_position -= 8;
        
        switch (rx_state.current_state) {
        case RX_IDLE:
            if (byte == HANDSHAKE_SYN) {
                pr_info("[epaper_rx] Handshake SYN received, sending ACK\n");
                send_handshake_ack();
            } else {
                if (byte > 250) {
                    pr_debug("[epaper_rx] Suspicious seq_num %d, ignoring\n", byte);
                    return;
                }
                rx_state.current_state = RX_SEQ_NUM;
                current_packet.seq_num = byte;
                update_state_timer();
                pr_debug("[epaper_rx] Received seq_num: %d\n", byte);
            }
            break;
            
        case RX_SEQ_NUM:
            rx_state.current_state = RX_DATA_LEN;
            current_packet.data_len = byte;
            update_state_timer();
            if (current_packet.data_len > MAX_PACKET_DATA) {
                pr_warn("[epaper_rx] Invalid data length: %d\n", current_packet.data_len);
                force_state_reset("Invalid data length");
                send_ack(false);
                return;
            }
            pr_debug("[epaper_rx] Received data_len: %d\n", byte);
            data_index = 0;
            break;
            
        case RX_DATA_LEN:
            if (current_packet.data_len == 0) {
                rx_state.current_state = RX_CRC32;
                rx_state.crc_byte_count = 0;
                current_packet.crc32 = byte;
            } else {
                rx_state.current_state = RX_DATA;
                if (data_index >= MAX_PACKET_DATA) {
                    force_state_reset("Data index overflow at first data byte");
                    return;
                }
                current_packet.data[data_index++] = byte;
                pr_debug("[epaper_rx] Received data[%d]: 0x%02x\n", data_index-1, byte);
            }
            break;
            
        case RX_DATA:
            if (data_index >= MAX_PACKET_DATA) {
                force_state_reset("Data index overflow during data reception");
                return;
            }
            if (data_index >= current_packet.data_len) {
                force_state_reset("Received more data bytes than expected");
                return;
            }
            current_packet.data[data_index++] = byte;
            pr_debug("[epaper_rx] Received data[%d]: 0x%02x\n", data_index-1, byte);
            if (data_index >= current_packet.data_len) {
                rx_state.current_state = RX_CRC32;
                rx_state.crc_byte_count = 0;
            }
            break;
            
        case RX_CRC32:
            if (rx_state.crc_byte_count >= 4) {
                force_state_reset("CRC byte count overflow");
                return;
            }
            
            switch (rx_state.crc_byte_count) {
            case 0:
                current_packet.crc32 = byte;
                break;
            case 1:
                current_packet.crc32 |= (u32)byte << 8;
                break;
            case 2:
                current_packet.crc32 |= (u32)byte << 16;
                break;
            case 3:
                current_packet.crc32 |= (u32)byte << 24;
                pr_debug("[epaper_rx] Received complete CRC32: 0x%08x\n", current_packet.crc32);
                
                if (verify_crc32(&current_packet)) {
                    if (current_packet.seq_num == rx_state.expected_seq) {
                        if (kfifo_avail(&rx_fifo) < current_packet.data_len) {
                            pr_warn("[epaper_rx] FIFO insufficient space - rejecting packet\n");
                            send_ack(false);
                        } else {
                            int stored = 0;
                            for (int i = 0; i < current_packet.data_len; i++) {
                                if (kfifo_put(&rx_fifo, current_packet.data[i])) {
                                    stored++;
                                } else {
                                    pr_err("[epaper_rx] FIFO error at byte %d\n", i);
                                    send_ack(false);
                                    force_state_reset("FIFO storage error");
                                    return;
                                }
                            }
                            
                            if (stored == current_packet.data_len) {
                                rx_state.expected_seq++;
                                send_ack(true);
                                wake_up(&data_waitqueue);
                                pr_info("[epaper_rx] Packet %d received successfully (%d bytes, CRC32 OK)\n", 
                                       current_packet.seq_num, current_packet.data_len);
                            } else {
                                force_state_reset("Partial FIFO storage");
                                return;
                            }
                        }
                    } else {
                        pr_warn("[epaper_rx] Wrong sequence: expected %d, got %d\n",
                               rx_state.expected_seq, current_packet.seq_num);
                        send_ack(false);
                    }
                } else {
                    pr_warn("[epaper_rx] CRC32 mismatch for seq %d\n", current_packet.seq_num);
                    memset(&current_packet, 0, sizeof(current_packet));
                    send_ack(false);
                }
                
                reset_rx_state();
                return;
            }
            rx_state.crc_byte_count++;
            break;
            
        default:
            force_state_reset("Unknown state");
            return;
        }
    }
}

static irqreturn_t clock_irq_handler(int irq, void *dev_id) {
    unsigned long current_time = jiffies;
    unsigned long flags;
    u8 data;
    static unsigned long burst_start_time = 0;
    static int burst_count = 0;
    const int MAX_BURST_COUNT = 1000;
    
    if (burst_start_time == 0) {
        burst_start_time = current_time;
        burst_count = 1;
    } else {
        if (time_before(current_time, burst_start_time + HZ)) {
            burst_count++;
            if (burst_count > MAX_BURST_COUNT) {
                pr_warn("[epaper_rx] Clock burst detected (%d clocks/sec), resetting\n", burst_count);
                spin_lock_irqsave(&rx_state_lock, flags);
                force_state_reset("Clock burst overload");
                spin_unlock_irqrestore(&rx_state_lock, flags);
                burst_start_time = 0;
                burst_count = 0;
                return IRQ_HANDLED;
            }
        } else {
            burst_start_time = current_time;
            burst_count = 1;
        }
    }
    
    if (rx_state.last_clock_time != 0 && 
        time_before(current_time, rx_state.last_clock_time + msecs_to_jiffies(1))) {
        pr_debug("[epaper_rx] Clock too fast, ignoring\n");
        return IRQ_HANDLED;
    }
    
    data = read_3bit_data();
    pr_info("[epaper_rx] Clock IRQ: data=0x%02x, state=%d\n", data, rx_state.current_state);
    
    if (!spin_trylock_irqsave(&rx_state_lock, flags)) {
        pr_debug("[epaper_rx] State lock busy, dropping clock\n");
        return IRQ_HANDLED;
    }
    
    if (rx_state.current_state >= RX_STATE_MAX) {
        force_state_reset("Invalid state in IRQ handler");
        spin_unlock_irqrestore(&rx_state_lock, flags);
        return IRQ_HANDLED;
    }
    
    rx_state.last_clock_time = current_time;
    process_3bit_data(data);
    spin_unlock_irqrestore(&rx_state_lock, flags);
    
    return IRQ_HANDLED;
}

static int rx_open(struct inode *inode, struct file *filp) {
    unsigned long flags;
    
    if (!mutex_trylock(&rx_mutex)) {
        return -EBUSY;
    }
    
    spin_lock_irqsave(&rx_state_lock, flags);
    reset_rx_state();
    spin_unlock_irqrestore(&rx_state_lock, flags);
    
    pr_info("[epaper_rx] Device opened\n");
    return 0;
}

static int rx_release(struct inode *inode, struct file *filp) {
    mutex_unlock(&rx_mutex);
    return 0;
}

static ssize_t rx_read(struct file *filp, char __user *buf, size_t count, loff_t *off) {
    u8 data;
    size_t bytes_read = 0;
    unsigned long flags;
    bool error_detected;
    int ret;
    
    if (count == 0) return 0;
    
    spin_lock_irqsave(&rx_state_lock, flags);
    error_detected = rx_state.error_detected;
    if (error_detected) {
        reset_rx_state();
    }
    spin_unlock_irqrestore(&rx_state_lock, flags);
    
    while (bytes_read < count) {
        if (kfifo_get(&rx_fifo, &data)) {
            if (copy_to_user(buf + bytes_read, &data, 1)) {
                return bytes_read > 0 ? bytes_read : -EFAULT;
            }
            bytes_read++;
        } else {
            if (filp->f_flags & O_NONBLOCK) {
                break;
            }
            
            ret = wait_event_interruptible(data_waitqueue, 
                                         !kfifo_is_empty(&rx_fifo));
            if (ret) {
                return ret;
            }
        }
    }
    
    return bytes_read;
}

static __poll_t rx_poll(struct file *filp, poll_table *wait) {
    __poll_t mask = 0;
    
    poll_wait(filp, &data_waitqueue, wait);
    
    if (!kfifo_is_empty(&rx_fifo)) {
        mask |= POLLIN | POLLRDNORM;
    }
    
    return mask;
}

static const struct file_operations rx_fops = {
    .owner = THIS_MODULE,
    .open = rx_open,
    .release = rx_release,
    .read = rx_read,
    .poll = rx_poll,
};

static int init_gpio(struct platform_device *pdev) {
    int i, ret, irq;
    
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        data_gpio[i] = gpiod_get_index(&pdev->dev, "rx-data", i, GPIOD_IN);
        if (IS_ERR(data_gpio[i])) {
            pr_err("[epaper_rx] Failed to get data GPIO %d: %ld\n", i, PTR_ERR(data_gpio[i]));
            ret = PTR_ERR(data_gpio[i]);
            goto cleanup_gpio;
        }
    }
    
    clock_gpio = gpiod_get(&pdev->dev, "rx-clock", GPIOD_IN);
    if (IS_ERR(clock_gpio)) {
        pr_err("[epaper_rx] Failed to get clock GPIO: %ld\n", PTR_ERR(clock_gpio));
        ret = PTR_ERR(clock_gpio);
        goto cleanup_gpio;
    }
    
    ack_gpio = gpiod_get(&pdev->dev, "rx-ack", GPIOD_OUT_LOW);
    if (IS_ERR(ack_gpio)) {
        pr_err("[epaper_rx] Failed to get ACK GPIO: %ld\n", PTR_ERR(ack_gpio));
        ret = PTR_ERR(ack_gpio);
        goto cleanup_gpio;
    }
    
    irq = gpiod_to_irq(clock_gpio);
    if (irq < 0) {
        pr_err("[epaper_rx] Failed to get clock IRQ\n");
        ret = irq;
        goto cleanup_gpio;
    }
    
    ret = request_irq(irq, clock_irq_handler, IRQF_TRIGGER_RISING, "epaper_clock", NULL);
    if (ret) {
        pr_err("[epaper_rx] Failed to request clock IRQ\n");
        goto cleanup_gpio;
    }
    
    return 0;

cleanup_gpio:
    if (!IS_ERR_OR_NULL(ack_gpio)) gpiod_put(ack_gpio);
    if (!IS_ERR_OR_NULL(clock_gpio)) gpiod_put(clock_gpio);
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        if (!IS_ERR_OR_NULL(data_gpio[i])) gpiod_put(data_gpio[i]);
    }
    return ret;
}

static int rx_probe(struct platform_device *pdev) {
    int ret;
    
    pdev_global = pdev;
    INIT_KFIFO(rx_fifo);
    
    timer_setup(&state_timeout_timer, state_timeout_callback, 0);
    
    ret = init_gpio(pdev);
    if (ret) return ret;
    
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[epaper_rx] Failed to allocate device number\n");
        goto cleanup_gpio;
    }
    
    cdev_init(&rx_cdev, &rx_fops);
    rx_cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&rx_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("[epaper_rx] Failed to add cdev\n");
        goto cleanup_chrdev;
    }
    
    rx_class = class_create(CLASS_NAME);
    if (IS_ERR(rx_class)) {
        pr_err("[epaper_rx] Failed to create class\n");
        ret = PTR_ERR(rx_class);
        goto cleanup_cdev;
    }
    
    rx_device = device_create(rx_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(rx_device)) {
        pr_err("[epaper_rx] Failed to create device\n");
        ret = PTR_ERR(rx_device);
        goto cleanup_class;
    }
    
    timer_setup(&state_timeout_timer, 0, 0);
    state_timeout_timer.function = state_timeout_callback;
    
    pr_info("[epaper_rx] RX driver initialized successfully\n");
    return 0;
    
cleanup_class:
    class_destroy(rx_class);
cleanup_cdev:
    cdev_del(&rx_cdev);
cleanup_chrdev:
    unregister_chrdev_region(dev_num, 1);
cleanup_gpio:
    if (!IS_ERR_OR_NULL(ack_gpio)) gpiod_put(ack_gpio);
    if (!IS_ERR_OR_NULL(clock_gpio)) gpiod_put(clock_gpio);
    for (int i = 0; i < DATA_PIN_COUNT; i++) {
        if (!IS_ERR_OR_NULL(data_gpio[i])) gpiod_put(data_gpio[i]);
    }
    return ret;
}

static void rx_remove(struct platform_device *pdev) {
    int i;
    
    device_destroy(rx_class, dev_num);
    class_destroy(rx_class);
    cdev_del(&rx_cdev);
    unregister_chrdev_region(dev_num, 1);
    free_irq(gpiod_to_irq(clock_gpio), NULL);
    
    gpiod_put(ack_gpio);
    gpiod_put(clock_gpio);
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        gpiod_put(data_gpio[i]);
    }
    
    del_timer_sync(&state_timeout_timer);
    
    pr_info("[epaper_rx] RX driver removed\n");
}

static const struct of_device_id epaper_rx_of_match[] = {
    { .compatible = "epaper,gpio-driver" },
    { }
};
MODULE_DEVICE_TABLE(of, epaper_rx_of_match);

static struct platform_driver epaper_rx_driver = {
    .probe = rx_probe,
    .remove = rx_remove,
    .driver = {
        .name = "epaper-rx",
        .of_match_table = epaper_rx_of_match,
    },
};

static int __init rx_driver_init(void) {
    return platform_driver_register(&epaper_rx_driver);
}

static void __exit rx_driver_exit(void) {
    platform_driver_unregister(&epaper_rx_driver);
}

module_init(rx_driver_init);
module_exit(rx_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Programming Assignment");
MODULE_DESCRIPTION("E-paper RX driver for reliable image data reception");
