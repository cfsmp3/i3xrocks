/*
 * i3bar.c - i3bar (plus i3-gaps and sway) protocol support
 * Copyright (C) 2014-2019  Vivien Didelot
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

#include "bar.h"
#include "block.h"
#include "json.h"
#include "line.h"
#include "log.h"
#include "map.h"
#include "term.h"

/* See https://i3wm.org/docs/i3bar-protocol.html for details */

static struct {
	const char * const key;
	bool string;
} i3bar_keys[] = {
	{ "", false }, /* unknown key */

	/* Standard keys */
	{ "full_text", true },
	{ "short_text", true },
	{ "color", true },
	{ "background", true },
	{ "border", true },
	{ "min_width", false }, /* can also be a number */
	{ "align", true },
	{ "name", true },
	{ "instance", true },
	{ "urgent", false },
	{ "separator", false },
	{ "separator_block_width", false },
	{ "markup", true },

	/* i3-gaps features */
	{ "border_top", false },
	{ "border_bottom", false },
	{ "border_left", false },
	{ "border_right", false },
};

static unsigned int i3bar_indexof(const char *key)
{
	unsigned int i;

	for (i = 0; i < sizeof(i3bar_keys) / sizeof(i3bar_keys[0]); i++)
		if (strcmp(i3bar_keys[i].key, key) == 0)
			return i;

	return 0;
}

static int i3bar_line_cb(char *line, size_t len, size_t num, void *data)
{
	unsigned int index = num + 1;
	struct map *map = data;
	const char *key;

	if (index >= sizeof(i3bar_keys) / sizeof(i3bar_keys[0])) {
		debug("ignoring excess line %d: %s", num, line);
		return 0;
	}

	key = i3bar_keys[index].key;

	return map_setn(map, key, line, len);
}

/* Parse up to count complete lines buffered from a raw stream */
int i3bar_read(struct line *line, size_t count, struct map *map)
{
	return line_parse(line, count, i3bar_line_cb, map);
}

static void i3bar_print_term(const struct bar *bar)
{
	struct block *block = bar->blocks;
	const char *full_text;

	term_restore_cursor();

	while (block) {
		full_text = map_get(block->env, "full_text");
		if (full_text)
			fprintf(stdout, "%s ", full_text);

		block = block->next;
	}

	fflush(stdout);
}

/* Pairs of a block being printed */
struct i3bar_pairs {
	FILE *stream;
	unsigned int count;
};

/* Last status line printed */
static struct {
	char *buf;
	size_t len;
} i3bar_last;

static int i3bar_print_pair(const char *key, const char *value, size_t len,
			    void *data)
{
	unsigned int index = i3bar_indexof(key);
	bool string = i3bar_keys[index].string;
	struct i3bar_pairs *pairs = data;
	bool escape;

	/* Skip unknown keys */
	if (!index)
		return 0;

	if (!value) {
		value = "null";
		len = strlen(value);
	}

	if (string) {
		if (json_is_string(value, len))
			escape = false; /* Expected string already quoted */
		else
			escape = true; /* Enforce the string type */
	} else {
		if (json_is_valid(value, len))
			escape = false; /* Already valid JSON */
		else
			escape = true; /* Unquoted string */
	}

	if (pairs->count++)
		fprintf(pairs->stream, ",");

	fprintf(pairs->stream, "\"%s\":", key);

	/* Values of any size are streamed rather than formatted */
	if (escape)
		return json_escape(pairs->stream, value, len);

	fwrite(value, 1, len, pairs->stream);

	return 0;
}

static int i3bar_print_block(struct block *block, FILE *stream,
			     unsigned int *mcount)
{
	const char *full_text = map_get(block->env, "full_text");
	struct i3bar_pairs pairs = { .stream = stream };
	int err;

	/* "full_text" is the only mandatory key */
	if (!full_text) {
		block_debug(block, "no text to display, skipping");
		return 0;
	}

	if ((*mcount)++)
		fprintf(stream, ",");

	fprintf(stream, "{");
	err = map_for_each(block->env, i3bar_print_pair, &pairs);
	fprintf(stream, "}");

	return err;
}

 int i3bar_print(const struct bar *bar)
{
	struct block *block = bar->blocks;
	unsigned int mcount = 0;
	char *buf = NULL;
	size_t len = 0;
	FILE *stream;
	int err = 0;

	if (bar->term) {
		i3bar_print_term(bar);
		return 0;
	}

	/* Compose the whole status line first */
	stream = open_memstream(&buf, &len);
	if (!stream)
		return -ENOMEM;

	fprintf(stream, ",[");
	while (block) {
		err = i3bar_print_block(block, stream, &mcount);
		if (err)
			break;

		block = block->next;
	}
	fprintf(stream, "]\n");

	if (fclose(stream)) {
		free(buf);
		return -ENOMEM;
	}

	/* Status lines may be large, do not repeat an unchanged one */
	if (i3bar_last.buf && len == i3bar_last.len &&
	    memcmp(buf, i3bar_last.buf, len) == 0) {
		free(buf);
		return err;
	}

	fwrite(buf, 1, len, stdout);
	fflush(stdout);

	free(i3bar_last.buf);
	i3bar_last.buf = buf;
	i3bar_last.len = len;

	return err;
}

int i3bar_printf(struct block *block, int lvl, const char *msg)
{
	const struct bar *bar = block->bar;
	struct map *map = block->env;
	int err;

	if (bar->term || lvl > LOG_ERROR)
		return 0;

	block->tainted = true;

	err = map_set(map, "full_text", msg);
	if (err)
		return err;

	if (lvl <= LOG_ERROR) {
		err = map_set(map, "urgent", "true");
		if (err)
			return err;
	}

	return i3bar_print(bar);
}

int i3bar_start(struct bar *bar)
{
	if (bar->term) {
		term_save_cursor();
		term_restore_cursor();
	} else {
		fprintf(stdout, "{\"version\":1,\"click_events\":true}\n[[]\n");
		fflush(stdout);
	}

	return 0;
}

void i3bar_stop(struct bar *bar)
{
	if (bar->term) {
		term_reset_cursor();
	} else {
		fprintf(stdout, "]\n");
		fflush(stdout);
	}

	free(i3bar_last.buf);
	i3bar_last.buf = NULL;
	i3bar_last.len = 0;
}

static struct block *i3bar_find(struct bar *bar, const struct map *map)
{
	const char *block_name, *block_instance;
	const char *map_name, *map_instance;
	struct block *block = bar->blocks;

	/* "name" and "instance" are the only identifiers provided by i3bar */
	map_name = map_get(map, "name") ? : "";
	map_instance = map_get(map, "instance") ? : "";

	while (block) {
		block_name = block_get(block, "name") ? : "";
		block_instance = block_get(block, "instance") ? : "";

		if (strcmp(block_name, map_name) == 0 &&
		    strcmp(block_instance, map_instance) == 0)
			return block;

		block = block->next;
	}

	return NULL;
}

static int i3bar_click_block(struct bar *bar, const struct map *click)
{
	struct block *block;
	int err;

	/* Look for the corresponding block */
	block = i3bar_find(bar, click);
	if (!block)
		return 0;

	if (block->tainted) {
		err = block_reset(block);
		if (err)
			return err;

		block->tainted = false;

		return i3bar_print(bar);
	}

	err = map_copy(block->env, click);
	if (err)
		return err;

	return block_click(block);
}

int i3bar_click(struct bar *bar)
{
	struct map *click;
	int err;

	click = map_create();
	if (!click)
		return -ENOMEM;

	for (;;) {
		/* Each click is one JSON object per line */
		err = json_read(&bar->clicks, 1, click);
		if (err == -EAGAIN) {
			/* Keep a partial line until the rest of it arrives */
			err = line_fill(&bar->clicks, STDIN_FILENO);
			if (err) {
				if (err == -EAGAIN)
					err = 0;

				break;
			}

			continue;
		}

		if (err)
			break;

		err = i3bar_click_block(bar, click);
		if (err)
			break;

		map_clear(click);
	}

	map_destroy(click);

	return err;
}

int i3bar_setup(struct block *block)
{
	const char *instance = map_get(block->config, "instance");
	const char *name = map_get(block->config, "name");
	char buf[BUFSIZ];
	int err;

	/* A block needs a name to be clickable */
	if (!name) {
		name = "foo";
		err = map_set(block->config, "name", name);
		if (err)
			return err;
	}

	if (instance)
		snprintf(buf, sizeof(buf), "%s:%s", name, instance);
	else
		snprintf(buf, sizeof(buf), "%s", name);

	block->name = strdup(buf);
	if (!block->name)
		return -ENOMEM;

	return 0;
}
