#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 100

#define LONG_LONG_UPPER 0xFFFFFFFF00000000
#define LONG_LONG_LOWER 0x00000000FFFFFFFF
#define LL_MAX 0xFFFFFFFFFFFFFFFF
#define STRING_LEN 40

struct BigN {
    unsigned long long lower, upper;
};

static void bign_divide(struct BigN *num,
                        unsigned long long divisor,
                        unsigned long long *remainder)
{
    if (!num->upper) {
        *remainder = num->lower % divisor;
        num->lower = num->lower / divisor;
        return;
    }

    unsigned long long borrow = num->upper % divisor;
    num->upper = num->upper / divisor;
    *remainder = (LL_MAX % divisor) * borrow + borrow + num->lower % divisor;
    num->lower = num->lower / divisor + (LL_MAX / divisor) * borrow +
                 *remainder / divisor;
    *remainder = *remainder % divisor;
}

static int bign_minus(struct BigN *num1, struct BigN *num2, struct BigN *result)
{
    unsigned long long borrow = 0;
    result->lower = num1->lower - num2->lower;
    if (result->lower > num1->lower)
        borrow = 1;

    result->upper = num1->upper - num2->upper - borrow;

    if (result->upper + borrow > num1->upper)
        return 1;
    return 0;
}

static int bign_add(struct BigN *num1, struct BigN *num2, struct BigN *result)
{
    unsigned long long tmp = 0;
    result->lower = num1->lower + num2->lower;
    if (result->lower < num1->lower)
        tmp = 1;
    result->upper = num1->upper + num2->upper + tmp;
    if (~num1->upper >= num2->upper + tmp && tmp && ~num2->upper)
        return 0;
    return 1;
}

static int bign_left_shift(struct BigN *num, unsigned int size)
{
    unsigned long long mask = LL_MAX >> size;
    num->upper = (num->upper << size) + ((~mask & num->lower) >> (64 - size));
    num->lower = num->lower << size;

    if (num->upper && size >= __builtin_clzll(num->upper))
        return 1;
    else if (size >= __builtin_clzll(num->lower) + 64)
        return 1;
    return 0;
}

static int long_long_multiple(unsigned long long num1,
                              unsigned long long num2,
                              struct BigN *result)
{
    unsigned long long u1, u2, l1, l2, z2, z1, z0, tmp, carry = 0;
    u1 = (num1 & LONG_LONG_UPPER) >> 32;
    u2 = (num2 & LONG_LONG_UPPER) >> 32;
    l1 = num1 & LONG_LONG_LOWER;
    l2 = num2 & LONG_LONG_LOWER;
    z2 = u1 * u2;
    z0 = l1 * l2;
    tmp = u1 * l2;
    z1 = u2 * l1;
    z1 += tmp;
    if (z1 < tmp)
        carry = 1;

    carry = 0;
    struct BigN bign1 = {.upper = z2, .lower = z0};
    struct BigN bign2 = {
        .upper = ((z1 & LONG_LONG_UPPER) >> 32) + (carry << 32),
        .lower = (z1 & LONG_LONG_LOWER) << 32};
    return bign_add(&bign1, &bign2, result);
}

/*
 * Multiple two BigN numbers. If the value of multipling is bigger than 128
 * bits, return 1.
 */
int bign_multiple(struct BigN *num1, struct BigN *num2, struct BigN *result)
{
    /* overflow */
    if (num1->upper && num2->upper)
        return 1;

    struct BigN tmp;
    memset(&tmp, 0, sizeof(struct BigN));
    long_long_multiple(num1->lower, num2->lower, result);

    if (num1->upper)
        long_long_multiple(num1->upper, num2->lower, &tmp);
    else if (num2->upper)
        long_long_multiple(num2->upper, num1->lower, &tmp);
    else
        return 0;

    /* overflow */
    if (tmp.upper)
        return 1;

    result->upper += tmp.lower;
    /* overflow */
    if (result->upper < tmp.lower)
        return 1;

    return 0;
}

static void bign2string(struct BigN *num, char *string)
{
    memset(string, 0, sizeof(char) * STRING_LEN);
    unsigned long long remainder;
    int ptr = 0;

    if (!num->lower && !num->upper) {
        string[0] = '0';
        return;
    }

    while (num->lower || num->upper) {
        bign_divide(num, 10, &remainder);
        string[ptr++] = (unsigned char) (remainder) + 48;
    }

    /*
     * Reverse string
     */
    --ptr;
    for (int i = 0; i <= ptr / 2; ++i) {
        char tmp;
        tmp = string[i];
        string[i] = string[ptr - i];
        string[ptr - i] = tmp;
    }
}

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);
static DEFINE_MUTEX(cal_mutex);

static void fib_sequence(long long k, char *string)
{
    int j = 0;
    if (k == 0) {
        string[0] = '0';
        string[1] = '\0';

        return;
    }

    string[0] = '\0';
    struct BigN f[k + 2];
    f[0].lower = 0;
    f[0].upper = 0;
    f[1].lower = 1;
    f[1].upper = 0;
    unsigned long long mask = 0x8000000000000000;
    for (int i = __builtin_clzll(k); i < 64; ++i) {
        struct BigN tmp1, tmp2;
        /* f[2j] */
        tmp1.upper = f[j + 1].upper;
        tmp1.lower = f[j + 1].lower;
        bign_left_shift(&tmp1, 1);
        bign_minus(&tmp1, &f[j], &tmp2);
        bign_multiple(&tmp2, &f[j], &f[2 * j]);

        /* f[2j + 1] */
        bign_multiple(&f[j], &f[j], &tmp1);
        bign_multiple(&f[j + 1], &f[j + 1], &tmp2);
        bign_add(&tmp1, &tmp2, &f[2 * j + 1]);
        j = j * 2;
        if ((mask >> i) & k) {
            bign_add(&f[j], &f[j + 1], &f[j + 2]);
            ++j;
        }
    }

    bign2string(&f[j], string);
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    char string[40];
    fib_sequence(*offset, string);
    copy_to_user(buf, string, 40);
    return 0;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);
    mutex_init(&cal_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    cdev_init(fib_cdev, &fib_fops);
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
