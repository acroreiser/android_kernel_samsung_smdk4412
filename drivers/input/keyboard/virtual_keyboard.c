

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>

#define VKBD_PROC_NAME	"kbd"
#define VKBD_INPUT_LEN	5

#define VKBD_RELEASE	0
#define VKBD_PRESS	1

struct vkbd_dev {
	long last_entry;
	spinlock_t lock;
	struct input_dev *device;
};

static struct vkbd_dev * vkbd = NULL;

void report_virtual_key(int code, int state)
{
	if(vkbd->device == NULL)
		return;

	input_event(vkbd->device, EV_KEY, code, state);
	input_sync(vkbd->device);
}
EXPORT_SYMBOL(report_virtual_key);

static int __init vkbd_init(void)
{
	int err = 0;
	int i = 0;

	printk(KERN_INFO "loading virtual keyboard driver\n");

	vkbd = kmalloc(sizeof(struct vkbd_dev), GFP_KERNEL);
	if(vkbd == NULL) {
		printk(KERN_ERR "cannot allocate vkbd device structure\n");
		err = -ENOMEM;
		goto err_alloc_device;
	}

	vkbd->device = input_allocate_device();
	if(vkbd->device == NULL) {
		printk(KERN_ERR "cannot allocate vkbd device\n");
		err = -ENOMEM;
		goto err_alloc_dev;
	}

	vkbd->device->name = "vkbd";
	vkbd->device->id.bustype = BUS_VIRTUAL;
	vkbd->device->id.product = 0x0000;
	vkbd->device->id.vendor = 0x0000;
	vkbd->device->id.version = 0x0000;

	vkbd->device->evbit[0] = BIT_MASK(EV_KEY);

	for(i = 0; i < BIT_WORD(KEY_MAX); i++) {
		vkbd->device->keybit[i] = 0xffff;
	}

	if(input_register_device(vkbd->device)) {
		printk(KERN_ERR "cannot register vkbd input device\n");
		err = -ENODEV;
		goto err_init_dev;
	}

	return 0;

err_init_dev:
	input_free_device(vkbd->device);
err_alloc_dev:
err_alloc_proc:
	kfree(vkbd);
err_alloc_device:

	return err;
}

static void __exit vkbd_end(void)
{
	printk(KERN_INFO "unloading virtual keyboard driver\n");

	input_unregister_device(vkbd->device);
	input_free_device(vkbd->device);
	kfree(vkbd);
}

module_init(vkbd_init);
module_exit(vkbd_end);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("blunderer <blunderer@blunderer.org>");
MODULE_DESCRIPTION("emulate kbd event thru /proc/kbd");

