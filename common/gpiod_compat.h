/*
 * Author: Jason Yang <jason98166@gmail.com>
 * Date: 2026-10-14
 *
 * One GPIO API over libgpiod 1.x and 2.x, picked from gpiod.h.
 */

#ifndef GPIOD_COMPAT_H
#define GPIOD_COMPAT_H

#include <dirent.h>
#include <errno.h>
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gpio_chip;
struct gpio_line;

static inline struct gpio_chip *gpio_chip_open(const char *name_or_path);
static inline struct gpio_chip *gpio_chip_open_by_label(const char *label);
/* release the lines first */
static inline void gpio_chip_close(struct gpio_chip *chip);

static inline struct gpio_line *gpio_line_request_output(struct gpio_chip *chip,
							 unsigned int offset,
							 const char *consumer,
							 int value);
static inline struct gpio_line *gpio_line_request_input(struct gpio_chip *chip,
							unsigned int offset,
							const char *consumer);
static inline int gpio_line_set_value(struct gpio_line *line, int value);
/* 0 or 1, -1 on error */
static inline int gpio_line_get_value(struct gpio_line *line);
static inline void gpio_line_release(struct gpio_line *line);

struct gpio_chip {
	struct gpiod_chip *chip;
};

static inline struct gpio_chip *gpio_chip_wrap(struct gpiod_chip *c)
{
	struct gpio_chip *chip;

	if (!c)
		return NULL;
	chip = malloc(sizeof(*chip));
	if (!chip) {
		gpiod_chip_close(c);
		return NULL;
	}
	chip->chip = c;
	return chip;
}

static inline void gpio_chip_close(struct gpio_chip *chip)
{
	if (!chip)
		return;
	gpiod_chip_close(chip->chip);
	free(chip);
}

#ifdef GPIOD_LINE_BULK_MAX_LINES /* 1.x only */

struct gpio_line {
	struct gpiod_line *line;
};

static inline struct gpio_chip *gpio_chip_open(const char *name_or_path)
{
	return gpio_chip_wrap(gpiod_chip_open_lookup(name_or_path));
}

static inline struct gpio_chip *gpio_chip_open_by_label(const char *label)
{
	return gpio_chip_wrap(gpiod_chip_open_by_label(label));
}

static inline struct gpio_line *gpio_line_wrap(struct gpiod_line *l)
{
	struct gpio_line *line = malloc(sizeof(*line));

	if (!line) {
		gpiod_line_release(l);
		return NULL;
	}
	line->line = l;
	return line;
}

static inline struct gpio_line *gpio_line_request_output(struct gpio_chip *chip,
							 unsigned int offset,
							 const char *consumer,
							 int value)
{
	struct gpiod_line *l = gpiod_chip_get_line(chip->chip, offset);

	if (!l || gpiod_line_request_output(l, consumer, value) < 0)
		return NULL;
	return gpio_line_wrap(l);
}

static inline struct gpio_line *gpio_line_request_input(struct gpio_chip *chip,
							unsigned int offset,
							const char *consumer)
{
	struct gpiod_line *l = gpiod_chip_get_line(chip->chip, offset);

	if (!l || gpiod_line_request_input(l, consumer) < 0)
		return NULL;
	return gpio_line_wrap(l);
}

static inline int gpio_line_set_value(struct gpio_line *line, int value)
{
	return gpiod_line_set_value(line->line, value);
}

static inline int gpio_line_get_value(struct gpio_line *line)
{
	return gpiod_line_get_value(line->line);
}

static inline void gpio_line_release(struct gpio_line *line)
{
	if (!line)
		return;
	gpiod_line_release(line->line);
	free(line);
}

#else /* libgpiod 2.x */

struct gpio_line {
	struct gpiod_line_request *req;
	unsigned int offset;
};

static inline struct gpio_chip *gpio_chip_open(const char *name_or_path)
{
	char path[64];

	if (name_or_path[0] != '/') {
		snprintf(path, sizeof(path), "/dev/%s", name_or_path);
		name_or_path = path;
	}
	return gpio_chip_wrap(gpiod_chip_open(name_or_path));
}

static inline struct gpio_chip *gpio_chip_open_by_label(const char *label)
{
	DIR *dev = opendir("/dev");
	struct dirent *ent;
	struct gpiod_chip *c = NULL;

	if (!dev)
		return NULL;
	while (!c && (ent = readdir(dev))) {
		char path[sizeof("/dev/") + sizeof(ent->d_name)];
		struct gpiod_chip_info *info;

		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
		if (!gpiod_is_gpiochip_device(path))
			continue;
		c = gpiod_chip_open(path);
		if (!c)
			continue;
		info = gpiod_chip_get_info(c);
		if (!info || strcmp(gpiod_chip_info_get_label(info), label) != 0) {
			gpiod_chip_close(c);
			c = NULL;
		}
		if (info)
			gpiod_chip_info_free(info);
	}
	closedir(dev);
	if (!c)
		errno = ENOENT;
	return gpio_chip_wrap(c);
}

static inline struct gpio_line *gpio_line_request(struct gpio_chip *chip,
						  unsigned int offset,
						  const char *consumer,
						  enum gpiod_line_direction dir,
						  int value)
{
	struct gpiod_line_settings *settings = gpiod_line_settings_new();
	struct gpiod_line_config *line_cfg = gpiod_line_config_new();
	struct gpiod_request_config *req_cfg = gpiod_request_config_new();
	struct gpiod_line_request *req = NULL;
	struct gpio_line *line = NULL;

	if (!settings || !line_cfg || !req_cfg)
		goto out;
	gpiod_line_settings_set_direction(settings, dir);
	if (dir == GPIOD_LINE_DIRECTION_OUTPUT)
		gpiod_line_settings_set_output_value(settings,
						     value ? GPIOD_LINE_VALUE_ACTIVE
							   : GPIOD_LINE_VALUE_INACTIVE);
	gpiod_request_config_set_consumer(req_cfg, consumer);
	if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) < 0)
		goto out;
	req = gpiod_chip_request_lines(chip->chip, req_cfg, line_cfg);
	if (!req)
		goto out;
	line = malloc(sizeof(*line));
	if (!line) {
		gpiod_line_request_release(req);
		goto out;
	}
	line->req = req;
	line->offset = offset;
out:
	if (req_cfg)
		gpiod_request_config_free(req_cfg);
	if (line_cfg)
		gpiod_line_config_free(line_cfg);
	if (settings)
		gpiod_line_settings_free(settings);
	return line;
}

static inline struct gpio_line *gpio_line_request_output(struct gpio_chip *chip,
							 unsigned int offset,
							 const char *consumer,
							 int value)
{
	return gpio_line_request(chip, offset, consumer,
				 GPIOD_LINE_DIRECTION_OUTPUT, value);
}

static inline struct gpio_line *gpio_line_request_input(struct gpio_chip *chip,
							unsigned int offset,
							const char *consumer)
{
	return gpio_line_request(chip, offset, consumer,
				 GPIOD_LINE_DIRECTION_INPUT, 0);
}

static inline int gpio_line_set_value(struct gpio_line *line, int value)
{
	return gpiod_line_request_set_value(line->req, line->offset,
					    value ? GPIOD_LINE_VALUE_ACTIVE
						  : GPIOD_LINE_VALUE_INACTIVE);
}

static inline int gpio_line_get_value(struct gpio_line *line)
{
	enum gpiod_line_value v = gpiod_line_request_get_value(line->req,
							       line->offset);

	if (v == GPIOD_LINE_VALUE_ERROR)
		return -1;
	return v == GPIOD_LINE_VALUE_ACTIVE;
}

static inline void gpio_line_release(struct gpio_line *line)
{
	if (!line)
		return;
	gpiod_line_request_release(line->req);
	free(line);
}

#endif /* libgpiod 2.x */

#endif /* GPIOD_COMPAT_H */
