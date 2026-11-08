#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jhp1023");
MODULE_DESCRIPTION("Morse Code Decoder Driver");

#define MAX_MORSE_PATTERN 8

struct button_dev {
    struct gpio_desc *gpio;
    int irq_num;
    int last_gpio_val;
    ktime_t event_time;
    ktime_t press_time;
    char pattern[MAX_MORSE_PATTERN];
    int pattern_len;
    bool button_pressed;
    bool decode_pending;
    struct timer_list morse_timer;
    spinlock_t lock;
    bool driver_removing;
};

static char decode_morse(const char *pattern, int len)
{
    if (len == 2 && strncmp(pattern, " -", 2) == 0) return 'A';
    if (len == 4 && strncmp(pattern, "-   ", 4) == 0) return 'B';
    if (len == 4 && strncmp(pattern, "- - ", 4) == 0) return 'C';
    if (len == 3 && strncmp(pattern, "-  ", 3) == 0) return 'D';
    if (len == 1 && pattern[0] == ' ') return 'E';
    if (len == 4 && strncmp(pattern, "  - ", 4) == 0) return 'F';
    if (len == 3 && strncmp(pattern, "-- ", 3) == 0) return 'G';
    if (len == 4 && strncmp(pattern, "    ", 4) == 0) return 'H';
    if (len == 2 && strncmp(pattern, "  ", 2) == 0) return 'I';
    if (len == 4 && strncmp(pattern, " ---", 4) == 0) return 'J';
    if (len == 3 && strncmp(pattern, "- -", 3) == 0) return 'K';
    if (len == 4 && strncmp(pattern, " -  ", 4) == 0) return 'L';
    if (len == 2 && strncmp(pattern, "--", 2) == 0) return 'M';
    if (len == 2 && strncmp(pattern, "- ", 2) == 0) return 'N';
    if (len == 3 && strncmp(pattern, "---", 3) == 0) return 'O';
    if (len == 4 && strncmp(pattern, " -- ", 4) == 0) return 'P';
    if (len == 4 && strncmp(pattern, "-- -", 4) == 0) return 'Q';
    if (len == 3 && strncmp(pattern, " - ", 3) == 0) return 'R';
    if (len == 3 && strncmp(pattern, "   ", 3) == 0) return 'S';
    if (len == 1 && pattern[0] == '-') return 'T';
    if (len == 3 && strncmp(pattern, "  -", 3) == 0) return 'U';
    if (len == 4 && strncmp(pattern, "   -", 4) == 0) return 'V';
    if (len == 3 && strncmp(pattern, " --", 3) == 0) return 'W';
    if (len == 4 && strncmp(pattern, "-  -", 4) == 0) return 'X';
    if (len == 4 && strncmp(pattern, "- --", 4) == 0) return 'Y';
    if (len == 4 && strncmp(pattern, "--  ", 4) == 0) return 'Z';
    return '?';
}

static void morse_timer_callback(struct timer_list *t)
{
    struct button_dev *dev = from_timer(dev, t, morse_timer);
    char decoded_char;
    unsigned long flags;
    int len;
    char pattern_copy[MAX_MORSE_PATTERN];
   
    spin_lock_irqsave(&dev->lock, flags);
   
    if (dev->driver_removing) {
        spin_unlock_irqrestore(&dev->lock, flags);
        return;
    }
   
    if (dev->pattern_len > 0 && dev->pattern_len < MAX_MORSE_PATTERN) {
        len = dev->pattern_len;
        memcpy(pattern_copy, dev->pattern, len);
        pattern_copy[len] = '\0';
       
        dev->pattern_len = 0;
        memset(dev->pattern, 0, sizeof(dev->pattern));
       
        spin_unlock_irqrestore(&dev->lock, flags);
       
        decoded_char = decode_morse(pattern_copy, len);
       
        pr_info("[MORSE DECODER] Pattern: %s (len=%d) -> Decoded Letter: '%c'\n",
                pattern_copy, len, decoded_char);
    } else {
        spin_unlock_irqrestore(&dev->lock, flags);
    }
}

static irqreturn_t button_th_handler(int irq, void *dev_id)
{
    struct button_dev *dev = dev_id;
   
    dev->last_gpio_val = gpiod_get_value(dev->gpio);
    dev->event_time = ktime_get();
   
    return IRQ_WAKE_THREAD;
}

static irqreturn_t button_bh_handler(int irq, void *dev_id)
{
    struct button_dev *dev = dev_id;
    int val = dev->last_gpio_val;
    ktime_t event_time = dev->event_time;
    s64 duration_ms;
    unsigned long flags;
    bool driver_removing;
   
    spin_lock_irqsave(&dev->lock, flags);
    driver_removing = dev->driver_removing;
    spin_unlock_irqrestore(&dev->lock, flags);
   
    if (driver_removing)
        return IRQ_HANDLED;
   
    if (val == 0) {
        del_timer(&dev->morse_timer);
       
        spin_lock_irqsave(&dev->lock, flags);
        dev->press_time = event_time;
        dev->button_pressed = true;
        spin_unlock_irqrestore(&dev->lock, flags);
       
    } else {
        spin_lock_irqsave(&dev->lock, flags);
       
        if (!dev->button_pressed) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return IRQ_HANDLED;
        }
       
        dev->button_pressed = false;
        duration_ms = ktime_ms_delta(event_time, dev->press_time);
       
        if (duration_ms > 10 && duration_ms < 5000) {
            if (dev->pattern_len < MAX_MORSE_PATTERN - 1) {
                if (duration_ms < 250) {
                    dev->pattern[dev->pattern_len++] = ' ';
                } else {
                    dev->pattern[dev->pattern_len++] = '-';
                }
            } else {
                pr_warn("[MORSE] Pattern buffer full, ignoring input\n");
            }
            spin_unlock_irqrestore(&dev->lock, flags);
           
            mod_timer(&dev->morse_timer, jiffies + msecs_to_jiffies(700));
           
        } else {
            spin_unlock_irqrestore(&dev->lock, flags);
            if (duration_ms <= 10) {
                pr_debug("[MORSE] Debounced: %lld ms\n", duration_ms);
            }
        }
    }
   
    return IRQ_HANDLED;
}

static int button_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct button_dev *button_dev;
    int ret;
   
    button_dev = devm_kzalloc(dev, sizeof(*button_dev), GFP_KERNEL);
    if (!button_dev)
        return -ENOMEM;
   
    spin_lock_init(&button_dev->lock);
    timer_setup(&button_dev->morse_timer, morse_timer_callback, 0);
   
    button_dev->pattern_len = 0;
    button_dev->press_time = 0;
    button_dev->button_pressed = false;
    button_dev->decode_pending = false;
    button_dev->driver_removing = false;
    memset(button_dev->pattern, 0, sizeof(button_dev->pattern));
   
    button_dev->gpio = devm_gpiod_get(dev, NULL, GPIOD_IN);
    if (IS_ERR(button_dev->gpio)) {
        dev_err(dev, "Failed to get GPIO descriptor\n");
        return PTR_ERR(button_dev->gpio);
    }
   
    button_dev->irq_num = gpiod_to_irq(button_dev->gpio);
    if (button_dev->irq_num < 0) {
        dev_err(dev, "Failed to get IRQ number\n");
        return button_dev->irq_num;
    }
   
    ret = devm_request_threaded_irq(dev,
                                    button_dev->irq_num,
                                    button_th_handler,
                                    button_bh_handler,
                                    IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                                    "button_morse",
                                    button_dev);
    if (ret) {
        dev_err(dev, "Failed to request threaded IRQ: %d\n", ret);
        return ret;
    }
   
    platform_set_drvdata(pdev, button_dev);
    dev_info(dev, "Morse Code Threaded IRQ Driver Ready (IRQ=%d)\n",
             button_dev->irq_num);
   
    return 0;
}

static void button_remove(struct platform_device *pdev)
{
    struct button_dev *dev = platform_get_drvdata(pdev);
    unsigned long flags;
   
    if (!dev)
        return;
   
    spin_lock_irqsave(&dev->lock, flags);
    dev->driver_removing = true;
    spin_unlock_irqrestore(&dev->lock, flags);
   
    disable_irq(dev->irq_num);
    del_timer_sync(&dev->morse_timer);
    synchronize_irq(dev->irq_num);
   
    dev_info(&pdev->dev, "Morse Code Driver Unloaded Safely\n");
}

static const struct of_device_id button_of_match[] = {
    { .compatible = "button", },
    { }
};
MODULE_DEVICE_TABLE(of, button_of_match);

static struct platform_driver button_platform_driver = {
    .probe  = button_probe,
    .remove = button_remove,
    .driver = {
        .name = "button_platform_driver",
        .of_match_table = button_of_match,
    },
};

module_platform_driver(button_platform_driver);


