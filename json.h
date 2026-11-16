/*
 * json.h - JSON manipulation header
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

#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stdio.h>

struct line;
struct map;

int json_read(struct line *line, size_t count, struct map *map);

bool json_is_string(const char *str, size_t len);
bool json_is_valid(const char *str, size_t len);
int json_escape(FILE *stream, const char *str, size_t len);

#endif /* JSON_H */

