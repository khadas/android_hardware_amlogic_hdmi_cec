/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 *     Amlogic HDMITX CEC HAL
 *       Copyright (C) 2014
 *
 * This implements a hdmi cec hardware library for the Android emulator.
 * the following code should be built as a shared library that will be
 * placed into /system/lib/hw/hdmi_cec.so
 *
 * It will be loaded by the code in hardware/libhardware/hardware.c
 * which is itself called from
 * frameworks/base/services/core/jni/com_android_server_hdmi_HdmiCecController.cpp
 */

#include <cutils/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <hardware/hdmi_cec.h>
#include <hardware/hardware.h>
#include <cutils/properties.h>
#include "hdmi_cec.h"
#include <jni.h>
#include <JNIHelp.h>

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "CEC"
#else
#define LOG_TAG "CEC"
#endif

/* Set to 1 to enable debug messages to the log */
#define DEBUG 1
#if DEBUG
# define D(format, args...) ALOGD("[%s]"format, __func__, ##args)
#else
# define D(...) do{}while(0)
#endif

#define  E(format, args...) ALOGE("[%s]"format, __func__, ##args)

#define CEC_FILE        "/dev/cec"
#define MAX_PORT        32

#define MESSAGE_SET_MENU_LANGUAGE 0x32

/*
 * structures for platform cec implement
 * @device_type : indentify type of cec device, such as tv or mbox
 * @run         : run flag for rx poll thread
 * @exit        : if rx poll thread is exited
 * @addr_bitmap : bit maps for each valid logical address
 * @fd          : file descriptor for global read/write
 * @ThreadId    : pthread for poll cec rx message
 * @cb_data     : data pointer for cec message RX call back
 * @cb          : event call back for cec message RX
 * @dev         : for hdmi_cec_device type
 * @port_data   : array for port data
 * @onCecMessageRx   : for JNI call if got message
 * @onAddAddress     : for event to extend process
 * @env              : saved JNI enverioment
 * @javavm           : java vm for jni
 */
struct aml_cec_hal {
    int                         device_type;
    int                         run;
    int                         exited;
    int                         addr_bitmap;
    int                         fd;
    int                         total_port;
    int                         ext_control;
    int                         flag;
    unsigned int                con_status;
    pthread_t                   ThreadId;
    void                       *cb_data;
    event_callback_t            cb;
    struct hdmi_cec_device     *dev;
    struct hdmi_port_info      *port_data;
    jmethodID                   onCecMessageRx;
    jmethodID                   onAddAddress;
    jobject                     obj;
    JavaVM                     *javavm;
};

struct aml_cec_hal *hal_info = NULL;

static int cec_rx_read_msg(unsigned char *buf, int msg_cnt)
{
    int i;
    char *path = CEC_FILE;

    if (msg_cnt <= 0 || !buf) {
        return 0;
    }
    /* maybe blocked at driver */
    i = read(hal_info->fd, buf, msg_cnt);
    if (i < 0) {
        E("read :%s failed, ret:%d\n", path, i);
        return -1;
    }
    return i;
}

static void check_connect_status(struct aml_cec_hal *hal)
{
    unsigned int prev_status, bit;
    int i, port, ret;
    hdmi_event_t event;

    prev_status = hal->con_status;
    for (i = 0; i < hal->total_port && hal->port_data != NULL; i++) {
        port = hal->port_data[i].port_id;
        ret = ioctl(hal_info->fd, CEC_IOC_GET_CONNECT_STATUS, &port);
        if (ret) {
            D("get port %d connected status failed, ret:%d\n", hal->port_data[i].port_id, ret);
            continue;
        }
        bit = prev_status & (1 << i);
        if (bit ^ ((port ? 1 : 0) << i)) {
            D("port:%d, connect status changed, now:%d, prev_status:%x\n",
              hal->port_data[i].port_id, port, prev_status);
            event.type = HDMI_EVENT_HOT_PLUG;
            event.dev = hal->dev;
            event.hotplug.connected = port;
            event.hotplug.port_id = hal->port_data[i].port_id;
            if (hal->cb && (hal->flag & (1 << HDMI_OPTION_SYSTEM_CEC_CONTROL))) {
                hal->cb(&event, hal_info->cb_data);
            }
            prev_status &= ~(bit);
            prev_status |= ((port ? 1 : 0) << i);
            D("now mask:%x\n", prev_status);
        }
    }
    hal->con_status = prev_status;
}

static void *cec_rx_loop(void *data)
{
    struct aml_cec_hal *hal = (struct aml_cec_hal *)data;
    hdmi_event_t event;
    unsigned char msg_buf[CEC_MESSAGE_BODY_MAX_LENGTH];
    int r;
#if DEBUG
    char buf[64] = {};
    int size = 0, i;
#endif
    JNIEnv *env;
    JavaVM *Vm;
    int ret;

    D("start\n");
    while (hal_info->fd < 0) {
        usleep(1000 * 1000);
        hal_info->fd = open(CEC_FILE, O_RDWR);
    }
    D("file open ok\n");
    while (hal && hal->run) {
        check_connect_status(hal);
        memset(&event, 0, sizeof(event));
        memset(msg_buf, 0, sizeof(msg_buf));

        /* try to got a message from dev */
        r = cec_rx_read_msg(msg_buf, CEC_MESSAGE_BODY_MAX_LENGTH);
        if (r <= 1) { /* ignore received ping messages */
            continue;
        }
    #if DEBUG
        size = 0;
        memset(buf, 0, sizeof(buf));
        for (i = 0; i < r; i++) {
            size += sprintf(buf + size, "%02x ", msg_buf[i]);
        }
        D("msg:%s", buf);
    #endif
        memcpy(event.cec.body, msg_buf + 1, r - 1);
        event.type = HDMI_EVENT_CEC_MESSAGE;
        event.dev = hal->dev;
        event.cec.initiator   = (msg_buf[0] >> 4) & 0xf;
        event.cec.destination = (msg_buf[0] >> 0) & 0xf;
        event.cec.length      = r - 1;
        if (hal->device_type == DEV_TYPE_PLAYBACK &&
            msg_buf[1] == 0x32 &&
            hal->ext_control) {
            D("ignore menu language change for tx\n");
        } else {
            if (hal->cb && (hal->flag & (1 << HDMI_OPTION_SYSTEM_CEC_CONTROL))) {
                hal->cb(&event, hal_info->cb_data);
            }
        }
        /* call java method to process cec message for ext control */
        if ((hal->ext_control == 0x03) &&
            (hal->device_type == DEV_TYPE_PLAYBACK) &&
            (hal->flag & (1 << HDMI_OPTION_SYSTEM_CEC_CONTROL)) &&
            (hal->javavm)) {
            Vm = hal->javavm;
            ret = (*Vm)->GetEnv(Vm, (void**)&env, JNI_VERSION_1_4);
            if (ret < 0) {
                ret = (*Vm)->AttachCurrentThread(Vm, &env, NULL);
                if (ret < 0) {
                    D("can't attach Vm");
                    continue;
                }
            }
            jbyteArray array = (*env)->NewByteArray(env, r);
            (*env)->SetByteArrayRegion(env, array, 0, r, msg_buf);
            (*env)->CallVoidMethod(env, hal->obj, hal->onCecMessageRx, array);
            (*env)->DeleteLocalRef(env, array);
        }
    }
    D("end\n");
    hal->exited = 1;
    return 0;
}

/*
 * (*add_logical_address)() passes the logical address that will be used
 * in this system.
 *
 * HAL may use it to configure the hardware so that the CEC commands addressed
 * the given logical address can be filtered in. This method can be called
 * as many times as necessary in order to support multiple logical devices.
 * addr should be in the range of valid logical addresses for the call
 * to succeed.
 *
 * Returns 0 on success or -errno on error.
 */
static int cec_add_logical_address(const struct hdmi_cec_device* dev, cec_logical_address_t addr)
{
    JNIEnv *env;
    JavaVM *Vm;
    int ret;

    if (!hal_info || hal_info->fd < 0)
        return -EINVAL;
    if (addr < CEC_ADDR_BROADCAST)
        hal_info->addr_bitmap |= (1 << addr);

    if (hal_info->ext_control) {
        hal_info->ext_control |= (0x02);
        if ((hal_info->device_type == DEV_TYPE_PLAYBACK) &&
            (hal_info->obj != NULL)) {
            Vm = hal_info->javavm;
            ret = (*Vm)->GetEnv(Vm, (void**)&env, JNI_VERSION_1_4);
            if (ret < 0) {
                ret = (*Vm)->AttachCurrentThread(Vm, &env, NULL);
                if (ret < 0) {
                    D("can't attach Vm");
                }
            } else if (hal_info->onAddAddress) {
                (*env)->CallVoidMethod(env, hal_info->obj, hal_info->onAddAddress, addr);
            }
        }
    }
    D("dev:%p, addr:%x, bitmap:%x\n", dev, addr, hal_info->addr_bitmap);
    return ioctl(hal_info->fd, CEC_IOC_ADD_LOGICAL_ADDR, addr);
}

/*
 * (*clear_logical_address)() tells HAL to reset all the logical addresses.
 *
 * It is used when the system doesn't need to process CEC command any more,
 * hence to tell HAL to stop receiving commands from the CEC bus, and change
 * the state back to the beginning.
 */
static void cec_clear_logical_address(const struct hdmi_cec_device* dev)
{
    if (!hal_info || hal_info->fd < 0)
        return ;
    hal_info->addr_bitmap = (1 << CEC_ADDR_BROADCAST);
    D("dev:%p, bitmap:%x\n", dev, hal_info->addr_bitmap);
    if (hal_info->ext_control) {
        hal_info->ext_control &= ~(0x02);
    }
    ioctl(hal_info->fd, CEC_IOC_CLR_LOGICAL_ADDR, 0);
}

/*
 * (*get_physical_address)() returns the CEC physical address. The
 * address is written to addr.
 *
 * The physical address depends on the topology of the network formed
 * by connected HDMI devices. It is therefore likely to change if the cable
 * is plugged off and on again. It is advised to call get_physical_address
 * to get the updated address when hot plug event takes place.
 *
 * Returns 0 on success or -errno on error.
 */
static int cec_get_physical_address(const struct hdmi_cec_device* dev, uint16_t* addr)
{
    int ret;
    if (!hal_info || hal_info->fd < 0)
        return -EINVAL;
    ret = ioctl(hal_info->fd, CEC_IOC_GET_PHYSICAL_ADDR, addr);
    D("dev:%p, physical addr:%x, ret:%d\n", dev, *addr, ret);
    return ret;
}

static char *get_send_result(int r)
{
    switch (r) {
    case HDMI_RESULT_SUCCESS:
        return "success";
    case HDMI_RESULT_NACK:
        return "no ack";
    case HDMI_RESULT_BUSY:
        return "busy";
    case HDMI_RESULT_FAIL:
        return "fail other";
    default:
        return "unknown fail code";
    }
}

/*
 * (*send_message)() transmits HDMI-CEC message to other HDMI device.
 *
 * The method should be designed to return in a certain amount of time not
 * hanging forever, which can happen if CEC signal line is pulled low for
 * some reason. HAL implementation should take the situation into account
 * so as not to wait forever for the message to get sent out.
 *
 * It should try retransmission at least once as specified in the standard.
 *
 * Returns error code. See HDMI_RESULT_SUCCESS, HDMI_RESULT_NACK, and
 * HDMI_RESULT_BUSY.
 */
static int cec_send_message(const struct hdmi_cec_device* dev, const cec_message_t* msg)
{
    int i, ret;
    unsigned char msg_buf[CEC_MESSAGE_BODY_MAX_LENGTH] = {};
#if DEBUG
    char buf[64] = {};
    int size = 0;
#endif

    if (!hal_info || hal_info->fd < 0)
        return HDMI_RESULT_FAIL;

    /* don't send message if controled by extend */
    if (hal_info->ext_control == 0x03 && dev) {
        return HDMI_RESULT_SUCCESS;
    }

    memset(msg_buf, 0, sizeof(msg_buf));
    msg_buf[0] = ((msg->initiator & 0xf) << 4) | (msg->destination & 0xf);
    memcpy(msg_buf + 1, msg->body, msg->length);
    ret = write(hal_info->fd, msg_buf, msg->length + 1);
#if DEBUG
    memset(buf, 0, sizeof(buf));
    for (i = 0; i < (int)msg->length; i++) {
        size += sprintf(buf + size, "%02x ", msg->body[i]);
    }
    D("[%x -> %x]len:%d, body:%s, result:%s\n",
      msg->initiator, msg->destination, msg->length,
      buf, get_send_result(ret));
#endif
    return ret;
}

/*
 * export cec send message API for other usage
 */
int cec_send_message_ext(int dest, int len, unsigned char *buffer)
{
    int addr;
    int i;
    cec_message_t msg;

    if (!hal_info)
        return -1;

    if (hal_info->device_type == DEV_TYPE_PLAYBACK) {
        addr = hal_info->addr_bitmap;
        addr &= 0x7fff;
        for (i = 0; i < 15; i++) {
            if (addr & 1) {
                break;
            }
            addr >>= 1;
        }
        msg.initiator = i;
    } else {
        msg.initiator = 0;  /* root for TV */
    }
    msg.destination = dest;
    msg.length = len;
    memcpy(msg.body, buffer, len);
    return cec_send_message(NULL, &msg);
}

/*
 * (*register_event_callback)() registers a callback that HDMI-CEC HAL
 * can later use for incoming CEC messages or internal HDMI events.
 * When calling from C++, use the argument arg to pass the calling object.
 * It will be passed back when the callback is invoked so that the context
 * can be retrieved.
 */
static void cec_register_event_callback(const struct hdmi_cec_device* dev,
        event_callback_t callback, void* arg)
{
    if (!hal_info || hal_info->fd < 0)
        return ;
    D("dev:%p, callback:%p, arg:%p\n", callback, arg, dev);
    hal_info->cb      = callback;
    hal_info->cb_data = arg;
}

/*
 * (*get_version)() returns the CEC version supported by underlying hardware.
 */
static void cec_get_version(const struct hdmi_cec_device* dev, int* version)
{
    if (!hal_info || hal_info->fd < 0)
        return ;
    ioctl(hal_info->fd, CEC_IOC_GET_VERSION, version);
    D("dev:%p, version:%x\n", dev, *version);
}

/*
 * (*get_vendor_id)() returns the identifier of the vendor. It is
 * the 24-bit unique company ID obtained from the IEEE Registration
 * Authority Committee (RAC).
 */
static void cec_get_vendor_id(const struct hdmi_cec_device* dev, uint32_t* vendor_id)
{
    if (!hal_info || hal_info->fd < 0)
        return ;
    ioctl(hal_info->fd, CEC_IOC_GET_VENDOR_ID, vendor_id);
    D("dev:%p, vendor_id:%x\n", dev, *vendor_id);
}

/*
 * (*get_port_info)() returns the hdmi port information of underlying hardware.
 * info is the list of HDMI port information, and 'total' is the number of
 * HDMI ports in the system.
 */
static void cec_get_port_info(const struct hdmi_cec_device* dev,
        struct hdmi_port_info* list[], int* total)
{
    int i;

    if (!hal_info || hal_info->fd < 0)
        return ;

    ioctl(hal_info->fd, CEC_IOC_GET_PORT_NUM, total);
    D("dev:%p, total port:%d\n", dev, *total);
    if (*total > MAX_PORT)
        *total = MAX_PORT;
    hal_info->port_data = malloc(sizeof(struct hdmi_port_info) * (*total));
    if (!hal_info->port_data) {
        E("alloc port_data failed\n");
        *total = 0;
        return ;
    }
    ioctl(hal_info->fd, CEC_IOC_GET_PORT_INFO, hal_info->port_data);
    for (i = 0; i < *total; i++) {
        D("port %d, type:%s, id:%d, cec support:%d, arc support:%d, physical address:%x\n",
          i, hal_info->port_data[i].type ? "output" : "input",
          hal_info->port_data[i].port_id,
          hal_info->port_data[i].cec_supported,
          hal_info->port_data[i].arc_supported,
          hal_info->port_data[i].physical_address);
    }
    *list = hal_info->port_data;
    hal_info->total_port = *total;
}

/*
 * (*set_option)() passes flags controlling the way HDMI-CEC service works down
 * to HAL implementation. Those flags will be used in case the feature needs
 * update in HAL itself, firmware or microcontroller.
 */
static void cec_set_option(const struct hdmi_cec_device* dev, int flag, int value)
{
    int ret;

    if (!hal_info || hal_info->fd < 0)
        return ;
    switch (flag) {
    case HDMI_OPTION_ENABLE_CEC:
        ret = ioctl(hal_info->fd, CEC_IOC_SET_OPTION_ENALBE_CEC, value);
        break;

    case HDMI_OPTION_WAKEUP:
        ret = ioctl(hal_info->fd, CEC_IOC_SET_OPTION_WAKEUP, value);
        break;

    case HDMI_OPTION_SYSTEM_CEC_CONTROL:
        ret = ioctl(hal_info->fd, CEC_IOC_SET_OPTION_SYS_CTRL, value);
        if (value)
            hal_info->flag |= (1 << HDMI_OPTION_SYSTEM_CEC_CONTROL);
        else
            hal_info->flag &= ~(1 << HDMI_OPTION_SYSTEM_CEC_CONTROL);
        break;

    /* set device auto-power off by uboot */
    case HDMI_OPTION_CEC_AUTO_DEVICE_OFF:
        ret = ioctl(hal_info->fd, CEC_IOC_SET_AUTO_DEVICE_OFF, value);
        break;

    case HDMI_OPTION_SET_LANG:
        ret = ioctl(hal_info->fd, CEC_IOC_SET_OPTION_SET_LANG, value);
        break;

    default:
        break;
    }
    D("dev:%p, flag:%x, value:%x, ret:%d, hal_flag:%x\n",
      dev, flag, value, ret, hal_info->flag);
}

/*
 * get connect status when boot
 */
static void get_boot_connect_status(struct aml_cec_hal *hal_info)
{
    unsigned int total;
    int i, port, ret;

    ret = ioctl(hal_info->fd, CEC_IOC_GET_PORT_NUM, &total);
    D("total port:%d, ret:%d\n", total, ret);
    if (ret < 0)
        return ;

    if (total > MAX_PORT)
        total = MAX_PORT;
    hal_info->con_status = 0;
    for (i = 0; i < total; i++) {
        port = i + 1;
        ret = ioctl(hal_info->fd, CEC_IOC_GET_CONNECT_STATUS, &port);
        if (ret) {
            D("get port %d connected status failed, ret:%d\n", i, ret);
            continue;
        }
        hal_info->con_status |= ((port ? 1 : 0) << i);
    }
    D("con_status:%x\n", hal_info->con_status);
}

/*
 * (*set_audio_return_channel)() configures ARC circuit in the hardware logic
 * to start or stop the feature. Flag can be either 1 to start the feature
 * or 0 to stop it.
 *
 * Returns 0 on success or -errno on error.
 */
static void cec_set_audio_return_channel(const struct hdmi_cec_device* dev, int port_id, int flag)
{
    int ret;
    if (!hal_info || hal_info->fd < 0)
        return ;
    ret = ioctl(hal_info->fd, CEC_IOC_SET_ARC_ENABLE, flag);
    D("dev:%p, port id:%d, flag:%x, ret:%d\n", dev, port_id, flag, ret);
}

/*
 * (*is_connected)() returns the connection status of the specified port.
 * Returns HDMI_CONNECTED if a device is connected, otherwise HDMI_NOT_CONNECTED.
 * The HAL should watch for +5V power signal to determine the status.
 */
static int cec_is_connected(const struct hdmi_cec_device* dev, int port_id)
{
    int status = -1, ret;

    if (!hal_info || hal_info->fd < 0)
        return HDMI_NOT_CONNECTED;

    /* use status pass port id */
    status = port_id;
    ret = ioctl(hal_info->fd, CEC_IOC_GET_CONNECT_STATUS, &status);
    if (ret)
        return HDMI_NOT_CONNECTED;
    D("dev:%p, port:%d, connected:%s\n", dev, port_id, status ? "yes" : "no");
    return (status) ? HDMI_CONNECTED : HDMI_NOT_CONNECTED;
}

/** Close the hdmi cec device */
static int cec_close(struct hw_device_t *dev)
{
    if (!hal_info)
        return -EINVAL;

    hal_info->run = 0;
    while (!hal_info->exited) {
        usleep(100 * 1000);
    }
    free(dev);
    close(hal_info->fd);
    free(hal_info->port_data);
    free(hal_info);
    D("closed ok\n");
    return 0;
}

/**
 * module methods
 */
static int open_cec( const struct hw_module_t* module, char const *name,
        struct hw_device_t **device )
{
    char value[PROPERTY_VALUE_MAX] = {};
    int dev_type;
    int ret;

    hal_info = malloc(sizeof(*hal_info));
    if (!hal_info) {
        D("%s, alloc memory failed\n", __func__);
        return -EINVAL;
    }
    memset(hal_info, 0, sizeof(*hal_info));
    if (strcmp(name, HDMI_CEC_HARDWARE_INTERFACE) != 0) {
        D("cec strcmp fail !!!");
        return -EINVAL;
    }
    if (device == NULL) {
        D("NULL cec device on open");
        return -EINVAL;
    }
    property_get("ro.hdmi.device_type", value, "4");
    D("ro.hdmi.device_type:%s\n", value);
    ret = sscanf(value, "%d", &dev_type);
    if (ret == 1 &&
       (dev_type >= DEV_TYPE_TV && dev_type <= DEV_TYPE_VIDEO_PROCESSOR)) {
        hal_info->device_type = dev_type;
    } else {
        hal_info->device_type = DEV_TYPE_PLAYBACK;
    }
    memset(value, 0, sizeof(value));
    property_get("persist.sys.hdmi.keep_awake", value, "true");
    if (!strcmp(value, "false")) {
        hal_info->ext_control = 1;
    } else {
        hal_info->ext_control = 0;
    }
    D("ext control:%d, %s", hal_info->ext_control, value);

    hdmi_cec_device_t *dev = malloc(sizeof(hdmi_cec_device_t));
    memset(dev, 0, sizeof(hdmi_cec_device_t));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*) module;
    dev->common.close = cec_close;

    dev->add_logical_address      = cec_add_logical_address;
    dev->clear_logical_address    = cec_clear_logical_address;
    dev->get_physical_address     = cec_get_physical_address;
    dev->send_message             = cec_send_message;
    dev->register_event_callback  = cec_register_event_callback;
    dev->get_version              = cec_get_version;
    dev->get_vendor_id            = cec_get_vendor_id;
    dev->get_port_info            = cec_get_port_info;
    dev->set_option               = cec_set_option;
    dev->set_audio_return_channel = cec_set_audio_return_channel;
    dev->is_connected             = cec_is_connected;

    *device = (hw_device_t*) dev;

    hal_info->run         = 1;
    hal_info->exited      = 0;
    hal_info->ThreadId    = 0;
    hal_info->dev         = dev;
    hal_info->total_port  = 0;
    hal_info->con_status  = 0;
    hal_info->port_data   = NULL;
    hal_info->addr_bitmap = (1 << CEC_ADDR_BROADCAST);
    hal_info->cb_data     = NULL;
    hal_info->cb          = NULL;
    hal_info->fd          = open(CEC_FILE, O_RDWR);
    hal_info->javavm      = NULL;
    hal_info->obj         = NULL;
    hal_info->flag        = 0;
    if (hal_info->fd < 0) {
        E("can't open %s\n", CEC_FILE);
        return -EINVAL;
    }
    ret = ioctl(hal_info->fd, CEC_IOC_SET_DEV_TYPE, hal_info->device_type);
    get_boot_connect_status(hal_info);
    pthread_create(&hal_info->ThreadId, NULL, cec_rx_loop, hal_info);
    pthread_setname_np(hal_info->ThreadId, "cec_rx_loop");

    return 0;
}

static struct hw_module_methods_t hdmi_cec_module_methods = {
    .open =  open_cec,
};

/*
 * The hdmi cec Module
 */
struct hdmi_cec_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                = HARDWARE_MODULE_TAG,
        .module_api_version = HDMI_CEC_MODULE_API_VERSION_1_0,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id                 = HDMI_CEC_HARDWARE_MODULE_ID,
        .name               = "Amlogic hdmi cec Module",
        .author             = "Amlogic Corp.",
        .methods            = &hdmi_cec_module_methods,
    },
};

JNIEXPORT int JNICALL
Java_com_droidlogic_app_HdmiCecExtend_nativeSendCecMessage(JNIEnv *env, jobject thiz, jint dest, jbyteArray body)
{
    jbyte *ba = (*env)->GetByteArrayElements(env, body, JNI_FALSE);
    jsize len = (*env)->GetArrayLength(env, body);

    return cec_send_message_ext(dest, len, ba);
}

JNIEXPORT void JNICALL
Java_com_droidlogic_app_HdmiCecExtend_nativeInit(JNIEnv *env, jobject thiz, jobject obj)
{
    jobject obj1;
    if (hal_info != NULL) {
        obj1 = (*env)->NewGlobalRef(env, obj);
        hal_info->obj = obj1;
    }
}

JNIEXPORT jint JNICALL
Java_com_droidlogic_app_HdmiCecExtend_nativeGetPhysicalAddr(JNIEnv *env, jobject thiz)
{
    unsigned short addr = -1;
    if (hal_info && cec_get_physical_address(hal_info->dev, &addr) < 0) {
        return -1;
    }

    return addr;
}

JNIEXPORT jint JNICALL
Java_com_droidlogic_app_HdmiCecExtend_nativeGetVendorId(JNIEnv *env, jobject thiz)
{
    unsigned int id = 0;

    if (hal_info)
        cec_get_vendor_id(hal_info->dev, &id);
    return id;
}

JNIEXPORT jint JNICALL
Java_com_droidlogic_app_HdmiCecExtend_nativeGetCecVersion(JNIEnv *env, jobject thiz)
{
    int version = 0;

    cec_get_version(hal_info->dev, &version);
    return version;
}

static JNINativeMethod hdmiExtend_method[] = {
    {"nativeSendCecMessage", "(I[B)I", (void *)Java_com_droidlogic_app_HdmiCecExtend_nativeSendCecMessage},
    {"nativeInit", "(Lcom/droidlogic/HdmiCecExtend;)V", (void *)Java_com_droidlogic_app_HdmiCecExtend_nativeInit},
    {"nativeGetPhysicalAddr", "()I", (void *)Java_com_droidlogic_app_HdmiCecExtend_nativeGetPhysicalAddr},
    {"nativeGetVendorId", "()I", (void *)Java_com_droidlogic_app_HdmiCecExtend_nativeGetVendorId},
    {"nativeGetCecVersion", "()I", (void *)Java_com_droidlogic_app_HdmiCecExtend_nativeGetCecVersion},
};

static int registerNativeMethods(JNIEnv* env, const char* className,
        const JNINativeMethod* methods, int numMethods)
{
    int rc;
    jclass clazz;
    clazz = (*env)->FindClass(env, className);

    if (clazz == NULL) {
        D("NULL clazz\n");
        return -1;
    }

    if ((rc = ((*env)->RegisterNatives(env, clazz, methods, numMethods))) < 0) {
        return -1;
    }

    if (hal_info != NULL) {
        hal_info->onCecMessageRx = (*env)->GetMethodID(env, clazz, "onCecMessageRx", "([B)V");
        if (hal_info->onCecMessageRx == NULL) {
            D("can't found method onCecMessageRx");
        } else {
            D("got method onCecMessageRx, env:%p", env);
        }
        hal_info->onAddAddress = (*env)->GetMethodID(env, clazz, "onAddAddress", "(I)V");
        if (hal_info->onAddAddress == NULL) {
            D("can't found method onAddAddress");
        } else {
            D("got method onAddAddress, env:%p", env);
        }
    }

    return 0;
}


JNIEXPORT jint
JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;
    jint result = -1;

    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
        D("GetEnv failed!\n");
        return result;
    }
    if (registerNativeMethods(env,
                              "com/droidlogic/HdmiCecExtend",
                              hdmiExtend_method,
                              NELEM(hdmiExtend_method)) < 0) {
        D("registerNativeMethods failed\n");
    }
    if (hal_info) {
        hal_info->javavm = vm;
    }

    return JNI_VERSION_1_4;
}

