/* based on gui-agent/vmside.c */

#include <stdint.h>
#include <xen/io/fbif.h>
#include <xen/io/kbdif.h>
#include <semaphore.h>
#include <sched.h>
#include <hw/hw.h>
#include <hw/pc.h>
#include <console.h>

#include <mm.h>
#include <hw/xenfb.h>
#include <fbfront.h>
#include <sysemu.h>

#include <qubes-gui-qemu.h>
#include <qubes-gui-protocol.h>
#include <libvchan.h>
#include <u2mfnlib.h>
#include <txrx.h>

#define QUBES_GUI_PROTOCOL_VERSION_STUBDOM (1 << 16 | 0)

struct QubesGuiState *qs;
libvchan_t *vchan;

#define min(x,y) ((x)>(y)?(y):(x))
#define QUBES_MAIN_WINDOW 1

static void *vga_vram;

static DisplayChangeListener *dcl;

extern uint32_t vga_ram_size;

void process_pv_update(QubesGuiState * qs,
                       int x, int y, int width, int height)
{
    struct msg_shmimage mx;
    struct msg_hdr hdr;

    hdr.type = MSG_SHMIMAGE;
    hdr.window = QUBES_MAIN_WINDOW;
    mx.x = x;
    mx.y = y;
    mx.width = width;
    mx.height = height;
    write_message(vchan, hdr, mx);
}


void qubes_create_window(QubesGuiState * qs, int w, int h)
{
    struct msg_hdr hdr;
    struct msg_create crt;
    int ret;

    // the following hopefully avoids missed damage events
    hdr.type = MSG_CREATE;
    hdr.window = QUBES_MAIN_WINDOW;
    crt.width = w;
    crt.height = h;
    crt.parent = 0;
    crt.x = 0;
    crt.y = 0;
    crt.override_redirect = 0;
    write_message(vchan, hdr, crt);
}

void send_pixmap_mfns(QubesGuiState * qs)
{
    struct shm_cmd shmcmd;
    struct msg_hdr hdr;
    uint32_t *mfns;
    int n;
    int i;
    void *data;
    void *data_aligned;
    int offset, copy_offset;

    data = surface_data(qs->surface);

    offset = (long) data & (XC_PAGE_SIZE - 1);
    data_aligned = (void *) ((long) data & ~(XC_PAGE_SIZE - 1));

    /* XXX: hardcoded 4 bytes per pixel - gui-daemon doesn't handle other bpp */
    n = (4 * surface_width(qs->surface) * surface_height(qs->surface) +
         offset + (XC_PAGE_SIZE-1)) / XC_PAGE_SIZE;
    if (mlock(data_aligned, n * XC_PAGE_SIZE) == -1) {
        perror("mlock failed");
        return;
    }
    mfns = g_new(uint32_t, n);
    if (!mfns) {
        fprintf(stderr,
                "Cannot allocate mfns array, %lu bytes needed\n",
                n * sizeof(*mfns));
        return;
    }
    fprintf(stderr,
            "dumping mfns: n=%d, w=%d, h=%d\n",
            n, surface_width(qs->surface), surface_height(qs->surface));
    for (i = 0; i < n; i++)
        u2mfn_get_mfn_for_page_with_fd(qs->u2mfn_fd,
                (long) data_aligned + i * XC_PAGE_SIZE,
                (int *) &mfns[i]);
    hdr.type = MSG_MFNDUMP;
    hdr.window = QUBES_MAIN_WINDOW;
    hdr.untrusted_len = sizeof(shmcmd) + n * sizeof(*mfns);
    shmcmd.width = surface_width(qs->surface);
    shmcmd.height = surface_height(qs->surface);
    shmcmd.num_mfn = n;
    shmcmd.off = offset;
    shmcmd.bpp = 24;
    write_struct(vchan, hdr);
    write_struct(vchan, shmcmd);
    write_data(vchan, (char *) mfns, n * sizeof(*mfns));
    g_free(mfns);
}

void send_wmname(QubesGuiState * qs, const char *wmname)
{
    struct msg_hdr hdr;
    struct msg_wmname msg;
    strncpy(msg.data, wmname, sizeof(msg.data));
    hdr.window = QUBES_MAIN_WINDOW;
    hdr.type = MSG_WMNAME;
    write_message(vchan, hdr, msg);
}

void send_wmhints(QubesGuiState * qs)
{
    struct msg_hdr hdr;
    struct msg_window_hints msg;

    // pass only some hints
    msg.flags = (PMinSize | PMaxSize);
    msg.min_width = surface_width(qs->surface);
    msg.min_height = surface_height(qs->surface);
    msg.max_width = surface_width(qs->surface);
    msg.max_height = surface_height(qs->surface);
    hdr.window = QUBES_MAIN_WINDOW;
    hdr.type = MSG_WINDOW_HINTS;
    write_message(vchan, hdr, msg);
}

void send_map(QubesGuiState * qs)
{
    struct msg_hdr hdr;
    struct msg_map_info map_info;

    map_info.override_redirect = 0;
    map_info.transient_for = 0;
    hdr.type = MSG_MAP;
    hdr.window = QUBES_MAIN_WINDOW;
    write_message(vchan, hdr, map_info);
}

void process_pv_resize(QubesGuiState * qs)
{
    if (!qs->surface) {
        return;
    }

    struct msg_hdr hdr;
    struct msg_configure conf;
    hdr.type = MSG_CONFIGURE;
    hdr.window = QUBES_MAIN_WINDOW;
    conf.x = qs->x;
    conf.y = qs->y;
    conf.width = surface_width(qs->surface);
    conf.height = surface_height(qs->surface);
    conf.override_redirect = 0;
    if (qs->log_level > 1)
        fprintf(stderr,
                "handle resize  w=%d h=%d\n", conf.width, conf.height);
    write_message(vchan, hdr, conf);
    send_pixmap_mfns(qs);
    send_wmhints(qs);
}

void handle_configure(QubesGuiState * qs)
{
    struct msg_configure r;
    read_data(vchan, (char *) &r, sizeof(r));
    fprintf(stderr,
            "configure msg, x/y %d %d (was %d %d), w/h %d %d\n",
            r.x, r.y, qs->x, qs->y, r.width, r.height);

    qs->x = r.x;
    qs->y = r.y;
}

int is_bitset(unsigned char *keys, int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

void setbit(unsigned char *keys, int num, int value)
{
    if (value)
        keys[num / 8] |= 1 << (num % 8);
    else
        keys[num / 8] &= ~(1 << (num % 8));
}

void send_keycode(QubesGuiState * qs, int keycode, int release)
{
    uint32_t scancode = qubes_keycode2scancode[keycode];

    setbit(qs->local_keys, keycode, !release);

    if (qs->log_level > 1)
        fprintf(stderr,
                "Received keycode %d(0x%x), converted to %d(0x%x)\n",
                keycode, keycode, scancode, scancode);
    if (!scancode) {
        fprintf(stderr, "Can't convert keycode %x to scancode\n",
                keycode);
        return;
    }
    if (release && (scancode & 0x80))
        // if scancode already have 0x80 bit set do not output key release
        return;
    if (scancode & 0xff000000)
        kbd_put_keycode((scancode & 0xff000000) >> 24);
    if (scancode & 0xff0000)
        kbd_put_keycode((scancode & 0xff0000) >> 16);
    if (scancode & 0xff00)
        kbd_put_keycode((scancode & 0xff00) >> 8);
    if (release)
        scancode |= 0x80;
    kbd_put_keycode(scancode & 0xff);
}

void sync_kbd_state(QubesGuiState * qs, int kbd_state) {
    int qemu_state = kbd_get_leds_state();

    if ( (!!(qemu_state & KDB_LED_CAPS_LOCK)) ^ (!!(kbd_state & LockMask))) {
        send_keycode(qs, 66, 0);
        send_keycode(qs, 66, 1);
    }
    if ( (!!(qemu_state & KDB_LED_NUM_LOCK)) ^ (!!(kbd_state & Mod2Mask))) {
        send_keycode(qs, 77, 0);
        send_keycode(qs, 77, 1);
    }
}

void handle_keypress(QubesGuiState * qs)
{
    struct msg_keypress key;
    uint32_t scancode;

    read_data(vchan, (char *) &key, sizeof(key));

    if (key.keycode != 66 && key.keycode != 77)
        sync_kbd_state(qs, key.state);
    send_keycode(qs, key.keycode, key.type != KeyPress);
}

void handle_button(QubesGuiState * qs)
{
    struct msg_button key;
    int button = 0;
    int z = 0;

    read_data(vchan, (char *) &key, sizeof(key));
    if (qs->log_level > 1)
        fprintf(stderr,
                "send buttonevent, type=%d button=%d\n",
                (int) key.type, key.button);

    if (key.button == Button1)
        button = MOUSE_EVENT_LBUTTON;
    else if (key.button == Button3)
        button = MOUSE_EVENT_RBUTTON;
    else if (key.button == Button2)
        button = MOUSE_EVENT_MBUTTON;
    else if (key.button == Button4)
        z = -1;
    else if (key.button == Button5)
        z = 1;

    sync_kbd_state(qs, key.state);
    if (button || z) {
        if (key.type == ButtonPress)
            qs->buttons |= button;
        else
            qs->buttons &= ~button;
        if (kbd_mouse_is_absolute())
            kbd_mouse_event(qs->x * 0x7FFF /
                            (ds_get_width(qs->ds) - 1),
                            qs->y * 0x7FFF /
                            (ds_get_height(qs->ds) - 1), z,
                            qs->buttons);
        else
            kbd_mouse_event(0, 0, 0, qs->buttons);
    } else {
        fprintf(stderr, "send buttonevent: unknown button %d\n",
                key.button);
    }
}

void handle_motion(QubesGuiState * qs)
{
    struct msg_motion key;
    int new_x, new_y;

    read_data(vchan, (char *) &key, sizeof(key));
    new_x = key.x;
    new_y = key.y;

    if (new_x >= ds_get_width(qs->ds))
        new_x = ds_get_width(qs->ds) - 1;
    if (new_y >= ds_get_height(qs->ds))
        new_y = ds_get_height(qs->ds) - 1;
    if (kbd_mouse_is_absolute()) {
        kbd_mouse_event(new_x * 0x7FFF / (ds_get_width(qs->ds) - 1),
                        new_y * 0x7FFF / (ds_get_height(qs->ds) - 1),
                        0,    /* TODO? */
                        qs->buttons);
    } else {
        kbd_mouse_event(new_x - qs->x, new_y - qs->y, 0,    /* TODO? */
                qs->buttons);
    }
    qs->x = new_x;
    qs->y = new_y;
}



void handle_keymap_notify(QubesGuiState * qs)
{
    int i;
    unsigned char remote_keys[32];
    read_struct(vchan, remote_keys);
    for (i = 0; i < 256; i++) {
        if (!is_bitset(remote_keys, i) && is_bitset(qs->local_keys, i)) {
            send_keycode(qs, i, 1);
            if (qs->log_level > 1)
                fprintf(stderr,
                        "handle_keymap_notify: unsetting key %d\n",
                        i);
        }
    }
}

void send_protocol_version(void)
{
    uint32_t version = QUBES_GUID_PROTOCOL_VERSION;
    write_struct(vchan, version);
}


/* end of based on gui-agent/vmside.c */

static void qubesgui_pv_update(DisplayChangeListener * dcl, int x, int y, int w,
                               int h)
{
    QubesGuiState *qs = container_of(dcl, QubesGuiState, dcl);
    if (!qs->init_done)
        return;
    // ignore one-line updates, Windows send them constantly at no reason
    if (h == 1)
        return;
    process_pv_update(qs, x, y, w, h);
}

static void qubesgui_pv_switch(DisplayChangeListener * dcl, DisplaySurface * surface)
{
    QubesGuiState *qs = container_of(dcl, QubesGuiState, dcl);

    if (!qs->init_done)
        return;

    qs->surface = surface;

    process_pv_resize(qs);
}

static void qubesgui_pv_refresh(DisplayChangeListener * dcl)
{
    vga_hw_update();
}

static bool qubesgui_pv_check_format(DisplayChangeListener *dcl,
                                     pixman_format_code_t format)
{
    return format == PIXMAN_x8r8g8b8;
}

static void qubesgui_message_handler(void *opaque)
{
#define KBD_NUM_BATCH 64
    QubesGuiState *qs = opaque;
    char discard[256];


    libvchan_wait(vchan);
    if (!qs->init_done) {
        qubesgui_init_connection(qs);
        return;
    }
    if (!libvchan_is_open(vchan)) {
        qs->init_done = 0;
        qs->init_state = 0;
        qemu_set_fd_handler(libvchan_fd_for_select(vchan), NULL, NULL, NULL);
        libvchan_close(vchan);
        /* FIXME: 0 here is hardcoded remote domain */
        vchan = peer_server_init(0, 6000);
        qemu_set_fd_handler(libvchan_fd_for_select(vchan),
                            qubesgui_message_handler, NULL, qs);
        fprintf(stderr,
                "qubes_gui: viewer disconnected, waiting for new connection\n");
        return;
    }

    write_data(vchan, NULL, 0); // trigger write of queued data, if any present
    if (libvchan_data_ready(vchan) == 0) {
        return;
    }

    if (!qs->hdr.type) {
        int hdr_size;
        /* read the header if not already done */
        hdr_size = read_data(vchan, (char *) &qs->hdr, sizeof(qs->hdr));
        if (hdr_size != sizeof(qs->hdr)) {
            fprintf(stderr,
                    "qubes_gui: got incomplete header (%d instead of %lu)\n",
                    hdr_size, sizeof(qs->hdr));
        }
    }

    if (qs->hdr.type && qs->vchan_data_to_discard < 0) {
        /* got header, check the data */

        /* fast path for not supported messages */
        switch (qs->hdr.type) {
            case MSG_KEYPRESS:
            case MSG_BUTTON:
            case MSG_MOTION:
            case MSG_KEYMAP_NOTIFY:
            case MSG_CONFIGURE:
                /* supported - handled later */
                break;
            default:
                fprintf(stderr,
                        "qubes_gui: got unknown msg type %d, ignoring\n",
                        qs->hdr.type);
            case MSG_CLIPBOARD_REQ:
            case MSG_CLIPBOARD_DATA:
            case MSG_MAP:
            case MSG_CLOSE:
            case MSG_CROSSING:
            case MSG_FOCUS:
            case MSG_EXECUTE:
                qs->vchan_data_to_discard = qs->hdr.untrusted_len;
        }
    }

    if (qs->vchan_data_to_discard >= 0) {
        while (libvchan_data_ready(vchan) && qs->vchan_data_to_discard) {
            qs->vchan_data_to_discard -= libvchan_read(vchan, discard,
                    min(qs->vchan_data_to_discard, sizeof(discard)));
        }
        if (!qs->vchan_data_to_discard) {
            /* whole message "processed" */
            qs->hdr.type = 0;
            /* -1 to distinguish between "0 bytes to discard" and "do not
             * discard this data" */
            qs->vchan_data_to_discard = -1;
        }
        return;
    }

    /* WARNING: here is an assumption that every payload (of supported
     * message) will fit into vchan buffer; for now it is true, but once
     * this agent will start support for bigger messages, some local
     * buffering needs to be done */
    if (libvchan_data_ready(vchan) < qs->hdr.untrusted_len) {
        /* wait for data */
        return;
    }

    switch (qs->hdr.type) {
    case MSG_KEYPRESS:
        handle_keypress(qs);
        break;
    case MSG_BUTTON:
        handle_button(qs);
        break;
    case MSG_MOTION:
        handle_motion(qs);
        break;
    case MSG_KEYMAP_NOTIFY:
        handle_keymap_notify(qs);
        break;
    case MSG_CONFIGURE:
        handle_configure(qs);
        break;
    default:
        fprintf(stderr,
                "BUG: qubes_gui: "
                "got unknown msg type %d, but not ignored earlier\n",
                qs->hdr.type);
        exit(1);
    }
    qs->hdr.type = 0;
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name = "qubes-gui",
    .dpy_gfx_update = qubesgui_pv_update,
    .dpy_gfx_switch = qubesgui_pv_switch,
    .dpy_gfx_check_format = qubesgui_pv_check_format,
    .dpy_refresh = qubesgui_pv_refresh
};

int qubesgui_pv_display_init(DisplayState * ds)
{

    fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    qs = g_new0(QubesGuiState, 1);
    if (!qs)
        return -1;

    qs->init_done = 0;
    qs->init_state = 0;

    qs->u2mfn_fd = u2mfn_get_fd();
    if (qs->u2mfn_fd == -1) {
        perror("u2mfn_get_fd failed");
        return -1;
    }

    fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    qs->dcl.ops = &dcl_ops;
    fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
    register_displaychangelistener(&qs->dcl);

    /* FIXME: 0 here is hardcoded remote domain */
    vchan = peer_server_init(0, 6000);
    qemu_set_fd_handler(libvchan_fd_for_select(vchan),
                        qubesgui_message_handler,
                        NULL,
                        qs);
    qubesgui_init_connection(qs);

    return 0;
}

int qubesgui_pv_display_vram(void *data)
{
    vga_vram = data;
    return 0;
}

void qubesgui_init_connection(QubesGuiState * qs)
{
    struct msg_xconf xconf;

    if (qs->init_state == 0) {
        qs->hdr.type = 0;
        /* -1 to distinguish between "0 bytes to discard" and "do not
         * discard this data" */
        qs->vchan_data_to_discard = -1;
        send_protocol_version();
        fprintf(stderr,
                "qubes_gui/init[%d]: version sent, waiting for xorg conf\n",
                __LINE__);
        // XXX warning - thread unsafe
        qs->init_state++;
    }
    if (qs->init_state == 1) {
        if (!libvchan_data_ready(vchan))
            return;

        read_struct(vchan, xconf);
        fprintf(stderr,
                "qubes_gui/init[%d]: got xorg conf, creating window\n",
                __LINE__);
        // If we don't have a surface yet just send an arbitary window
        // size. QEMU should set a surface very soon.
        qubes_create_window(qs,
                            qs->surface ? surface_width(qs->surface) : 100,
                            qs->surface ? surface_height(qs->surface) : 100);

        send_map(qs);
        send_wmname(qs, qemu_get_vm_name());

        fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
        /* process_pv_resize will send mfns */
        process_pv_resize(qs);

        qs->init_state++;
        qs->init_done = 1;
    }
}
