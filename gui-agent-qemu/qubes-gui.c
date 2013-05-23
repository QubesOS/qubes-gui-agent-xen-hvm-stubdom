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
#include <txrx.h>

#define QUBES_GUI_PROTOCOL_VERSION_STUBDOM (1 << 16 | 0)

struct QubesGuiState *qs;
libvchan_t *vchan;

#define min(x,y) ((x)>(y)?(y):(x))
#define QUBES_MAIN_WINDOW 1

static void *vga_vram;

static DisplayChangeListener *dcl;

extern uint32_t vga_ram_size;

static void update_24bpp_from_16bpp(uint16_t *src, uint32_t *dst, int
		src_linesize, int dst_linesize, int x, int y, int width, int height) {
	int i, j;
	int r,g,b;

	/* it is really keep as 32bpp for convenient array elements access (same as Xorg
	 * expect) */
	for (j=y; j<y+height; j++) {
		for (i=x; i<x+width; i++) {
			r=(src[src_linesize*j+i] >> 11)<<3;
			g=((src[src_linesize*j+i] & ((1<<11)-1)) >> 5) << 2;
			b=((src[src_linesize*j+i] & ((1<<5)-1))) << 3;
			dst[dst_linesize*j+i] = (r << 16) | (g << 8) | b;
		}
	}
}

static void update_24bpp_from_8bpp(uint8_t *src, uint32_t *dst, int
		src_linesize, int dst_linesize, int x, int y, int width, int height) {
	int i, j;
	int r,g,b;

	/* it is really keep as 32bpp for convenient array elements access (same as Xorg
	 * expect) */
	for (j=y; j<y+height; j++) {
		for (i=x; i<x+width; i++) {
			r=(src[src_linesize*j+i] >> 5)<<5;
			g=((src[src_linesize*j+i] & ((1<<5)-1)) >> 2) << 5;
			b=((src[src_linesize*j+i] & ((1<<2)-1))) << 6;
			dst[dst_linesize*j+i] = (r << 16) | (g << 8) | b;
		}
	}
}

void process_pv_update(QubesGuiState * qs,
		       int x, int y, int width, int height)
{
	struct msg_shmimage mx;
	struct msg_hdr hdr;

	/* convert to 24bpp if needed */
	if (is_buffer_shared(qs->ds->surface)) {
		switch (ds_get_bits_per_pixel(qs->ds)) {
			case 16:
				update_24bpp_from_16bpp((uint16_t*)ds_get_data(qs->ds),
						qs->nonshared_vram, ds_get_linesize(qs->ds)/2,
						ds_get_width(qs->ds), x, y, width, height);
				break;
			case 8:
				update_24bpp_from_8bpp(ds_get_data(qs->ds), qs->nonshared_vram,
						ds_get_linesize(qs->ds), ds_get_width(qs->ds), x, y,
						width, height);
				break;
		}
	}
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
	int n, bpp;
	int i;
	void *data;
	int offset, copy_offset;

	bpp = ds_get_bits_per_pixel(qs->ds);
	if (is_buffer_shared(qs->ds->surface)) {
		data = ((void *) ds_get_data(qs->ds));
		switch (ds_get_bits_per_pixel(qs->ds)) {
			case 16:
				fprintf(stderr, "16bpp detected, converting to 24bpp\n");
				update_24bpp_from_16bpp(data, qs->nonshared_vram,
						ds_get_linesize(qs->ds)/2, ds_get_width(qs->ds), 0, 0,
						ds_get_width(qs->ds), ds_get_height(qs->ds));
				data = qs->nonshared_vram;
				bpp = 24;
				break;
			case 8:
				fprintf(stderr, "8bpp detected, converting to 24bpp\n");
				update_24bpp_from_8bpp(data, qs->nonshared_vram,
						ds_get_linesize(qs->ds), ds_get_width(qs->ds), 0, 0,
						ds_get_width(qs->ds), ds_get_height(qs->ds));
				data = qs->nonshared_vram;
				bpp = 24;
				break;
			case 24:
			case 32:
				/* no conversion needed */
				break;
			default:
				fprintf(stderr, 
						"%dbpp not supported, expect messy display content\n", 
						ds_get_bits_per_pixel(qs->ds));
		}
	} else {
		data = qs->nonshared_vram;
	}

	offset = (long) data & (XC_PAGE_SIZE - 1);

	/* XXX: hardcoded 4 bytes per pixel - gui-daemon doesn't handle other bpp */
	n = (4 * ds_get_width(qs->ds) * ds_get_height(qs->ds) + offset + (XC_PAGE_SIZE-1)) / XC_PAGE_SIZE;
	mfns = malloc(n * sizeof(*mfns));
	if (!mfns) {
		fprintf(stderr, "Cannot allocate mfns array, %lu bytes needed\n", n * sizeof(*mfns));
		return;
	}
	fprintf(stderr, "dumping mfns: n=%d, w=%d, h=%d, bpp=%d\n", n, ds_get_width(qs->ds), ds_get_height(qs->ds), bpp);
	for (i = 0; i < n; i++)
		mfns[i] = virtual_to_mfn(data + i * XC_PAGE_SIZE);
	hdr.type = MSG_MFNDUMP;
	hdr.window = QUBES_MAIN_WINDOW;
	hdr.untrusted_len = sizeof(shmcmd) + n * sizeof(*mfns);
	shmcmd.width = ds_get_width(qs->ds);
	shmcmd.height = ds_get_height(qs->ds);
	shmcmd.num_mfn = n;
	shmcmd.off = offset;
	shmcmd.bpp = bpp;
	write_struct(vchan, hdr);
	write_struct(vchan, shmcmd);
	write_data(vchan, (char *) mfns, n * sizeof(*mfns));
	free(mfns);
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
	msg.min_width = ds_get_width(qs->ds);
	msg.min_height = ds_get_height(qs->ds);
	msg.max_width = ds_get_width(qs->ds);
	msg.max_height = ds_get_height(qs->ds);
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

void process_pv_resize(QubesGuiState * qs, int width, int height,
		       int linesize)
{
	struct msg_hdr hdr;
	struct msg_configure conf;
	if (qs->log_level > 1)
		fprintf(stderr,
			"handle resize  w=%d h=%d\n", width, height);
	hdr.type = MSG_CONFIGURE;
	hdr.window = QUBES_MAIN_WINDOW;
	conf.x = qs->x;
	conf.y = qs->y;
	conf.width = width;
	conf.height = height;
	conf.override_redirect = 0;
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
		kbd_mouse_event(new_x * 0x7FFF / (ds_get_width(qs->ds) - 1), new_y * 0x7FFF / (ds_get_height(qs->ds) - 1), 0,	/* TODO? */
				qs->buttons);
	} else {
		kbd_mouse_event(new_x - qs->x, new_y - qs->y, 0,	/* TODO? */
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
		if (!is_bitset(remote_keys, i)
		    && is_bitset(qs->local_keys, i)) {
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

static void qubesgui_pv_update(DisplayState * ds, int x, int y, int w,
			       int h)
{
	QubesGuiState *qs = ds->opaque;
	if (!qs->init_done)
		return;
	// ignore one-line updates, Windows send them constantly at no reason
	if (h == 1)
		return;
	process_pv_update(qs, x, y, w, h);
}

static void qubesgui_pv_resize(DisplayState * ds)
{
	QubesGuiState *qs = ds->opaque;

	fprintf(stderr, "resize to %dx%d@%d, %d required\n",
		ds_get_width(ds), ds_get_height(ds),
		ds_get_bits_per_pixel(ds), ds_get_linesize(ds));
	if (!qs->init_done)
		return;

	process_pv_resize(qs, ds_get_width(ds), ds_get_height(ds),
			  ds_get_linesize(ds));
}

static void qubesgui_pv_setdata(DisplayState * ds)
{
	QubesGuiState *qs = ds->opaque;

	if (!qs->init_done)
		return;
	process_pv_resize(qs, ds_get_width(ds), ds_get_height(ds),
			  ds_get_linesize(ds));
}

static void qubesgui_pv_refresh(DisplayState * ds)
{
	vga_hw_update();
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

	write_data(vchan, NULL, 0);	// trigger write of queued data, if any present
	if (libvchan_data_ready(vchan) == 0) {
		return;
	}

	if (!qs->hdr.type) {
		int hdr_size;
		/* read the header if not already done */
		hdr_size = read_data(vchan, (char *) &qs->hdr, sizeof(qs->hdr));
		if (hdr_size != sizeof(qs->hdr)) {
			fprintf(stderr, "qubes_gui: got incomplete header (%d instead of %lu)\n",
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
				fprintf(stderr, "qubes_gui: got unknown msg type %d, ignoring\n",
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
		fprintf(stderr, "BUG: qubes_gui: got unknown msg type %d, but not ignored earlier\n",
			qs->hdr.type);
		exit(1);
	}
	qs->hdr.type = 0;
}

static DisplaySurface *qubesgui_create_displaysurface(int width,
						      int height)
{
	DisplaySurface *surface =
	    (DisplaySurface *) qemu_mallocz(sizeof(DisplaySurface));
	if (surface == NULL) {
		fprintf(stderr,
			"qubesgui_create_displaysurface: malloc failed\n");
		exit(1);
	}

	surface->width = width;
	surface->height = height;
	surface->linesize = width * 4;
	surface->pf = qemu_default_pixelformat(32);
#ifdef WORDS_BIGENDIAN
	surface->flags = QEMU_ALLOCATED_FLAG | QEMU_BIG_ENDIAN_FLAG;
#else
	surface->flags = QEMU_ALLOCATED_FLAG;
#endif
	surface->data = qs->nonshared_vram;

	return surface;
}

static DisplaySurface *qubesgui_resize_displaysurface(DisplaySurface *
						      surface, int width,
						      int height)
{
	surface->width = width;
	surface->height = height;
	surface->linesize = width * 4;
	surface->pf = qemu_default_pixelformat(32);
#ifdef WORDS_BIGENDIAN
	surface->flags = QEMU_ALLOCATED_FLAG | QEMU_BIG_ENDIAN_FLAG;
#else
	surface->flags = QEMU_ALLOCATED_FLAG;
#endif
	surface->data = qs->nonshared_vram;

	return surface;
}

static void qubesgui_free_displaysurface(DisplaySurface * surface)
{
	if (surface == NULL)
		return;
	qemu_free(surface);
}

static void qubesgui_pv_display_allocator(void)
{
	DisplaySurface *ds;
	DisplayAllocator *da = qemu_mallocz(sizeof(DisplayAllocator));
	da->create_displaysurface = qubesgui_create_displaysurface;
	da->resize_displaysurface = qubesgui_resize_displaysurface;
	da->free_displaysurface = qubesgui_free_displaysurface;
	if (register_displayallocator(qs->ds, da) != da) {
		fprintf(stderr,
			"qubesgui_pv_display_allocator: could not register DisplayAllocator\n");
		exit(1);
	}

	qs->nonshared_vram = qemu_memalign(XC_PAGE_SIZE, vga_ram_size);
	if (!qs->nonshared_vram) {
		fprintf(stderr,
			"qubesgui_pv_display_allocator: could not allocate nonshared_vram\n");
		exit(1);
	}
	/* Touch the pages before sharing them */
	memset(qs->nonshared_vram, 0xff, vga_ram_size);

	ds = qubesgui_create_displaysurface(ds_get_width(qs->ds),
					    ds_get_height(qs->ds));
	defaultallocator_free_displaysurface(qs->ds->surface);
	qs->ds->surface = ds;
}

int qubesgui_pv_display_init(DisplayState * ds)
{

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
	qs = qemu_mallocz(sizeof(QubesGuiState));
	if (!qs)
		return -1;

	qs->ds = ds;
	qs->init_done = 0;
	qs->init_state = 0;

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
	qubesgui_pv_display_allocator();

	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
	dcl = qemu_mallocz(sizeof(DisplayChangeListener));
	if (!dcl)
		exit(1);
	ds->opaque = qs;
	dcl->dpy_update = qubesgui_pv_update;
	dcl->dpy_resize = qubesgui_pv_resize;
	dcl->dpy_setdata = qubesgui_pv_setdata;
	dcl->dpy_refresh = qubesgui_pv_refresh;
	fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
	register_displaychangelistener(ds, dcl);

	/* FIXME: 0 here is hardcoded remote domain */
	vchan = peer_server_init(0, 6000);
	qemu_set_fd_handler(libvchan_fd_for_select(vchan), qubesgui_message_handler, NULL,
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
		qubes_create_window(qs, ds_get_width(qs->ds),
				    ds_get_height(qs->ds));

		send_map(qs);
		send_wmname(qs, qemu_name);

		fprintf(stderr, "qubes_gui/init: %d\n", __LINE__);
		/* process_pv_resize will send mfns */
		process_pv_resize(qs, ds_get_width(qs->ds),
				  ds_get_height(qs->ds),
				  ds_get_linesize(qs->ds));

		qs->init_state++;
		qs->init_done = 1;
	}
}

// vim:ts=4:noet:sw=4:
