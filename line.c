/*
 * line.c - generic line parser
 * Copyright (C) 2015-2019  Vivien Didelot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "line.h"
#include "log.h"
#include "sys.h"

/* Minimum free space offered to each read */
#define LINE_CHUNK	BUFSIZ

/* Make room for up to LINE_CHUNK more bytes at the end of the buffer,
 * -ENOBUFS if LINE_SIZE_MAX bytes are already buffered.
 */
static int line_reserve(struct line *line)
{
	size_t size;
	char *buf;

	/* Drop the lines already parsed */
	if (line->start) {
		memmove(line->buf, line->buf + line->start,
			line->len - line->start);
		line->len -= line->start;
		line->start = 0;
	}

	/* Give back the room a long line needed once it is consumed */
	if (!line->len && line->size > LINE_CHUNK) {
		free(line->buf);
		line->buf = NULL;
		line->size = 0;
	}

	if (line->len >= LINE_SIZE_MAX)
		return -ENOBUFS;

	if (line->size - line->len >= LINE_CHUNK)
		return 0;

	size = line->size ? line->size * 2 : LINE_CHUNK;
	while (size - line->len < LINE_CHUNK)
		size *= 2;

	if (size > LINE_SIZE_MAX)
		size = LINE_SIZE_MAX;

	buf = realloc(line->buf, size);
	if (!buf)
		return -ENOMEM;

	line->buf = buf;
	line->size = size;

	return 0;
}

/* Append a single read to the buffer, -EAGAIN if nothing is available.
 * Nothing is read once LINE_SIZE_MAX bytes are buffered: -ENOBUFS.
 */
int line_fill(struct line *line, int fd)
{
	size_t count;
	int err;

	err = line_reserve(line);
	if (err)
		return err;

	err = sys_read(fd, line->buf + line->len, line->size - line->len,
		       &count);
	if (err)
		return err;

	line->len += count;

	return 0;
}

/* Return the end of the next complete line, NULL if it is not there yet */
static char *line_end(const struct line *line)
{
	if (line->start == line->len)
		return NULL;

	return memchr(line->buf + line->start, '\n', line->len - line->start);
}

bool line_complete(const struct line *line)
{
	return line_end(line) != NULL;
}

/* Pop the next complete line, excluding its newline character */
static int line_next(struct line *line, char **str, size_t *len)
{
	char *end = line_end(line);

	if (!end)
		return -EAGAIN;

	/* replace newline with terminating null byte */
	*end = '\0';

	*str = line->buf + line->start;
	*len = end - *str;
	line->start += *len + 1;

	return 0;
}

/* Parse up to count buffered complete lines, -EAGAIN if fewer were there */
int line_parse(struct line *line, size_t count, line_cb_t *cb, void *data)
{
	size_t num = 0;
	size_t len;
	char *str;
	int err;

	while (count--) {
		err = line_next(line, &str, &len);
		if (err)
			return err;

		debug("%.3zu: %s", num, str);

		if (cb) {
			err = cb(str, len, num, data);
			if (err)
				return err;
		}

		num++;
	}

	return 0;
}

void line_clear(struct line *line)
{
	free(line->buf);
	memset(line, 0, sizeof(*line));
}

/* Read up to count lines excluding their newline character */
int line_read(int fd, size_t count, line_cb_t *cb, void *data)
{
	struct line line = { 0 };
	int err;

	while (count) {
		err = line_parse(&line, 1, cb, data);
		if (err == -EAGAIN)
			err = line_fill(&line, fd);
		else if (!err)
			count--;

		if (err)
			break;
	}

	line_clear(&line);

	return count ? err : 0;
}
