/*
 * line.h - generic line parsing header
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

#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <unistd.h>

/* Unparsed input buffered at most, far beyond any sensible status */
#define LINE_SIZE_MAX	(16 * 1024 * 1024)

/* Input buffered until complete lines are available, whatever their size */
struct line {
	char *buf;
	size_t start;	/* beginning of the unparsed data */
	size_t len;	/* end of the buffered data */
	size_t size;	/* allocated size */
};

/* A line may contain null bytes, len excludes the terminating one */
typedef int line_cb_t(char *line, size_t len, size_t num, void *data);

int line_fill(struct line *line, int fd);
bool line_complete(const struct line *line);
int line_parse(struct line *line, size_t count, line_cb_t *cb, void *data);
void line_clear(struct line *line);

int line_read(int fd, size_t count, line_cb_t *cb, void *data);

#endif /* IO_H */
