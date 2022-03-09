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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <imsg.h>
#include <unistd.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include "amused.h"
#include "log.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

int
play_oggvorbis(int fd)
{
	static uint8_t pcmout[4096];
	FILE *f;
	OggVorbis_File vf;
	vorbis_info *vi;
	int current_section, eof = 0, ret = 0;

	if ((f = fdopen(fd, "r")) == NULL)
		err(1, "fdopen");

	if (ov_open_callbacks(f, &vf, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0) {
		log_warnx("input is not an Ogg bitstream");
		ret = -1;
		goto end;
	}

	/*
	 * we could extract some tags by looping over the NULL
	 * terminated array returned by ov_comment(&vf, -1), see
	 * previous revision of this file.
	 */
	vi = ov_info(&vf, -1);
	if (player_setup(16, vi->rate, vi->channels) == -1)
		err(1, "player_setrate");

	while (!eof) {
		long ret;

		ret = ov_read(&vf, pcmout, sizeof(pcmout), 0, 2, 1,
		    &current_section);
		if (ret == 0)
			eof = 1;
		else if (ret > 0) {
			/* TODO: deal with sample rate changes */
			if (!play(pcmout, ret)) {
				ret = 1;
				break;
			}
		}
	}

	ov_clear(&vf);

end:
	fclose(f);
	return ret;
}
