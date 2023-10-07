/*	$OpenBSD: control.c,v 1.8 2021/03/02 04:10:07 jsg Exp $	*/

/*
 * Copyright (c) 2022, 2023 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <net/if.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amused.h"
#include "ev.h"
#include "log.h"
#include "control.h"
#include "playlist.h"

#define	CONTROL_BACKLOG	5

struct {
	int		fd;
	struct playlist	play;
	int		tx;
} control_state = {.fd = -1, .tx = -1};

struct ctl_conn {
	TAILQ_ENTRY(ctl_conn)	entry;
	int			monitor; /* 1 if client is in monitor mode */
	struct imsgev		iev;
};

TAILQ_HEAD(ctl_conns, ctl_conn)	ctl_conns = TAILQ_HEAD_INITIALIZER(ctl_conns);

struct ctl_conn	*control_connbyfd(int);
struct ctl_conn	*control_connbypid(pid_t);
void		 control_close(int);

int
control_init(char *path)
{
	struct sockaddr_un	 sun;
	int			 fd, flags;
	mode_t			 old_umask;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		log_warn("%s: socket", __func__);
		return (-1);
	}

	if ((flags = fcntl(fd, F_GETFL)) == -1 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		fatal("fcntl(O_NONBLOCK)");

	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
		fatal("fcntl(CLOEXEC)");

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, path, sizeof(sun.sun_path));

	if (unlink(path) == -1)
		if (errno != ENOENT) {
			log_warn("%s: unlink %s", __func__, path);
			close(fd);
			return (-1);
		}

	old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_warn("%s: bind: %s", __func__, path);
		close(fd);
		umask(old_umask);
		return (-1);
	}
	umask(old_umask);

	if (chmod(path, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) == -1) {
		log_warn("%s: chmod", __func__);
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	return (fd);
}

static void
enable_accept(int fd, int ev, void *bula)
{
	ev_add(control_state.fd, POLLIN, control_accept, NULL);
}

int
control_listen(int fd)
{
	if (control_state.fd != -1)
		fatalx("%s: received unexpected controlsock", __func__);

	control_state.fd = fd;
	if (listen(control_state.fd, CONTROL_BACKLOG) == -1) {
		log_warn("%s: listen", __func__);
		return (-1);
	}

	enable_accept(-1, 0, NULL);
	return (0);
}

void
control_accept(int listenfd, int event, void *bula)
{
	int			 connfd;
	socklen_t		 len;
	struct sockaddr_un	 sun;
	struct ctl_conn		*c;

	len = sizeof(sun);
	if ((connfd = accept4(listenfd, (struct sockaddr *)&sun, &len,
	    SOCK_CLOEXEC | SOCK_NONBLOCK)) == -1) {
		/*
		 * Pause accept if we are out of file descriptors, or
		 * libevent will haunt us here too.
		 */
		if (errno == ENFILE || errno == EMFILE) {
			struct timeval evtpause = { 1, 0 };

			ev_del(control_state.fd);
			ev_timer(&evtpause, enable_accept, NULL);
		} else if (errno != EWOULDBLOCK && errno != EINTR &&
		    errno != ECONNABORTED)
			log_warn("%s: accept4", __func__);
		return;
	}

	if ((c = calloc(1, sizeof(struct ctl_conn))) == NULL) {
		log_warn("%s: calloc", __func__);
		close(connfd);
		return;
	}

	imsg_init(&c->iev.ibuf, connfd);
	c->iev.handler = control_dispatch_imsg;
	c->iev.events = POLLIN;
	ev_add(c->iev.ibuf.fd, c->iev.events, c->iev.handler, &c->iev);

	TAILQ_INSERT_TAIL(&ctl_conns, c, entry);
}

struct ctl_conn *
control_connbyfd(int fd)
{
	struct ctl_conn	*c;

	TAILQ_FOREACH(c, &ctl_conns, entry) {
		if (c->iev.ibuf.fd == fd)
			break;
	}

	return (c);
}

struct ctl_conn *
control_connbypid(pid_t pid)
{
	struct ctl_conn	*c;

	TAILQ_FOREACH(c, &ctl_conns, entry) {
		if (c->iev.ibuf.pid == pid)
			break;
	}

	return (c);
}

void
control_close(int fd)
{
	struct ctl_conn	*c;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warnx("%s: fd %d: not found", __func__, fd);
		return;
	}

	/* abort the transaction if running by this user */
	if (control_state.tx != -1 && c->iev.ibuf.fd == control_state.tx) {
		playlist_free(&control_state.play);
		control_state.tx = -1;
	}

	msgbuf_clear(&c->iev.ibuf.w);
	TAILQ_REMOVE(&ctl_conns, c, entry);

	ev_del(c->iev.ibuf.fd);
	close(c->iev.ibuf.fd);

	/* Some file descriptors are available again. */
	if (ev_timer_pending()) {
		ev_timer(NULL, NULL, NULL);
		ev_add(control_state.fd, POLLIN, control_accept, NULL);
	}

	free(c);
}

void
control_notify(int type)
{
	struct ctl_conn *c;
	struct player_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.event = type;
	ev.position = current_position;
	ev.duration = current_duration;
	ev.mode.repeat_one = repeat_one;
	ev.mode.repeat_all = repeat_all;
	ev.mode.consume = consume;

	TAILQ_FOREACH(c, &ctl_conns, entry) {
		if (!c->monitor)
			continue;

		imsg_compose_event(&c->iev, IMSG_CTL_MONITOR, 0, 0,
		    -1, &ev, sizeof(ev));
	}
}

static int
new_mode(int val, int newval)
{
	if (newval == MODE_UNDEF)
		return val;
	if (newval == MODE_TOGGLE)
		return !val;
	return !!newval;
}

void
control_dispatch_imsg(int fd, int event, void *bula)
{
	struct ctl_conn		*c;
	struct imsg		 imsg;
	struct player_mode	 mode;
	struct player_seek	 seek;
	ssize_t		 	 n, off;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warnx("%s: fd %d: not found", __func__, fd);
		return;
	}

	if (event & POLLIN) {
		if (((n = imsg_read(&c->iev.ibuf)) == -1 && errno != EAGAIN) ||
		    n == 0) {
			control_close(fd);
			return;
		}
	}
	if (event & POLLOUT) {
		if (msgbuf_write(&c->iev.ibuf.w) <= 0 && errno != EAGAIN) {
			control_close(fd);
			return;
		}
	}

	for (;;) {
		if ((n = imsg_get(&c->iev.ibuf, &imsg)) == -1) {
			control_close(fd);
			return;
		}
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_CTL_PLAY:
			switch (play_state) {
			case STATE_STOPPED:
				main_playlist_resume();
				break;
			case STATE_PLAYING:
				/* do nothing */
				break;
			case STATE_PAUSED:
				play_state = STATE_PLAYING;
				main_send_player(IMSG_RESUME, -1, NULL ,0);
				break;
			}
			control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_TOGGLE_PLAY:
			switch (play_state) {
			case STATE_STOPPED:
				control_notify(IMSG_CTL_PLAY);
				main_playlist_resume();
				break;
			case STATE_PLAYING:
				control_notify(IMSG_CTL_PAUSE);
				play_state = STATE_PAUSED;
				main_send_player(IMSG_PAUSE, -1, NULL, 0);
				break;
			case STATE_PAUSED:
				control_notify(IMSG_CTL_PLAY);
				play_state = STATE_PLAYING;
				main_send_player(IMSG_RESUME, -1, NULL, 0);
				break;
			}
			break;
		case IMSG_CTL_PAUSE:
			if (play_state != STATE_PLAYING)
				break;
			play_state = STATE_PAUSED;
			main_send_player(IMSG_PAUSE, -1, NULL, 0);
			control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_STOP:
			if (play_state == STATE_STOPPED)
				break;
			play_state = STATE_STOPPED;
			main_send_player(IMSG_STOP, -1, NULL, 0);
			control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_FLUSH:
			playlist_truncate();
			control_notify(IMSG_CTL_COMMIT);
			break;
		case IMSG_CTL_SHOW:
			main_send_playlist(&c->iev);
			break;
		case IMSG_CTL_STATUS:
			main_send_status(&c->iev);
			break;
		case IMSG_CTL_NEXT:
			control_notify(imsg.hdr.type);
			main_send_player(IMSG_STOP, -1, NULL, 0);
			main_playlist_advance();
			break;
		case IMSG_CTL_PREV:
			control_notify(imsg.hdr.type);
			main_send_player(IMSG_STOP, -1, NULL, 0);
			main_playlist_previous();
			break;
		case IMSG_CTL_JUMP:
			main_playlist_jump(&c->iev, &imsg);
			break;
		case IMSG_CTL_MODE:
			if (IMSG_DATA_SIZE(imsg) != sizeof(mode)) {
				log_warnx("%s: got wrong size", __func__);
				break;
			}
			memcpy(&mode, imsg.data, sizeof(mode));
			consume = new_mode(consume, mode.consume);
			repeat_all = new_mode(repeat_all, mode.repeat_all);
			repeat_one = new_mode(repeat_one, mode.repeat_one);
			control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_BEGIN:
			if (control_state.tx != -1) {
				main_senderr(&c->iev, "locked");
				break;
			}
			control_state.tx = c->iev.ibuf.fd;
			imsg_compose_event(&c->iev, IMSG_CTL_BEGIN, 0, 0, -1,
			    NULL, 0);
			break;
		case IMSG_CTL_ADD:
			if (control_state.tx != -1 &&
			    control_state.tx != c->iev.ibuf.fd) {
				main_senderr(&c->iev, "locked");
				break;
			}
			main_enqueue(control_state.tx != -1,
			   &control_state.play,&c->iev, &imsg);
			if (control_state.tx == -1)
				control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_COMMIT:
			if (control_state.tx != c->iev.ibuf.fd) {
				main_senderr(&c->iev, "locked");
				break;
			}
			if (IMSG_DATA_SIZE(imsg) != sizeof(off)) {
				main_senderr(&c->iev, "wrong size");
				break;
			}
			memcpy(&off, imsg.data, sizeof(off));
			playlist_swap(&control_state.play, off);
			memset(&control_state.play, 0,
			    sizeof(control_state.play));
			control_state.tx = -1;
			imsg_compose_event(&c->iev, IMSG_CTL_COMMIT, 0, 0, -1,
			    NULL, 0);
			control_notify(imsg.hdr.type);
			break;
		case IMSG_CTL_MONITOR:
			c->monitor = 1;
			break;
		case IMSG_CTL_SEEK:
			if (IMSG_DATA_SIZE(imsg) != sizeof(seek)) {
				main_senderr(&c->iev, "wrong size");
				break;
			}
			memcpy(&seek, imsg.data, sizeof(seek));
			main_seek(&seek);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}

	imsg_event_add(&c->iev);
}

int
control_imsg_relay(struct imsg *imsg)
{
	struct ctl_conn	*c;

	if ((c = control_connbypid(imsg->hdr.pid)) == NULL)
		return (0);

	return (imsg_compose_event(&c->iev, imsg->hdr.type, 0, imsg->hdr.pid,
	    -1, imsg->data, IMSG_DATA_SIZE(*imsg)));
}
