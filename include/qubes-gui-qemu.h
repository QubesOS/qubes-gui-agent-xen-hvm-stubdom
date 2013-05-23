#ifndef _QUBES_GUI_QEMU_H
#define _QUBES_GUI_QEMU_H
/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2012  Marek Marczykowski <marmarek@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

/* header file for qemu in stubdom */

#include <semaphore.h>
#include <sched.h>
#include <hw/hw.h>
#include <hw/pc.h>

#include <libvchan.h>
#include <console.h>

#include <qubes-gui-protocol.h>

/* from /usr/include/X11/X.h */
#define KeyPress               2
#define ButtonPress            4
#define Button1                 1
#define Button2                 2
#define Button3                 3
#define Button4                 4
#define Button5                 5
#define ShiftMask       (1<<0)
#define LockMask        (1<<1)
#define ControlMask     (1<<2)
#define Mod1Mask        (1<<3)
#define Mod2Mask        (1<<4)
#define Mod3Mask        (1<<5)
#define Mod4Mask        (1<<6)
#define Mod5Mask        (1<<7)


/* from /usr/include/X11/Xutil.h */
#define PPosition       (1L << 2) /* program specified position */
#define PSize           (1L << 3) /* program specified size */
#define PMinSize        (1L << 4) /* program specified minimum size */
#define PMaxSize        (1L << 5) /* program specified maximum size */
#define PResizeInc      (1L << 6) /* program specified resize increments */
#define PAspect         (1L << 7) /* program specified min and max aspect ratios */
#define PBaseSize       (1L << 8) /* program specified base for incrementing */
#define PWinGravity     (1L << 9) /* program specified window gravity */


typedef struct QubesGuiState {
    void *nonshared_vram;
    struct DisplayState *ds;
	int log_level;
	libvchan_t *vchan;
	/* current message, keep here b/c  */
	struct msg_hdr hdr;
	/* amount of data to discard */
	int vchan_data_to_discard;

	char *clipboard_data;
	int clipboard_data_len;
	int x;
	int y;
	int z; /* TODO */
	int buttons;
	int init_done;
	int init_state;
	unsigned char local_keys[32];
} QubesGuiState;

int qubesgui_pv_display_init(struct DisplayState *ds);
void qubesgui_init_connection(QubesGuiState *qs);

extern uint32_t qubes_keycode2scancode[256];

#endif /* _QUBES_GUI_QEMU_H */
