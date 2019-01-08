#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <acpi/video.h>
#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/pci_ids.h>
#include <uapi/linux/input-event-codes.h>

MODULE_AUTHOR("kitsunyan");
MODULE_DESCRIPTION("ThinkPad T25 keyboard driver");
MODULE_LICENSE("GPL");

MODULE_SOFTDEP("pre: atkbd");

#define EC_ADDR_FN 0x46
#define EC_ADDR_FN_MASK 0x01

#define ATKBD_SCAN_MUTE 0xa0
#define ATKBD_SCAN_NULL 0xaa
#define ATKBD_SCAN_VOLUMEDOWN 0xae
#define ATKBD_SCAN_VOLUMEUP 0xb0
#define ATKBD_SCAN_HOME 0xc7
#define ATKBD_SCAN_END 0xcf
#define ATKBD_SCAN_INSERT 0xd2

#define FPKBD_INPUT_PRODUCT 0x5055
#define FPKBD_INPUT_VERSION 0x4101

enum fpkbd_scan {
	FPKBD_SCAN_NULL = -1,
	FPKBD_SCAN_MUTE,
	FPKBD_SCAN_VOLUMEDOWN,
	FPKBD_SCAN_VOLUMEUP,
	FPKBD_SCAN_F20,
	FPKBD_SCAN_COFFEE,
	FPKBD_SCAN_BATTERY,
	FPKBD_SCAN_F21,
	FPKBD_SCAN_CAMERA,
	FPKBD_SCAN_INSERT,
	FPKBD_SCAN_HOME,
	FPKBD_SCAN_END,
	FPKBD_SCAN_BRIGHTNESSDOWN,
	FPKBD_SCAN_BRIGHTNESSUP,
	FPKBD_SCAN_MAX
};

static u16 fpkbd_keycode_map[FPKBD_SCAN_MAX] = {
	KEY_MUTE,
	KEY_VOLUMEDOWN,
	KEY_VOLUMEUP,
	KEY_F20,
	KEY_COFFEE,
	KEY_BATTERY,
	KEY_F21,
	KEY_CAMERA,
	KEY_INSERT,
	KEY_HOME,
	KEY_END,
	KEY_BRIGHTNESSDOWN,
	KEY_BRIGHTNESSUP
};

struct atkbd_transform {
	unsigned int from_code;
	enum fpkbd_scan to_code_normal;
	enum fpkbd_scan to_code_fn;
};

static struct atkbd_transform atkbd_transform_list[6] = {
	{ ATKBD_SCAN_MUTE, FPKBD_SCAN_MUTE, FPKBD_SCAN_F20 }, /* Fn + F1 */
	{ ATKBD_SCAN_VOLUMEDOWN, FPKBD_SCAN_VOLUMEDOWN, FPKBD_SCAN_COFFEE }, /* Fn + F2 */
	{ ATKBD_SCAN_VOLUMEUP, FPKBD_SCAN_VOLUMEUP, FPKBD_SCAN_BATTERY }, /* Fn + F3 */
	{ ATKBD_SCAN_HOME, FPKBD_SCAN_HOME, FPKBD_SCAN_BRIGHTNESSDOWN }, /* Fn + Left */
	{ ATKBD_SCAN_END, FPKBD_SCAN_END, FPKBD_SCAN_BRIGHTNESSUP }, /* Fn + Right */
	{ ATKBD_SCAN_INSERT, FPKBD_SCAN_INSERT, FPKBD_SCAN_NULL }, /* Fn + End */
};

static int (** atkbd_fixup_ptr)(void *, unsigned int);
static int (* atkbd_fixup_old)(void *, unsigned int);
static struct kprobe acpi_video_kprobe;

static u8 atbuf[8];
static int atbuf_start = 0;
static int atbuf_end = 0;
static wait_queue_head_t atbuf_wait_queue;
static struct task_struct * atbuf_thread;

static int atkbd_emul_next;
static struct atkbd_transform * atkbd_transform_map[256];

static struct input_dev * fpkbd_input_dev;
static struct mutex fpkbd_input_dev_mutex;

static int fpkbd_atkbd_fixup(void * atkbd, unsigned int code) {
	if (atkbd_fixup_old != NULL) {
		code = atkbd_fixup_old(atkbd, code);
	}
	if (atkbd_emul_next) {
		atkbd_emul_next = 0;
		if (atkbd_transform_map[0xff & (code | 0x80)] != NULL) {
			if (!(code & 0x80)) {
				if ((atbuf_end >= atbuf_start &&
					atbuf_end - atbuf_start + 1 < ARRAY_SIZE(atbuf)) ||
					(atbuf_end < atbuf_start &&
					atbuf_start + atbuf_end + 1 < ARRAY_SIZE(atbuf))) {
					atbuf[atbuf_end] = code | 0x80;
					atbuf_end = atbuf_end + 1 == ARRAY_SIZE(atbuf) ? 0 : atbuf_end + 1;
				}
				wake_up_interruptible(&atbuf_wait_queue);
			}
			code = ATKBD_SCAN_NULL;
		}
	} else if (code == 0xe0) {
		atkbd_emul_next = 1;
	}
	return code;
}

static void fpkbd_input_dev_send_code(int code) {
	u16 key = fpkbd_keycode_map[code];
	if (key != KEY_RESERVED) {
		mutex_lock(&fpkbd_input_dev_mutex);
		input_event(fpkbd_input_dev, EV_MSC, MSC_SCAN, code);
		input_report_key(fpkbd_input_dev, key, 1);
		input_sync(fpkbd_input_dev);
		input_event(fpkbd_input_dev, EV_MSC, MSC_SCAN, code);
		input_report_key(fpkbd_input_dev, key, 0);
		input_sync(fpkbd_input_dev);
		mutex_unlock(&fpkbd_input_dev_mutex);
	}
}

static void fpkbd_kprobe_nothing(void) {}

static int fpkbd_acpi_video_pre(struct kprobe * p, struct pt_regs * regs) {
	if (regs->si == ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS ||
		regs->si == ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS) {
		if (regs->si == ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS) {
			fpkbd_input_dev_send_code(FPKBD_SCAN_F21);
		} else if (regs->si == ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS) {
			fpkbd_input_dev_send_code(FPKBD_SCAN_CAMERA);
		}
		regs->ip = (u64) fpkbd_kprobe_nothing;
		return 1;
	} else {
		return 0;
	}
}

static int atbuf_thread_callback(void * data) {
	u8 codes[ARRAY_SIZE(atbuf)];
	int i, count;
	u8 ec_value;
	struct atkbd_transform * transform;
	int fn_pressed;
	int code;
	while (!kthread_should_stop()) {
		wait_event_interruptible(atbuf_wait_queue, kthread_should_stop() || atbuf_start != atbuf_end);
		if (!kthread_should_stop()) {
			count = 0;
			while (atbuf_start != atbuf_end && count < ARRAY_SIZE(codes)) {
				codes[count++] = atbuf[atbuf_start];
				atbuf_start = atbuf_start + 1 == ARRAY_SIZE(atbuf) ? 0 : atbuf_start + 1;
			}
			if (count > 0) {
				if (!ec_read(EC_ADDR_FN, &ec_value)) {
					fn_pressed = !!(ec_value & EC_ADDR_FN_MASK);
				}
				for (i = 0; i < count; i++) {
					transform = atkbd_transform_map[codes[i]];
					code = fn_pressed ? transform->to_code_fn : transform->to_code_normal;
					if (code != FPKBD_SCAN_NULL) {
						fpkbd_input_dev_send_code(code);
					}
				}
			}
		}
	}
	return 0;
}

static void * atkbd_fixup_ptr_get(void) {
	return (void *) kallsyms_lookup_name("atkbd:atkbd_platform_scancode_fixup");
}

static int __init fpkbd_init(void) {
	int i;
	int error;
	atkbd_fixup_ptr = atkbd_fixup_ptr_get();
	if (atkbd_fixup_ptr == NULL) {
		pr_err("Can not get atkbd\n");
		error = -EMEDIUMTYPE;
		goto init_fail;
	}
	acpi_video_kprobe.pre_handler = fpkbd_acpi_video_pre;
	acpi_video_kprobe.post_handler = NULL;
	acpi_video_kprobe.fault_handler = NULL;
	acpi_video_kprobe.addr = (kprobe_opcode_t *) kallsyms_lookup_name("acpi_video_device_notify");
	if (acpi_video_kprobe.addr == NULL) {
		pr_err("Can not get acpi-video\n");
		error = -EMEDIUMTYPE;
		goto init_fail;
	}
	error = register_kprobe(&acpi_video_kprobe);
	if (error != 0) {
		pr_err("Can not register kprobe\n");
		goto init_fail;
	}
	atbuf_thread = kthread_create(atbuf_thread_callback, NULL, KBUILD_MODNAME "-atkbd");
	if (IS_ERR(atbuf_thread)) {
		pr_err("Can not create kthread\n");
		error = PTR_ERR(atbuf_thread);
		goto kthread_create_fail;
	}
	fpkbd_input_dev = input_allocate_device();
	if (fpkbd_input_dev == NULL) {
		pr_err("Can not allocate input device\n");
		error = -ENOMEM;
		goto input_allocate_device_fail;
	}
	fpkbd_input_dev->name = "ThinkPad T25 Extra Buttons";
	fpkbd_input_dev->phys = KBUILD_MODNAME "/input0";
	fpkbd_input_dev->id.bustype = BUS_HOST;
	fpkbd_input_dev->id.vendor = PCI_VENDOR_ID_LENOVO;
	fpkbd_input_dev->id.product = FPKBD_INPUT_PRODUCT;
	fpkbd_input_dev->id.version = FPKBD_INPUT_VERSION;
	fpkbd_input_dev->keycodesize = sizeof(fpkbd_keycode_map[0]);
	fpkbd_input_dev->keycodemax = FPKBD_SCAN_MAX;
	fpkbd_input_dev->keycode = fpkbd_keycode_map;
	input_set_capability(fpkbd_input_dev, EV_MSC, MSC_SCAN);
	for (i = 0; i < FPKBD_SCAN_MAX; i++) {
		input_set_capability(fpkbd_input_dev, EV_KEY, fpkbd_keycode_map[i]);
	}
	mutex_init(&fpkbd_input_dev_mutex);
	error = input_register_device(fpkbd_input_dev);
	if (error != 0) {
		pr_err("Can not register input device\n");
		goto input_register_device_fail;
	}
	memset(atkbd_transform_map, 0, sizeof(atkbd_transform_map));
	for (i = 0; i < ARRAY_SIZE(atkbd_transform_list); i++) {
		atkbd_transform_map[atkbd_transform_list[i].from_code] = &atkbd_transform_list[i];
	}
	atkbd_fixup_old = *atkbd_fixup_ptr;
	*atkbd_fixup_ptr = fpkbd_atkbd_fixup;
	atkbd_emul_next = 0;
	init_waitqueue_head(&atbuf_wait_queue);
	wake_up_process(atbuf_thread);
	return 0;
input_register_device_fail:
	kfree(fpkbd_input_dev->keycode);
	input_free_device(fpkbd_input_dev);
input_allocate_device_fail:
	kthread_stop(atbuf_thread);
kthread_create_fail:
	unregister_kprobe(&acpi_video_kprobe);
init_fail:
	return error;
}

static void __exit fpkbd_exit(void) {
	if (atkbd_fixup_ptr == atkbd_fixup_ptr_get()) {
		*atkbd_fixup_ptr = atkbd_fixup_old;
	}
	unregister_kprobe(&acpi_video_kprobe);
	kthread_stop(atbuf_thread);
	input_unregister_device(fpkbd_input_dev);
}

module_init(fpkbd_init);
module_exit(fpkbd_exit);
