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

static const struct of_device_id epaper_tx_of_match[] = {
    { .compatible = "epaper,gpio-tx" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, epaper_tx_of_match);

static struct device *tx_gpio_dev;

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
static int init_gpio_pins(struct device *dev);
static int epaper_tx_probe(struct platform_device *pdev);
static void epaper_tx_remove(struct platform_device *pdev);

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
    int bit0 = (data >> 0) & 1;
    int bit1 = (data >> 1) & 1;
    int bit2 = (data >> 2) & 1;
    
    pr_debug("[epaper_tx] Sending 3-bit data: 0x%02x (GPIO5=%d, GPIO6=%d, GPIO12=%d)\n", 
             data, bit0, bit1, bit2);
    
    gpiod_set_value(data_gpio[0], bit0);
    gpiod_set_value(data_gpio[1], bit1);
    gpiod_set_value(data_gpio[2], bit2);
    
    wmb();
    mdelay(1);
    
    int read_bit0 = gpiod_get_value(data_gpio[0]);
    int read_bit1 = gpiod_get_value(data_gpio[1]);
    int read_bit2 = gpiod_get_value(data_gpio[2]);
    
    if (read_bit0 != bit0 || read_bit1 != bit1 || read_bit2 != bit2) {
        pr_warn("[epaper_tx] GPIO readback mismatch! Set: [%d,%d,%d], Read: [%d,%d,%d]\n",
                bit0, bit1, bit2, read_bit0, read_bit1, read_bit2);
    }
    
    gpiod_set_value(clock_gpio, 0);
    mdelay(1);
    gpiod_set_value(clock_gpio, 1);
    mdelay(1);
    gpiod_set_value(clock_gpio, 0);
    mdelay(1);
}

static void send_byte(u8 byte) {
    pr_info("[epaper_tx] Sending byte: 0x%02x\n", byte);
    
    gpiod_set_value(clock_gpio, 0);
    mdelay(2);
    
    send_3bit_data(byte & 0x07);
    send_3bit_data((byte >> 3) & 0x07);
    send_3bit_data((byte >> 6) & 0x03);
    
    mdelay(2);
}

static int wait_for_ack(void) {
    int ret;
    unsigned long timeout_jiffies = msecs_to_jiffies(TIMEOUT_MS);
    unsigned long start_time = jiffies;
    int initial_ack_gpio, final_ack_gpio;
    int poll_count = 0;
    
    initial_ack_gpio = gpiod_get_value(ack_gpio);
    pr_info("[epaper_tx] Waiting for ACK (handshake, timeout: %dms, initial ACK GPIO: %d)\n", 
            TIMEOUT_MS, initial_ack_gpio);
    
    atomic_set(&ack_received, 0);
    atomic_set(&ack_status, 0);
    
    ret = wait_event_timeout(ack_waitqueue, 
                           ({
                               if (++poll_count % 100 == 0) {
                                   int current_ack = gpiod_get_value(ack_gpio);
                                   pr_debug("[epaper_tx] Poll %d: ACK GPIO=%d, ack_received=%d\n", 
                                           poll_count/100, current_ack, atomic_read(&ack_received));
                               }
                               atomic_read(&ack_received);
                           }), 
                           timeout_jiffies);
    
    unsigned long elapsed_ms = jiffies_to_msecs(jiffies - start_time);
    final_ack_gpio = gpiod_get_value(ack_gpio);
    
    if (ret == 0) {
        pr_warn("[epaper_tx] *** ACK TIMEOUT *** after %lums (seq: %d, retry: %d)\n", 
                elapsed_ms, current_state.last_seq_sent, current_state.retry_count);
        pr_warn("[epaper_tx] ACK GPIO: initial=%d, final=%d, ack_received=%d, polls=%d\n", 
                initial_ack_gpio, final_ack_gpio, atomic_read(&ack_received), poll_count);
        pr_warn("[epaper_tx] Recommendation: Check RX logs, /proc/interrupts for ACK IRQ count\n");
        return -ETIMEDOUT;
    }
    
    smp_rmb();
    
    if (atomic_read(&ack_status)) {
        pr_info("[epaper_tx] *** ACK SUCCESS *** for seq %d (elapsed: %lums, polls: %d)\n", 
                current_state.last_seq_sent, elapsed_ms, poll_count);
        pr_info("[epaper_tx] ACK GPIO: initial=%d, final=%d\n", 
                initial_ack_gpio, final_ack_gpio);
        return 0;
    } else {
        pr_warn("[epaper_tx] *** NACK RECEIVED *** for seq %d (elapsed: %lums)\n", 
                current_state.last_seq_sent, elapsed_ms);
        pr_warn("[epaper_tx] ACK GPIO: initial=%d, final=%d\n", 
                initial_ack_gpio, final_ack_gpio);
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
            mdelay(50 + current_state.retry_count * 10);
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
    static int irq_count = 0;
    unsigned long current_time = jiffies;
    bool current_ack;
    unsigned long time_since_last = 0;
    
    irq_count++;
    
    if (last_ack_time != 0) {
        time_since_last = jiffies_to_msecs(current_time - last_ack_time);
    }
    
    current_ack = gpiod_get_value(ack_gpio);
    
    pr_info("[epaper_tx] *** ACK IRQ #%d *** GPIO=%d, time_since_last=%lums\n", 
            irq_count, current_ack, time_since_last);
    
    if (time_before(current_time, last_ack_time + msecs_to_jiffies(5))) {
        pr_debug("[epaper_tx] IRQ debounced (too soon: %lums)\n", time_since_last);
        return IRQ_HANDLED;
    }
    
    pr_info("[epaper_tx] ACK context: transmission_active=%d, handshake_complete=%d, seq=%d\n",
            current_state.transmission_active, current_state.handshake_complete, 
            current_state.last_seq_sent);
    
    atomic_set(&ack_status, 1);
    atomic_set(&ack_received, 1);
    
    smp_wmb();
    wake_up(&ack_waitqueue);
    last_ack_time = current_time;
    
    pr_info("[epaper_tx] *** ACK PROCESSED *** - wakeup sent to waiting thread (GPIO=%d)\n", current_ack);
    
    return IRQ_HANDLED;
}

static int perform_handshake(void) {
    int retry, ret;
    int last_error = -ECONNREFUSED;
    int ack_gpio_state;
    
    pr_info("[epaper_tx] === HANDSHAKE SEQUENCE START ===\n");
    
    ack_gpio_state = gpiod_get_value(ack_gpio);
    pr_info("[epaper_tx] Initial GPIO states - ACK:%d, Clock:%d, Data:[%d,%d,%d]\n",
            ack_gpio_state, gpiod_get_value(clock_gpio),
            gpiod_get_value(data_gpio[0]), gpiod_get_value(data_gpio[1]), 
            gpiod_get_value(data_gpio[2]));
    
    for (retry = 0; retry < MAX_RETRIES; retry++) {
        pr_info("[epaper_tx] === Handshake attempt %d/%d ===\n", retry + 1, MAX_RETRIES);
        
        atomic_set(&ack_received, 0);
        atomic_set(&ack_status, 0);
        
        pr_info("[epaper_tx] Sending HANDSHAKE_SYN (0x%02x)...\n", HANDSHAKE_SYN);
        
        atomic_set(&ack_received, 0);
        atomic_set(&ack_status, 0);
        
        send_byte(HANDSHAKE_SYN);
        
        ack_gpio_state = gpiod_get_value(ack_gpio);
        pr_info("[epaper_tx] Post-SYN GPIO states - ACK:%d, Clock:%d, Data:[%d,%d,%d]\n",
                ack_gpio_state, gpiod_get_value(clock_gpio),
                gpiod_get_value(data_gpio[0]), gpiod_get_value(data_gpio[1]), 
                gpiod_get_value(data_gpio[2]));
        
        pr_info("[epaper_tx] Waiting for SYN-ACK response...\n");
        
        ret = wait_event_timeout(ack_waitqueue, 
                               atomic_read(&ack_received), 
                               msecs_to_jiffies(TIMEOUT_MS));
        
        if (ret > 0 && atomic_read(&ack_status)) {
            current_state.handshake_complete = true;
            tx_stats.successful_handshakes++;
            pr_info("[epaper_tx] *** HANDSHAKE SUCCESS on attempt %d ***\n", retry + 1);
            pr_info("[epaper_tx] === HANDSHAKE SEQUENCE COMPLETE ===\n");
            return 0;
        }
        
        last_error = (ret == 0) ? -ETIMEDOUT : -ECOMM;
        ack_gpio_state = gpiod_get_value(ack_gpio);
        
        if (ret == -ETIMEDOUT) {
            pr_warn("[epaper_tx] Handshake timeout on attempt %d (ACK GPIO: %d)\n", 
                    retry + 1, ack_gpio_state);
        } else if (ret == -ECOMM) {
            pr_warn("[epaper_tx] Handshake NACK on attempt %d (ACK GPIO: %d)\n", 
                    retry + 1, ack_gpio_state);
        } else {
            pr_warn("[epaper_tx] Handshake error %d on attempt %d (ACK GPIO: %d)\n", 
                    ret, retry + 1, ack_gpio_state);
        }
        
        if (retry < MAX_RETRIES - 1) {
            pr_info("[epaper_tx] Waiting 80ms before retry...\n");
            mdelay(80);
        }
    }
    
    tx_stats.failed_handshakes++;
    pr_err("[epaper_tx] === HANDSHAKE SEQUENCE FAILED ===\n");
    
    if (last_error == -ETIMEDOUT) {
        pr_err("[epaper_tx] Final diagnosis: Receiver not responding after %d attempts\n", MAX_RETRIES);
        pr_err("[epaper_tx] Check: 1) RX driver loaded? 2) ACK GPIO wiring? 3) RX receiving SYN?\n");
        return -EHOSTUNREACH;
    } else if (last_error == -ECOMM) {
        pr_err("[epaper_tx] Final diagnosis: Receiver rejecting connection after %d attempts\n", MAX_RETRIES);
        return -ECONNREFUSED;
    } else {
        pr_err("[epaper_tx] Final diagnosis: Unknown error %d after %d attempts\n", last_error, MAX_RETRIES);
        return last_error;
    }
}

static int tx_open(struct inode *inode, struct file *filp) {
    pr_info("[epaper_tx] *** TX DEVICE OPEN CALLED ***\n");
    
    if (!mutex_trylock(&tx_mutex)) {
        pr_warn("[epaper_tx] Device busy\n");
        return -EBUSY;
    }
    
    reset_tx_state();
    pr_info("[epaper_tx] Device opened successfully\n");
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
    
    pr_info("[epaper_tx] *** TX WRITE CALLED: %zu bytes ***\n", count);
    
    if (count == 0) return 0;
    if (count > BUFFER_SIZE) count = BUFFER_SIZE;
    
    user_data = kmalloc(count, GFP_KERNEL);
    if (!user_data) {
        pr_err("[epaper_tx] Memory allocation failed\n");
        return -ENOMEM;
    }
    
    if (current_state.error_state) {
        reset_tx_state();
    }
    
    if (copy_from_user(user_data, buf, count)) {
        pr_err("[epaper_tx] Copy from user failed\n");
        kfree(user_data);
        return -EFAULT;
    }
    
    pr_info("[epaper_tx] Data copied from user, starting transmission\n");
    
    if (!current_state.handshake_complete) {
        pr_info("[epaper_tx] Performing handshake before data transfer\n");
        ret = perform_handshake();
        if (ret < 0) {
            pr_err("[epaper_tx] Handshake failed: %d\n", ret);
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

static int init_gpio_pins(struct device *dev) {
    int ret, i;
    
    if (!dev) {
        pr_err("[epaper_tx] No device provided\n");
        return -ENODEV;
    }
    
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        data_gpio[i] = gpiod_get_index(dev, "data", i, GPIOD_OUT_LOW);
        if (IS_ERR(data_gpio[i])) {
            pr_err("[epaper_tx] Failed to get data GPIO %d: %ld\n", 
                   i, PTR_ERR(data_gpio[i]));
            ret = PTR_ERR(data_gpio[i]);
            data_gpio[i] = NULL;
            goto cleanup_data_gpios;
        }
    }
    
    clock_gpio = gpiod_get(dev, "clock", GPIOD_OUT_LOW);
    if (IS_ERR(clock_gpio)) {
        pr_err("[epaper_tx] Failed to get clock GPIO: %ld\n", PTR_ERR(clock_gpio));
        ret = PTR_ERR(clock_gpio);
        clock_gpio = NULL;
        goto cleanup_data_gpios;
    }
    
    ack_gpio = gpiod_get(dev, "ack", GPIOD_IN);
    if (IS_ERR(ack_gpio)) {
        pr_err("[epaper_tx] Failed to get ACK GPIO: %ld\n", PTR_ERR(ack_gpio));
        ret = PTR_ERR(ack_gpio);
        ack_gpio = NULL;
        goto cleanup_clock_gpio;
    }
    
    pr_info("[epaper_tx] GPIO initialization successful\n");
    return 0;
    
cleanup_clock_gpio:
    if (clock_gpio) {
        gpiod_put(clock_gpio);
        clock_gpio = NULL;
    }
cleanup_data_gpios:
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        if (data_gpio[i]) {
            gpiod_put(data_gpio[i]);
            data_gpio[i] = NULL;
        }
    }
    return ret;
}

static int epaper_tx_probe(struct platform_device *pdev) {
    int ret, irq;
    
    pr_info("[epaper_tx] Probing platform device\n");
    
    tx_gpio_dev = &pdev->dev;
    
    ret = init_gpio_pins(&pdev->dev);
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
    
    pr_info("[epaper_tx] Platform device probed successfully\n");
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
    return ret;
}

static void epaper_tx_remove(struct platform_device *pdev) {
    int i;
    
    device_destroy(tx_class, dev_num);
    class_destroy(tx_class);
    cdev_del(&tx_cdev);
    unregister_chrdev_region(dev_num, 1);
    
    free_irq(gpiod_to_irq(ack_gpio), NULL);
    
    for (i = 0; i < DATA_PIN_COUNT; i++) {
        if (data_gpio[i]) {
            gpiod_put(data_gpio[i]);
            data_gpio[i] = NULL;
        }
    }
    if (clock_gpio) {
        gpiod_put(clock_gpio);
        clock_gpio = NULL;
    }
    if (ack_gpio) {
        gpiod_put(ack_gpio);
        ack_gpio = NULL;
    }
    
    pr_info("[epaper_tx] Platform device removed\n");
}

static struct platform_driver epaper_tx_platform_driver = {
    .probe = epaper_tx_probe,
    .remove = epaper_tx_remove,
    .driver = {
        .name = "epaper-gpio-tx",
        .of_match_table = epaper_tx_of_match,
    },
};

static int __init tx_driver_init(void) {
    pr_info("[epaper_tx] Initializing TX driver\n");
    
    return platform_driver_register(&epaper_tx_platform_driver);
}

static void __exit tx_driver_exit(void) {
    platform_driver_unregister(&epaper_tx_platform_driver);
    pr_info("[epaper_tx] TX driver unloaded\n");
}

module_init(tx_driver_init);
module_exit(tx_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("System Programming Assignment");
MODULE_DESCRIPTION("E-paper TX driver for reliable image data transmission");
