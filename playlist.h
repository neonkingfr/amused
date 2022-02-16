/*
 * Copyright (c) 2022 Omar Polo <op@openbsd.org>
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

#ifndef PLAYLIST_H
#define PLAYLIST_H

struct playlist {
	size_t	 len;
	size_t	 cap;
	char	**songs;
};

enum play_state {
	STATE_STOPPED,
	STATE_PLAYING,
	STATE_PAUSED,
};

extern struct playlist	playlist;

extern enum play_state	 play_state;
extern int		 repeat_one;
extern int		 repeat_all;
extern ssize_t		 play_off;

void			 playlist_enqueue(const char *);
const char		*playlist_current(void);
const char		*playlist_advance(void);
void			 playlist_reset(void);
void			 playlist_truncate(void);

#endif
