/*
 * json.c - flat JSON parsing
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

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "line.h"
#include "log.h"
#include "map.h"
#include "sys.h"

/* Parse four hexadecimal digits, return false if they are invalid */
static bool json_parse_hex(const char *str, uint32_t *value)
{
	int hex;
	char c;
	int i;

	*value = 0;

	for (i = 0; i < 4; i++) {
		c = str[i];

		if (!isxdigit((unsigned char) c))
			return false;

		if (c >= '0' && c <= '9')
			hex = c - '0';
		else if (c >= 'a' && c <= 'f')
			hex = c - 'a' + 10;
		else
			hex = c - 'A' + 10;

		*value |= hex << (12 - i * 4);
	}

	return true;
}

/* Return the number of characters parsed after "\u", 0 if it is invalid.
 * The UTF-8 encoding is stored in utf8 and its number of bytes in len.
 */
static size_t json_parse_codepoint(const char *str, char *utf8, size_t *len)
{
	uint32_t codepoint, low;
	size_t parsed = 4;

	if (!json_parse_hex(str, &codepoint))
		return 0;

	/* Combine a UTF-16 surrogate pair, a lone surrogate is kept as is */
	if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
	    str[4] == '\\' && str[5] == 'u' && json_parse_hex(str + 6, &low) &&
	    low >= 0xdc00 && low <= 0xdfff) {
		codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
			    (low - 0xdc00);
		parsed = 10;
	}

	if (codepoint <= 0x7f) {
		*len = 1;
		utf8[0] = codepoint;
	} else if (codepoint <= 0x7ff) {
		*len = 2;
		utf8[0] = 0xc0 | (codepoint >> 6);
		utf8[1] = 0x80 | (codepoint & 0x3f);
	} else if (codepoint <= 0xffff) {
		*len = 3;
		utf8[0] = 0xe0 | (codepoint >> 12);
		utf8[1] = 0x80 | ((codepoint >> 6) & 0x3f);
		utf8[2] = 0x80 | (codepoint & 0x3f);
	} else {
		*len = 4;
		utf8[0] = 0xf0 | (codepoint >> 18);
		utf8[1] = 0x80 | ((codepoint >> 12) & 0x3f);
		utf8[2] = 0x80 | ((codepoint >> 6) & 0x3f);
		utf8[3] = 0x80 | (codepoint & 0x3f);
	}

	return parsed;
}

/* Return the number of characters parsed after a backslash, 0 if it is invalid */
static size_t json_parse_escape(const char *str, char *utf8, size_t *len)
{
	size_t parsed;

	*len = 1;

	switch (*str) {
	case '"':
	case '\\':
	case '/':
		utf8[0] = *str;
		break;
	case 'b':
		utf8[0] = '\b';
		break;
	case 'f':
		utf8[0] = '\f';
		break;
	case 'n':
		utf8[0] = '\n';
		break;
	case 'r':
		utf8[0] = '\r';
		break;
	case 't':
		utf8[0] = '\t';
		break;
	case 'u':
		parsed = json_parse_codepoint(str + 1, utf8, len);
		if (!parsed)
			return 0;

		return parsed + 1;
	default:
		return 0;
	}

	return 1;
}

/* Return the length of the parsed string, 0 if it is invalid.
 * The decoded string is null-terminated in buf, but it may contain null
 * bytes itself, so its length (if requested) is stored in len.
 */
static size_t json_parse_string(const char *str, char *buf, size_t size,
				size_t *len)
{
	const char *end = str + 1;
	size_t count = 0;
	size_t parsed;
	char utf8[4];
	size_t n;

	if (*str != '"')
		return 0;

	while (*end != '"') {
		/* control character or end-of-line? */
		if (iscntrl((unsigned char) *end))
			return 0;

		if (*end == '\\') {
			parsed = json_parse_escape(end + 1, utf8, &n);
			if (!parsed)
				return 0;

			end += parsed + 1;
		} else {
			utf8[0] = *end++;
			n = 1;
		}

		if (buf) {
			/* Keep room for the terminating null byte */
			if (size - count <= n)
				return 0;

			memcpy(buf + count, utf8, n);
		}

		count += n;
	}

	if (buf) {
		if (count >= size)
			return 0;

		buf[count] = '\0';
	}

	if (len)
		*len = count;

	return ++end - str;
}

/* Return the length of the parsed non scalar (with open/close delimiter), 0 if it is invalid */
static size_t json_parse_nested_struct(const char *str, char open, char close,
				       char *buf, size_t size)
{
	const char *end = str;
	size_t len;
	int nested;

	if (*str != open)
		return 0;

	nested = 1;
	while (nested) {
		++end;

		/* control character or end-of-line? */
		if (iscntrl(*end) || *end == '\0')
			return 0;

		if (*end == open)
			nested++;
		else if (*end == close)
			nested--;
	}

	len = ++end - str;
	if (!len)
		return 0;

	if (buf) {
		if (size <= len)
			return 0;

		strncpy(buf, str, len);
		buf[len] = '\0';
	}

	return len;
}

/* Return the length of the parsed array, 0 if it is invalid */
static size_t json_parse_nested_array(const char *str, char *buf, size_t size)
{
	return json_parse_nested_struct(str, '[', ']', buf, size);
}

/* Return the length of the parsed object, 0 if it is invalid */
static size_t json_parse_nested_object(const char *str, char *buf, size_t size)
{
	return json_parse_nested_struct(str, '{', '}', buf, size);
}

/* Return the length of the parsed number, 0 if it is invalid */
static size_t json_parse_number(const char *str, char *buf, size_t size)
{
	size_t len;
	char *end;

	strtoul(str, &end, 10);

	len = end - str;
	if (!len)
		return 0;

	if (buf) {
		if (size <= len)
			return 0;

		strncpy(buf, str, len);
		buf[len] = '\0';
	}

	return len;
}

/* Return the length of the parsed literal, 0 if it is invalid */
static size_t json_parse_literal(const char *str, const char *literal,
				 char *buf, size_t size)
{
	const size_t len = strlen(literal);

	if (strncmp(str, literal, len) != 0)
		return 0;

	if (buf) {
		if (size <= len)
			return 0;

		memcpy(buf, literal, len + 1);
	}

	return len;
}

/* A value can be a string, number, object, array, true, false, or null.
 * Strings are decoded, other values are copied verbatim; the length of
 * the result (if requested) is stored in vallen.
 */
static size_t json_parse_value(const char *str, char *buf, size_t size,
			       size_t *vallen)
{
	size_t len;

	len = json_parse_string(str, buf, size, vallen);
	if (len)
		return len;

	len = json_parse_number(str, buf, size);
	if (!len)
		len = json_parse_nested_object(str, buf, size);
	if (!len)
		len = json_parse_nested_array(str, buf, size);
	if (!len)
		len = json_parse_literal(str, "true", buf, size);
	if (!len)
		len = json_parse_literal(str, "false", buf, size);
	if (!len)
		len = json_parse_literal(str, "null", buf, size);

	if (len && vallen)
		*vallen = len;

	return len;
}

/* Return the length of a separator optionally enclosed by whitespaces, 0 otherwise */
static size_t json_parse_sep(const char *str, char sep)
{
	size_t len = 0;

	while (isspace(*str))
		str++, len++;

	if (*str != sep)
		return 0;

	str++;
	len++;

	while (isspace(*str))
		str++, len++;

	return len;
}

/* Parse an inline ["name"][\s+:\s+][value] name-value pair */
static size_t json_parse_pair(const char *str, char *name, size_t namesiz,
			      char *val, size_t valsiz, size_t *vallen)
{
	size_t pair_len = 0;
	size_t len;

	len = json_parse_string(str, name, namesiz, NULL);
	if (!len)
		return 0;

	pair_len += len;
	str += len;

	len = json_parse_sep(str, ':');
	if (!len)
		return 0;

	pair_len += len;
	str += len;

	len = json_parse_value(str, val, valsiz, vallen);
	if (!len)
		return 0;

	pair_len += len;

	return pair_len;
}

static int json_line_cb(char *line, size_t size, size_t num, void *data)
{
	struct map *map = data;
	char *name, *val;
	size_t vallen;
	size_t len;
	int err = 0;

	/* A decoded name or value is never longer than its line */
	name = malloc(size + 1);
	val = malloc(size + 1);
	if (!name || !val) {
		err = -ENOMEM;
		goto out;
	}

	for (;;) {
		/* Only support inline flattened structures at the moment */
		while (*line == '[' || *line == ']' || *line == ',' ||
		       *line == '{' || *line == '}' || isspace(*line))
			line++;

		if (*line == '\0')
			break;

		len = json_parse_pair(line, name, size + 1, val, size + 1,
				      &vallen);
		if (!len) {
			err = -EINVAL;
			break;
		}

		line += len;

		/* valid delimiters after a pair */
		if (*line != ',' && *line != '}' && *line != '\0' &&
		    !isspace(*line)) {
			err = -EINVAL;
			break;
		}

		if (*line != '\0')
			line++;

		if (map) {
			err = map_setn(map, name, val, vallen);
			if (err)
				break;
		}
	}

out:
	free(name);
	free(val);

	return err;
}

/* Parse up to count complete lines buffered from a JSON stream */
int json_read(struct line *line, size_t count, struct map *map)
{
	return line_parse(line, count, json_line_cb, map);
}

bool json_is_string(const char *str, size_t len)
{
	if (!len)
		return false;

	return json_parse_string(str, NULL, 0, NULL) == len;
}

bool json_is_valid(const char *str, size_t len)
{
	if (!len)
		return false;

	return json_parse_value(str, NULL, 0, NULL) == len;
}

/* Print len bytes of str as a JSON string, whatever they contain */
int json_escape(FILE *stream, const char *str, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	const char *end = str + len;
	const char *plain = str;
	char escape[7];
	unsigned char c;

	fputc('"', stream);

	for (; str < end; str++) {
		c = *str;

		switch (c) {
		case '\b':
			strcpy(escape, "\\b");
			break;
		case '\f':
			strcpy(escape, "\\f");
			break;
		case '\n':
			strcpy(escape, "\\n");
			break;
		case '\r':
			strcpy(escape, "\\r");
			break;
		case '\t':
			strcpy(escape, "\\t");
			break;
		case '\\':
			strcpy(escape, "\\\\");
			break;
		case '"':
			strcpy(escape, "\\\"");
			break;
		default:
			if (!iscntrl(c))
				continue;

			/* \u00XX, as control characters are single bytes */
			strcpy(escape, "\\u00");
			escape[4] = hex[c >> 4];
			escape[5] = hex[c & 0xf];
			escape[6] = '\0';
			break;
		}

		/* Flush the characters which need no escaping */
		fwrite(plain, 1, str - plain, stream);
		plain = str + 1;

		fputs(escape, stream);
	}

	fwrite(plain, 1, end - plain, stream);
	fputc('"', stream);

	return ferror(stream) ? -EIO : 0;
}
