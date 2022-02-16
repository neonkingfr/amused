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
#include <inttypes.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <imsg.h>

#include <FLAC/stream_decoder.h>

#include "amused.h"

static FLAC__StreamDecoderWriteStatus
writecb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const int32_t * const *buffer, void *data)
{
	static uint8_t buf[BUFSIZ];
	int i;
	size_t len;

	if (player_shouldstop())
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	for (i = 0, len = 0; i < frame->header.blocksize; ++i) {
		if (len+4 >= sizeof(buf)) {
			sio_write(hdl, buf, len);
			len = 0;
		}

		buf[len++] = buffer[0][i] & 0xff;
		buf[len++] = (buffer[0][i] >> 8) & 0xff;

		buf[len++] = buffer[1][i] & 0xff;
		buf[len++] = (buffer[1][i] >> 8) & 0xff;
	}

	if (len != 0)
		sio_write(hdl, buf, len);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void
metacb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *meta,
    void *d)
{
	uint32_t sample_rate;
	struct sio_par par;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
		sample_rate = meta->data.stream_info.sample_rate;

		printf("sample rate: %d\n", sample_rate);
		printf("channels: %d\n", meta->data.stream_info.channels);
		printf("bps: %d\n", meta->data.stream_info.bits_per_sample);
		printf("total samples: %"PRIu64"\n", meta->data.stream_info.total_samples);

		if (player_setrate(sample_rate) == -1)
			err(1, "player_setrate");

		sio_stop(hdl);

		sio_initpar(&par);
		par.rate = sample_rate;
		if (!sio_setpar(hdl, &par))
			err(1, "sio_setpar");
		if (!sio_getpar(hdl, &par))
			err(1, "sio_getpar");
		/* TODO: check that there is a sane sample rate? */
		if (!sio_start(hdl))
			err(1, "sio_start");
	}
}

static void
errcb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status,
    void *data)
{
	warnx("error: %s", FLAC__StreamDecoderErrorStatusString[status]);
}

void
play_flac(int fd)
{
	FILE *f;
	int ok = 1;
	FLAC__StreamDecoder *decoder = NULL;
	FLAC__StreamDecoderInitStatus init_status;

	if ((f = fdopen(fd, "r")) == NULL)
		err(1, "fdopen");

	decoder = FLAC__stream_decoder_new();
	if (decoder == NULL)
		err(1, "flac stream decoder");

	FLAC__stream_decoder_set_md5_checking(decoder, 1);

	init_status = FLAC__stream_decoder_init_FILE(decoder, f, writecb,
	    metacb, errcb, NULL);
	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		errx(1, "flac decoder: %s",
		    FLAC__StreamDecoderInitStatusString[init_status]);

	ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
	warnx("decoding %s", ok ? "succeeded" : "failed");
	warnx("state: %s",
	    FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(decoder)]);

	FLAC__stream_decoder_delete(decoder);
}
