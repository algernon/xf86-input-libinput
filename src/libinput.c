/*
 * Copyright © 2013 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <xorg-server.h>
#include <exevents.h>
#include <xf86Xinput.h>
#include <xserver-properties.h>
#include <libinput.h>
#include <linux/input.h>

#define TOUCHPAD_MAX_BUTTONS 7 /* three buttons, 4 scroll buttons */
#define TOUCHPAD_NUM_AXES 4 /* x, y, hscroll, vscroll */
#define TOUCH_MAX_SLOTS 15
#define XORG_KEYCODE_OFFSET 8

/*
   libinput does not provide axis information for absolute devices, instead
   it scales into the screen dimensions provided. So we set up the axes with
   a fixed range, let libinput scale into that range and then the server
   do the scaling it usually does.
 */
#define TOUCH_AXIS_MAX 0xffff

struct xf86libinput_driver {
	struct libinput *libinput;
	int device_count;
};

static struct xf86libinput_driver driver_context;

struct xf86libinput {
	char *path;
	struct libinput_device *device;

	int scroll_vdist;
	int scroll_hdist;
	int scroll_vdist_remainder;
	int scroll_hdist_remainder;

	struct {
		double x;
		double y;
		double x_remainder;
		double y_remainder;
	} scale;
};

static int
xf86libinput_on(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput *libinput = driver_context.libinput;
	struct libinput_device *device = driver_data->device;

	device = libinput_path_add_device(libinput, driver_data->path);
	if (!device)
		return !Success;
	libinput_device_ref(device);
	libinput_device_set_user_data(device, pInfo);
	driver_data->device = device;

	pInfo->fd = libinput_get_fd(libinput);
	/* Can't use xf86AddEnabledDevice here */
	AddEnabledDevice(pInfo->fd);
	dev->public.on = TRUE;

	return Success;
}

static int
xf86libinput_off(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;

	RemoveEnabledDevice(pInfo->fd);
	pInfo->fd = -1;
	dev->public.on = FALSE;

	libinput_path_remove_device(driver_data->device);
	libinput_device_unref(driver_data->device);
	driver_data->device = NULL;

	return Success;
}

static void
xf86libinput_ptr_ctl(DeviceIntPtr dev, PtrCtrl *ctl)
{
}


static void
init_button_map(unsigned char *btnmap, size_t size)
{
	int i;

	memset(btnmap, 0, size);
	for (i = 0; i <= TOUCHPAD_MAX_BUTTONS; i++)
		btnmap[i] = i;
}

static void
init_button_labels(Atom *labels, size_t size)
{
	memset(labels, 0, size * sizeof(Atom));
	labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
	labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
	labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
	labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
	labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
	labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
	labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
}

static void
init_axis_labels(Atom *labels, size_t size)
{
	memset(labels, 0, size * sizeof(Atom));
	labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
	labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
	labels[2] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_HSCROLL);
	labels[3] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_VSCROLL);
}

static int
xf86libinput_init_pointer(InputInfoPtr pInfo)
{
	DeviceIntPtr dev= pInfo->dev;
	struct xf86libinput *driver_data = pInfo->private;
	int min, max, res;

	unsigned char btnmap[TOUCHPAD_MAX_BUTTONS + 1];
	Atom btnlabels[TOUCHPAD_MAX_BUTTONS];
	Atom axislabels[TOUCHPAD_NUM_AXES];

	init_button_map(btnmap, ARRAY_SIZE(btnmap));
	init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
	init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

	InitPointerDeviceStruct((DevicePtr)dev, btnmap,
				TOUCHPAD_MAX_BUTTONS,
				btnlabels,
				xf86libinput_ptr_ctl,
				GetMotionHistorySize(),
				TOUCHPAD_NUM_AXES,
				axislabels);
	min = -1;
	max = -1;
	res = 0;

	xf86InitValuatorAxisStruct(dev, 0,
			           XIGetKnownProperty(AXIS_LABEL_PROP_REL_X),
				   min, max, res * 1000, 0, res * 1000, Relative);
	xf86InitValuatorAxisStruct(dev, 1,
			           XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y),
				   min, max, res * 1000, 0, res * 1000, Relative);

	SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL, driver_data->scroll_hdist, 0);
	SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL, driver_data->scroll_vdist, 0);

	return Success;
}

static void
xf86libinput_kbd_ctrl(DeviceIntPtr device, KeybdCtrl *ctrl)
{
#define CAPSFLAG	1
#define NUMFLAG		2
#define SCROLLFLAG	4

    static struct { int xbit, code; } bits[] = {
        { CAPSFLAG,	LIBINPUT_LED_CAPS_LOCK },
        { NUMFLAG,	LIBINPUT_LED_NUM_LOCK },
        { SCROLLFLAG,	LIBINPUT_LED_SCROLL_LOCK },
	{ 0, 0 },
    };
    int i = 0;
    enum libinput_led leds = 0;
    InputInfoPtr pInfo = device->public.devicePrivate;
    struct xf86libinput *driver_data = pInfo->private;
    struct libinput_device *ldevice = driver_data->device;

    while (bits[i].xbit) {
	    leds |= !!(ctrl->leds & bits[i].xbit);
	    i++;
    }

    libinput_device_led_update(ldevice, leds);
}

static void
xf86libinput_init_keyboard(InputInfoPtr pInfo)
{
	DeviceIntPtr dev= pInfo->dev;

	InitKeyboardDeviceStruct(dev, NULL, NULL,
				 xf86libinput_kbd_ctrl);
}

static void
xf86libinput_init_touch(InputInfoPtr pInfo)
{
	DeviceIntPtr dev = pInfo->dev;
	int min, max, res;
	unsigned char btnmap[TOUCHPAD_MAX_BUTTONS + 1];
	Atom btnlabels[TOUCHPAD_MAX_BUTTONS];
	Atom axislabels[TOUCHPAD_NUM_AXES];

	init_button_map(btnmap, ARRAY_SIZE(btnmap));
	init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
	init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

	InitPointerDeviceStruct((DevicePtr)dev, btnmap,
				TOUCHPAD_MAX_BUTTONS,
				btnlabels,
				xf86libinput_ptr_ctl,
				GetMotionHistorySize(),
				TOUCHPAD_NUM_AXES,
				axislabels);
	min = 0;
	max = TOUCH_AXIS_MAX;
	res = 0;

	xf86InitValuatorAxisStruct(dev, 0,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X),
				   min, max, res * 1000, 0, res * 1000, Absolute);
	xf86InitValuatorAxisStruct(dev, 1,
			           XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y),
				   min, max, res * 1000, 0, res * 1000, Absolute);
	InitTouchClassDeviceStruct(dev, TOUCH_MAX_SLOTS, XIDirectTouch, 2);

}

static int
xf86libinput_init(DeviceIntPtr dev)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	struct xf86libinput *driver_data = pInfo->private;
	struct libinput_device *device = driver_data->device;

	dev->public.on = FALSE;

	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD))
		xf86libinput_init_keyboard(pInfo);
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER))
		xf86libinput_init_pointer(pInfo);
	if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH))
		xf86libinput_init_touch(pInfo);

	/* unref the device now, because we'll get a new ref during
	   DEVICE_ON */
	libinput_device_unref(device);

	return 0;
}

static void
xf86libinput_destroy(DeviceIntPtr dev)
{
}

static int
xf86libinput_device_control(DeviceIntPtr dev, int mode)
{
	int rc = BadValue;

	switch(mode) {
		case DEVICE_INIT:
			rc = xf86libinput_init(dev);
			break;
		case DEVICE_ON:
			rc = xf86libinput_on(dev);
			break;
		case DEVICE_OFF:
			rc = xf86libinput_off(dev);
			break;
		case DEVICE_CLOSE:
			xf86libinput_destroy(dev);
			break;
	}

	return rc;
}

static void
xf86libinput_handle_motion(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int x, y;

	x = libinput_event_pointer_get_dx(event);
	y = libinput_event_pointer_get_dy(event);

	x = li_fixed_to_int(x);
	y = li_fixed_to_int(y);

	xf86PostMotionEvent(dev, Relative, 0, 2, x, y);
}

static void
xf86libinput_handle_button(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int button;
	int is_press;

	switch(libinput_event_pointer_get_button(event)) {
		case BTN_LEFT: button = 1; break;
		case BTN_MIDDLE: button = 2; break;
		case BTN_RIGHT: button = 3; break;
		default: /* no touchpad actually has those buttons */
			return;
	}
	is_press = (libinput_event_pointer_get_button_state(event) == LIBINPUT_POINTER_BUTTON_STATE_PRESSED);
	xf86PostButtonEvent(dev, Relative, button, is_press, 0, 0);
}

static void
xf86libinput_handle_key(InputInfoPtr pInfo, struct libinput_event_keyboard *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int is_press;
	int key = libinput_event_keyboard_get_key(event);

	key += XORG_KEYCODE_OFFSET;

	is_press = (libinput_event_keyboard_get_key_state(event) == LIBINPUT_KEYBOARD_KEY_STATE_PRESSED);
	xf86PostKeyboardEvent(dev, key, is_press);
}

static void
xf86libinput_handle_axis(InputInfoPtr pInfo, struct libinput_event_pointer *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int axis;
	li_fixed_t value;

	if (libinput_event_pointer_get_axis(event) ==
			LIBINPUT_POINTER_AXIS_VERTICAL_SCROLL)
		axis = 3;
	else
		axis = 4;

	value = libinput_event_pointer_get_axis_value(event);
	xf86PostMotionEvent(dev, Relative, axis, 1, li_fixed_to_int(value));
}

static void
xf86libinput_handle_touch(InputInfoPtr pInfo, struct libinput_event_touch *event)
{
	DeviceIntPtr dev = pInfo->dev;
	int type;
	int slot;
	ValuatorMask *m;

	/* libinput doesn't give us hw touch ids which X expects, so
	   emulate them here */
	static int next_touchid;
	static int touchids[TOUCH_MAX_SLOTS] = {0};

	slot = libinput_event_touch_get_slot(event);

	switch (libinput_event_touch_get_touch_type(event)) {
		case LIBINPUT_TOUCH_TYPE_DOWN:
			type = XI_TouchBegin;
			touchids[slot] = next_touchid++;
			break;
		case LIBINPUT_TOUCH_TYPE_UP:
			type = XI_TouchEnd;
			break;
		case LIBINPUT_TOUCH_TYPE_MOTION:
			type = XI_TouchUpdate;
			break;
		default:
			 return;
	};

	m = valuator_mask_new(2);
	valuator_mask_set_double(m, 0,
			li_fixed_to_double(libinput_event_touch_get_x(event)));
	valuator_mask_set_double(m, 1,
			li_fixed_to_double(libinput_event_touch_get_y(event)));

	xf86PostTouchEvent(dev, touchids[slot], type, 0, m);
}

static void
xf86libinput_handle_event(struct libinput_event *event)
{
	struct libinput_device *device;
	InputInfoPtr pInfo;

	device = libinput_event_get_device(event);
	pInfo = libinput_device_get_user_data(device);

	switch (libinput_event_get_type(event)) {
		case LIBINPUT_EVENT_NONE:
		case LIBINPUT_EVENT_DEVICE_ADDED:
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			/* FIXME */
			break;

		case LIBINPUT_EVENT_POINTER_MOTION:
			xf86libinput_handle_motion(pInfo,
						   libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			xf86libinput_handle_button(pInfo,
						   libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			xf86libinput_handle_key(pInfo,
						libinput_event_get_keyboard_event(event));
			break;
		case LIBINPUT_EVENT_POINTER_AXIS:
			xf86libinput_handle_axis(pInfo,
						 libinput_event_get_pointer_event(event));
			break;
		case LIBINPUT_EVENT_TOUCH_FRAME:
			break;
		case LIBINPUT_EVENT_TOUCH_TOUCH:
			xf86libinput_handle_touch(pInfo,
						  libinput_event_get_touch_event(event));
			break;
	}
}


static void
xf86libinput_read_input(InputInfoPtr pInfo)
{
	struct libinput *libinput = driver_context.libinput;
	int rc;
	struct libinput_event *event;

        rc = libinput_dispatch(libinput);
	if (rc == -EAGAIN)
		return;

	if (rc < 0) {
		ErrorFSigSafe("Error reading events: %d\n", -rc);
		return;
	}

	while ((event = libinput_get_event(libinput))) {
		xf86libinput_handle_event(event);
		libinput_event_destroy(event);
	}
}

static int
open_restricted(const char *path, int flags, void *data)
{
	int fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void
close_restricted(int fd, void *data)
{
	close(fd);
}

static void
get_screen_dimensions(struct libinput_device *device,
		      int *width, int *height, void *userdata)
{
	*width = TOUCH_AXIS_MAX;
	*height = TOUCH_AXIS_MAX;
}

const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
    .get_current_screen_dimensions = get_screen_dimensions,
};

static int xf86libinput_pre_init(InputDriverPtr drv,
				 InputInfoPtr pInfo,
				 int flags)
{
	struct xf86libinput *driver_data = NULL;
        struct libinput *libinput = NULL;
	struct libinput_device *device;
	char *path;

	driver_context.device_count++;

	pInfo->fd = -1;
	pInfo->type_name = XI_TOUCHPAD;
	pInfo->device_control = xf86libinput_device_control;
	pInfo->read_input = xf86libinput_read_input;
	pInfo->control_proc = NULL;
	pInfo->switch_mode = NULL;

	driver_data = calloc(1, sizeof(*driver_data));
	if (!driver_data)
		goto fail;

	driver_data->scroll_vdist = 1;
	driver_data->scroll_hdist = 1;

	path = xf86SetStrOption(pInfo->options, "Device", NULL);
	if (!path)
		goto fail;

	if (!driver_context.libinput)
		driver_context.libinput = libinput_path_create_context(&interface, &driver_context);
	libinput = driver_context.libinput;

	if (libinput == NULL) {
		xf86IDrvMsg(pInfo, X_ERROR, "Creating a device for %s failed\n", path);
		goto fail;
	}

	device = libinput_path_add_device(libinput, path);
	if (!device) {
		xf86IDrvMsg(pInfo, X_ERROR, "Failed to create a device for %s\n", path);
		goto fail;
	}

	/* We ref the device but remove it afterwards. The hope is that
	   between now and DEVICE_INIT/DEVICE_ON, the device doesn't change.
	  */
	libinput_device_ref(device);
	libinput_path_remove_device(device);

	pInfo->private = driver_data;
	driver_data->path = path;
	driver_data->device = device;

	return Success;

fail:
	free(path);
	free(driver_data);
	return BadValue;
}

static void
xf86libinput_uninit(InputDriverPtr drv,
		    InputInfoPtr pInfo,
		    int flags)
{
	struct xf86libinput *driver_data = pInfo->private;
	if (driver_data) {
		if (--driver_context.device_count == 0) {
			libinput_destroy(driver_context.libinput);
			driver_context.libinput = NULL;
		}
		free(driver_data->path);
		free(driver_data);
		pInfo->private = NULL;
	}
}


InputDriverRec xf86libinput_driver = {
	.driverVersion	= 1,
	.driverName	= "libinput",
	.PreInit	= xf86libinput_pre_init,
	.UnInit		= xf86libinput_uninit,
};

static XF86ModuleVersionInfo xf86libinput_version_info = {
	"libinput",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_XINPUT,
	ABI_XINPUT_VERSION,
	MOD_CLASS_XINPUT,
	{0, 0, 0, 0}
};

static pointer
xf86libinput_setup_proc(pointer module, pointer options, int *errmaj, int *errmin)
{
	xf86AddInputDriver(&xf86libinput_driver, module, 0);
	return module;
}

_X_EXPORT XF86ModuleData libinputModuleData = {
	.vers		= &xf86libinput_version_info,
	.setup		= &xf86libinput_setup_proc,
	.teardown	= NULL
};


