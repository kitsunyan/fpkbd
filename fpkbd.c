#include <acpi/video.h>
#include <linux/acpi.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <uapi/linux/input-event-codes.h>

MODULE_AUTHOR("kitsunyan");
MODULE_DESCRIPTION("ThinkPad T25 keyboard driver");
MODULE_LICENSE("GPL");

MODULE_SOFTDEP("pre: atkbd");
MODULE_SOFTDEP("pre: thinkpad_acpi");

#define EC_ADDR_FN 0x46
#define EC_ADDR_FN_MASK 0x01

#define ATSCAN_MUTE 0xa0
#define ATSCAN_NULL 0xaa
#define ATSCAN_VOLUMEDOWN 0xae
#define ATSCAN_VOLUMEUP 0xb0
#define ATSCAN_HOME 0xc7
#define ATSCAN_END 0xcf
#define ATSCAN_INSERT 0xd2

static struct {
	unsigned int from_atscan;
	unsigned int to_key_normal;
	unsigned int to_key_fn;
} atkbd_transform[6] = {
	{ ATSCAN_MUTE, KEY_MUTE, KEY_F20 }, /* Fn + F1 */
	{ ATSCAN_VOLUMEDOWN, KEY_VOLUMEDOWN, KEY_COFFEE }, /* Fn + F2 */
	{ ATSCAN_VOLUMEUP, KEY_VOLUMEUP, KEY_BATTERY }, /* Fn + F3 */
	{ ATSCAN_HOME, KEY_HOME, KEY_BRIGHTNESSDOWN }, /* Fn + Left */
	{ ATSCAN_END, KEY_END, KEY_BRIGHTNESSUP }, /* Fn + Right */
	{ ATSCAN_INSERT, KEY_INSERT, KEY_RESERVED }, /* Fn + End */
};

static int (** atkbd_fixup_ptr)(void *, unsigned int);
static int (* atkbd_fixup_old)(void *, unsigned int);
static struct kprobe video_hotkey;

static u8 atbuf[8];
static int atbuf_start = 0;
static int atbuf_end = 0;
static wait_queue_head_t atbuf_wait;
static struct task_struct * atbuf_thread;

static int atkbd_emul_next;

static int fpkbd_atkbd_fixup(void * atkbd, unsigned int code) {
	int i;
	if (atkbd_fixup_old != NULL) {
		code = atkbd_fixup_old(atkbd, code);
	}
	if (atkbd_emul_next) {
		atkbd_emul_next = 0;
		for (i = 0; i < ARRAY_SIZE(atkbd_transform); i++) {
			if ((code | 0x80) == atkbd_transform[i].from_atscan) {
				if (!(code & 0x80)) {
					if ((atbuf_end >= atbuf_start &&
						atbuf_end - atbuf_start + 1 < ARRAY_SIZE(atbuf)) ||
						(atbuf_end < atbuf_start &&
						atbuf_start + atbuf_end + 1 < ARRAY_SIZE(atbuf))) {
						atbuf[atbuf_end] = code | 0x80;
						atbuf_end = atbuf_end + 1 == ARRAY_SIZE(atbuf) ? 0 : atbuf_end + 1;
					}
					wake_up_interruptible(&atbuf_wait);
				}
				code = ATSCAN_NULL;
				break;
			}
		}
	} else if (code == 0xe0) {
		atkbd_emul_next = 1;
	}
	return code;
}

static void fpkbd_tpacpi_emulate_hotkey(int key) {
	void (* send_key)(unsigned int) = (void *) kallsyms_lookup_name("thinkpad_acpi:tpacpi_input_send_key");
	u16 ** keycode_map = (void *) kallsyms_lookup_name("thinkpad_acpi:hotkey_keycode_map");
	int size;
	int i;
	if (send_key != NULL && keycode_map != NULL && *keycode_map != NULL) {
		size = ksize(*keycode_map) / sizeof(u16);
		for (i = 0; i < size; i++) {
			if ((*keycode_map)[i] == key) {
				send_key(i);
				break;
			}
		}
	}
}

static void fpkbd_video_hotkey_nothing(void) {}

static int fpkbd_video_hotkey_pre(struct kprobe * p, struct pt_regs * regs) {
	if (regs->si == ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS ||
		regs->si == ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS) {
		if (regs->si == ACPI_VIDEO_NOTIFY_DEC_BRIGHTNESS) {
			fpkbd_tpacpi_emulate_hotkey(KEY_F21);
		} else if (regs->si == ACPI_VIDEO_NOTIFY_INC_BRIGHTNESS) {
			fpkbd_tpacpi_emulate_hotkey(KEY_CAMERA);
		}
		regs->ip = (u64) fpkbd_video_hotkey_nothing;
		return 1;
	} else {
		return 0;
	}
}

static int atbuf_thread_callback(void * data) {
	u8 codes[ARRAY_SIZE(atbuf)];
	int count;
	int i, j;
	u8 ec_value;
	int fn_pressed;
	int key;
	while (!kthread_should_stop()) {
		wait_event_interruptible(atbuf_wait, kthread_should_stop() || atbuf_start != atbuf_end);
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
					for (j = 0; j < ARRAY_SIZE(atkbd_transform); j++) {
						if (atkbd_transform[j].from_atscan == codes[i]) {
							key = fn_pressed ? atkbd_transform[j].to_key_fn
								: atkbd_transform[j].to_key_normal;
							if (key != KEY_RESERVED) {
								fpkbd_tpacpi_emulate_hotkey(key);
							}
							break;
						}
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
	int code;
	atkbd_fixup_ptr = atkbd_fixup_ptr_get();
	if (atkbd_fixup_ptr == NULL) {
		printk(KERN_ERR "Can not get atkbd\n");
		return -EMEDIUMTYPE;
	}
	video_hotkey.pre_handler = fpkbd_video_hotkey_pre;
	video_hotkey.post_handler = NULL;
	video_hotkey.fault_handler = NULL;
	video_hotkey.addr = (kprobe_opcode_t *) kallsyms_lookup_name("acpi_video_device_notify");
	if (video_hotkey.addr == NULL) {
		printk(KERN_ERR "Can not get acpi-video\n");
		return -EMEDIUMTYPE;
	}
	code = register_kprobe(&video_hotkey);
	if (code != 0) {
		printk(KERN_ERR "Can not register kprobe\n");
		return code;
	}
	atbuf_thread = kthread_create(atbuf_thread_callback, NULL, "fpkbd-atkbd");
	if (IS_ERR(atbuf_thread)) {
		printk(KERN_ERR "Can not create kthread\n");
		unregister_kprobe(&video_hotkey);
		return PTR_ERR(atbuf_thread);
	}
	atkbd_fixup_old = *atkbd_fixup_ptr;
	*atkbd_fixup_ptr = fpkbd_atkbd_fixup;
	atkbd_emul_next = 0;
	init_waitqueue_head(&atbuf_wait);
	wake_up_process(atbuf_thread);
	return 0;
}

static void __exit fpkbd_exit(void) {
	if (atkbd_fixup_ptr == atkbd_fixup_ptr_get()) {
		*atkbd_fixup_ptr = atkbd_fixup_old;
	}
	unregister_kprobe(&video_hotkey);
	kthread_stop(atbuf_thread);
}

module_init(fpkbd_init);
module_exit(fpkbd_exit);
