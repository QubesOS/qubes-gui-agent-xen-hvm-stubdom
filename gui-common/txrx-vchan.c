/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libvchan.h>
#include <sys/select.h>
#include <xenstore.h>
#include "double-buffer.h"

int double_buffered = 0;

void handle_vchan_error(libvchan_t *vchan, const char *op)
{
    if (!libvchan_is_open(vchan)) {
        fprintf(stderr, "EOF\n");
        exit(0);
    } else {
        fprintf(stderr, "Error while vchan %s, terminating\n", op);
        exit(1);
    }
}

int write_data_exact(libvchan_t *vchan, char *buf, int size)
{
	int written = 0;
	int ret;

	while (written < size) {
		ret = libvchan_write(vchan, buf + written, size - written);
		if (ret <= 0)
            handle_vchan_error(vchan, "write data");
		written += ret;
	}
//      fprintf(stderr, "sent %d bytes\n", size);
	return size;
}

int write_data(libvchan_t *vchan, char *buf, int size)
{
	int count;
	if (!double_buffered)
		return write_data_exact(vchan, buf, size); // this may block
	double_buffer_append(buf, size);
	count = libvchan_buffer_space(vchan);
	if (count > double_buffer_datacount())
		count = double_buffer_datacount();
        // below, we write only as much data as possible without
        // blocking; remainder of data stays in the double buffer
	write_data_exact(vchan, double_buffer_data(), count);
	double_buffer_substract(count);
	return size;
}

int real_write_message(libvchan_t *vchan, char *hdr, int size, char *data, int datasize)
{
	write_data(vchan, hdr, size);
	write_data(vchan, data, datasize);
	return 0;
}

int read_data(libvchan_t *vchan, char *buf, int size)
{
	int written = 0;
	int ret;
	while (written < size) {
		ret = libvchan_read(vchan, buf + written, size - written);
		if (ret < 0)
            handle_vchan_error(vchan, "read data");
		written += ret;
	}
//      fprintf(stderr, "read %d bytes\n", size);
	return size;
}

int wait_for_vchan_or_argfd_once(libvchan_t *vchan, int nfd, int *fd, fd_set * retset)
{
	fd_set rfds;
	int vfd, max = 0, ret, i;
	struct timeval tv = { 0, 100000 };
	write_data(vchan, NULL, 0);	// trigger write of queued data, if any present
	vfd = libvchan_fd_for_select(vchan);
	FD_ZERO(&rfds);
	for (i = 0; i < nfd; i++) {
		int cfd = fd[i];
		FD_SET(cfd, &rfds);
		if (cfd > max)
			max = cfd;
	}
	FD_SET(vfd, &rfds);
	if (vfd > max)
		max = vfd;
	max++;
	ret = select(max, &rfds, NULL, NULL, &tv);
	if (ret < 0 && errno == EINTR)
		return 0;
	if (ret < 0) {
		perror("select");
		exit(1);
	}
	if (!libvchan_is_open(vchan)) {
		fprintf(stderr, "libvchan_is_eof\n");
		exit(0);
	}
	if (FD_ISSET(vfd, &rfds))
		// the following will never block; we need to do this to
		// clear libvchan_fd pending state 
		libvchan_wait(vchan);
	if (retset)
		*retset = rfds;
	return ret;
}

void wait_for_vchan_or_argfd(libvchan_t *vchan, int nfd, int *fd, fd_set * retset)
{
	while (wait_for_vchan_or_argfd_once(vchan, nfd, fd, retset) == 0);
}

libvchan_t *peer_server_init(int domain, int port)
{
    libvchan_t *vchan;
#ifdef CONFIG_STUBDOM
	double_buffer_init();
	double_buffered = 1;
#else
	double_buffered = 0; // writes to vchan may block
#endif
	vchan = libvchan_server_init(domain, port, 4096, 4096);
	if (!vchan) {
		perror("libvchan_server_init");
		exit(1);
	}
	return vchan;
}

char *get_vm_name(int dom, int *target_dom)
{
	struct xs_handle *xs;
	char buf[64];
	char *name;
	char *target_dom_str;
	unsigned int len = 0;

	xs = xs_open(0);
	if (!xs) {
		perror("xs_daemon_open");
		exit(1);
	}
	snprintf(buf, sizeof(buf), "/local/domain/%d/target", dom);
	target_dom_str = xs_read(xs, 0, buf, &len);
	if (target_dom_str) {
		errno = 0;
		*target_dom = strtol(target_dom_str, (char **) NULL, 10);
		if (errno != 0) {
			perror("strtol");
			exit(1);
		}
	} else
		*target_dom = dom;
	snprintf(buf, sizeof(buf), "/local/domain/%d/name", *target_dom);
	name = xs_read(xs, 0, buf, &len);
	if (!name) {
		perror("xs_read domainname");
		exit(1);
	}
	xs_close(xs);
	return name;
}
