#define _GNU_SOURCE

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <wayland-client.h>

#include "detect.h"

#define WAYFETCH_VERSION "0.1"

struct output_info {
	int32_t width;
	int32_t height;
	int32_t refresh;      /* mHz */
	int32_t transform;
	int32_t phys_w;       /* mm */
	int32_t phys_h;       /* mm */
	int32_t scale;
	int have_current;
	int have_mode;
	char name[64];
	char desc[96];
};

struct output {
	struct wl_list link;
	struct wl_output *wl_output;
	uint32_t version;
	struct output_info info;
};

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_list outputs;
	int have_output;
	int have_xwayland_shell;
	char **protocols;     /* unique wl_registry interface names */
	int nprotocols;
	int capprotocols;
};

static void
output_geometry(void *data, struct wl_output *output,
                int32_t x, int32_t y, int32_t physical_width,
                int32_t physical_height, int32_t subpixel,
                const char *make, const char *model, int32_t transform)
{
	struct output *out = data;

	(void)output;
	(void)x;
	(void)y;
	(void)subpixel;

	out->info.phys_w = physical_width;
	out->info.phys_h = physical_height;
	out->info.transform = transform;

	if (!out->info.desc[0] && make && model)
		snprintf(out->info.desc, sizeof(out->info.desc), "%s %s",
		         make, model);
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
	struct output *out = data;

	(void)output;

	/* Some compositors never set CURRENT: keep any mode as a fallback. */
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		out->info.have_current = 1;
	} else if (out->info.have_current) {
		return;
	}

	out->info.width = width;
	out->info.height = height;
	out->info.refresh = refresh;
	out->info.have_mode = 1;
}

static void
output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	struct output *out = data;

	(void)output;
	out->info.scale = factor;
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
	struct output *out = data;

	(void)output;
	snprintf(out->info.name, sizeof(out->info.name), "%s", name);
}

static void
output_description(void *data, struct wl_output *output,
                   const char *description)
{
	struct output *out = data;

	(void)output;
	snprintf(out->info.desc, sizeof(out->info.desc), "%s", description);
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
add_protocol(struct app *app, const char *name)
{
	char *copy;
	int i;

	for (i = 0; i < app->nprotocols; i++)
		if (strcmp(app->protocols[i], name) == 0)
			return;

	if (app->nprotocols == app->capprotocols) {
		int cap = app->capprotocols ? app->capprotocols * 2 : 64;
		char **p = realloc(app->protocols, (size_t)cap * sizeof(*p));

		if (!p)
			return;
		app->protocols = p;
		app->capprotocols = cap;
	}

	copy = strndup(name, 100);
	if (copy)
		app->protocols[app->nprotocols++] = copy;
}

static int
compare_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
free_protocols(struct app *app)
{
	int i;

	for (i = 0; i < app->nprotocols; i++)
		free(app->protocols[i]);
	free(app->protocols);
	app->protocols = NULL;
	app->nprotocols = app->capprotocols = 0;
}

static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version)
{
	struct app *app = data;
	struct output *out;
	uint32_t bind_version;

	add_protocol(app, interface);

	if (strcmp(interface, "xwayland_shell_v1") == 0)
		app->have_xwayland_shell = 1;

	if (strcmp(interface, wl_output_interface.name) != 0)
		return;

	out = calloc(1, sizeof(*out));
	if (!out)
		return;

	bind_version = version < 4 ? version : 4;

	out->wl_output = wl_registry_bind(registry, name,
	                                  &wl_output_interface,
	                                  bind_version);
	if (!out->wl_output) {
		free(out);
		return;
	}

	out->version = bind_version;
	wl_list_insert(app->outputs.prev, &out->link);
	wl_output_add_listener(out->wl_output, &output_listener, out);
	app->have_output = 1;
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
destroy_outputs(struct app *app)
{
	struct output *out, *tmp;

	wl_list_for_each_safe(out, tmp, &app->outputs, link) {
		if (out->version >= 3)
			wl_output_release(out->wl_output);
		else
			wl_proxy_destroy((struct wl_proxy *)out->wl_output);

		wl_list_remove(&out->link);
		free(out);
	}
}

static const char *const logo[] = {
	"      ........      ",
	"    .:---------.    ",
	"  ..#.--:------:#.  ",
	" .-:##:--::---:##-. ",
	".--:##.-:##:--.#:-- ",
	".-:-##.:#..#-:#.---.",
	".---.###.-:#..#.---.",
	" .--:###:--.###:--. ",
	"  .--.##---:##:--.  ",
	"   .:-::----::-:.   ",
	"      ...::...      ",
};

#define LOGO_N ((int)(sizeof(logo) / sizeof(logo[0])))
#define LOGO_GAP 3
#define MAX_LINES 128

static int logo_w;

static void
init_logo(void)
{
	int i;

	for (i = 0; i < LOGO_N; i++) {
		int l = (int)strlen(logo[i]);

		if (l > logo_w)
			logo_w = l;
	}
}

struct line {
	char label[64];
	char value[256];
	int cont;         /* continuation of the previous line */
	int indent;
};

static struct line lines[MAX_LINES];
static int nlines;

static void
add_line(const char *label, const char *fmt, ...)
{
	va_list ap;
	struct line *l;

	if (nlines >= MAX_LINES)
		return;

	l = &lines[nlines++];
	snprintf(l->label, sizeof(l->label), "%s", label);

	va_start(ap, fmt);
	vsnprintf(l->value, sizeof(l->value), fmt, ap);
	va_end(ap);
}

static void
add_cont(int indent, const char *text)
{
	struct line *l;

	if (nlines >= MAX_LINES)
		return;

	l = &lines[nlines++];
	l->cont = 1;
	l->indent = indent;
	snprintf(l->value, sizeof(l->value), "%s", text);
}

/* "label: a, b, c" wrapped to the terminal width */
static void
add_list(const char *label, char *const *items, int n, int width)
{
	int indent = (int)strlen(label) + 2;
	int limit = width - indent;
	char buf[256];
	size_t len = 0;
	int first = 1, i;

	if (limit < 20)
		limit = 20;
	if (limit > 200)
		limit = 200;

	if (n == 0) {
		add_line(label, "unknown");
		return;
	}

	buf[0] = '\0';
	for (i = 0; i < n; i++) {
		size_t il = strlen(items[i]);
		size_t need = il + (i < n - 1 ? 1 : 0);

		if (len && len + 1 + need > (size_t)limit) {
			if (first)
				add_line(label, "%s", buf);
			else
				add_cont(indent, buf);
			first = 0;
			len = 0;
			buf[0] = '\0';
		}

		if (len)
			buf[len++] = ' ';
		memcpy(buf + len, items[i], il);
		len += il;
		if (i < n - 1)
			buf[len++] = ',';
		buf[len] = '\0';
	}

	if (first)
		add_line(label, "%s", buf);
	else
		add_cont(indent, buf);
}

static int
term_cols(void)
{
	struct winsize ws;
	const char *c;

	if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_col > 0)
		return ws.ws_col;

	c = getenv("COLUMNS");
	if (c && atoi(c) > 0)
		return atoi(c);

	return 80;
}

static const char *
env_nonempty(const char *name)
{
	const char *v = getenv(name);

	return v && *v ? v : NULL;
}

static void
output_size(const struct output_info *in, int *w, int *h)
{
	*w = in->width;
	*h = in->height;

	if (in->transform == WL_OUTPUT_TRANSFORM_90 ||
	    in->transform == WL_OUTPUT_TRANSFORM_270 ||
	    in->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
	    in->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		int tmp = *w;
		*w = *h;
		*h = tmp;
	}
}

static int
count_outputs(const struct app *app)
{
	const struct output *out;
	int n = 0;

	wl_list_for_each(out, &app->outputs, link)
		if (out->info.have_mode)
			n++;

	return n;
}

static void
add_monitors(const struct app *app)
{
	const struct output *out;
	int total = count_outputs(app);
	int idx = 0;

	if (total == 0) {
		add_line("resolution", "unknown");
		return;
	}

	wl_list_for_each(out, &app->outputs, link) {
		const struct output_info *in = &out->info;
		char label[96], value[200];
		size_t off;
		int w, h;

		if (!in->have_mode)
			continue;

		idx++;
		output_size(in, &w, &h);

		if (total == 1) {
			snprintf(label, sizeof(label), "resolution");
		} else if (in->name[0]) {
			snprintf(label, sizeof(label), "resolution (%s)", in->name);
		} else {
			snprintf(label, sizeof(label), "resolution (#%d)", idx);
		}

		off = (size_t)snprintf(value, sizeof(value), "%dx%d", w, h);

		if (in->refresh > 0)
			off += (size_t)snprintf(value + off, sizeof(value) - off,
			                        " @ %d Hz",
			                        (in->refresh + 500) / 1000);

		if (in->scale > 1)
			off += (size_t)snprintf(value + off, sizeof(value) - off,
			                        ", scale %d", in->scale);

		if (in->phys_w > 0 && in->phys_h > 0) {
			double diag_mm = sqrt((double)in->phys_w * in->phys_w +
			                      (double)in->phys_h * in->phys_h);

			snprintf(value + off, sizeof(value) - off,
			         ", %.1f\"", diag_mm / 25.4);
		}

		add_line(label, "%s", value);
	}
}

static int
use_color(void)
{
	return isatty(STDOUT_FILENO) && !env_nonempty("NO_COLOR");
}

static void
print_line(const struct line *l, const char *c_lab, const char *c_off)
{
	if (l->cont)
		printf("%*s%s", l->indent, "", l->value);
	else if (l->label[0])
		printf("%s%s:%s %s", c_lab, l->label, c_off, l->value);
	else
		printf("%s%s%s", c_lab, l->value, c_off);
}

/* '#' is the white fill of the W, everything else is yellow */
static void
print_logo_row(const char *row, int color)
{
	const char *yellow = "\033[1;33m", *white = "\033[1;37m";
	int cur = 0, i, len = (int)strlen(row);

	for (i = 0; i < len; i++) {
		int want = row[i] == '#' ? 2 : 1;

		if (color && want != cur) {
			fputs(want == 2 ? white : yellow, stdout);
			cur = want;
		}
		putchar(row[i]);
	}

	if (color && cur)
		fputs("\033[0m", stdout);

	for (i = len; i < logo_w; i++)
		putchar(' ');
}

static void
print_all(int color, int vertical, int show_logo)
{
	const char *c_lab = color ? "\033[1;36m" : "";
	const char *c_off = color ? "\033[0m" : "";
	int r;

	if (!show_logo) {
		for (r = 0; r < nlines; r++) {
			print_line(&lines[r], c_lab, c_off);
			putchar('\n');
		}
		return;
	}

	if (vertical) {
		for (r = 0; r < LOGO_N; r++) {
			print_logo_row(logo[r], color);
			putchar('\n');
		}
		putchar('\n');

		for (r = 0; r < nlines; r++) {
			print_line(&lines[r], c_lab, c_off);
			putchar('\n');
		}
		return;
	}

	{
		int rows = LOGO_N > nlines ? LOGO_N : nlines;
		int info_off = LOGO_N > nlines ? (LOGO_N - nlines) / 2 : 0;

		for (r = 0; r < rows; r++) {
			int ii = r - info_off;

			if (r < LOGO_N)
				print_logo_row(logo[r], color);
			else
				printf("%*s", logo_w, "");

			if (ii >= 0 && ii < nlines) {
				printf("%*s", LOGO_GAP, "");
				print_line(&lines[ii], c_lab, c_off);
			}

			putchar('\n');
		}
	}
}

static int
wayland_error(struct app *app)
{
	destroy_outputs(app);
	free_protocols(app);

	if (app->registry)
		wl_registry_destroy(app->registry);

	if (app->display)
		wl_display_disconnect(app->display);

	puts("Wayfetch needs wayland :3");
	return 1;
}

int
main(int argc, char **argv)
{
	struct app app = {0};
	struct compositor_info ci;
	const char *wc, *lib;
	char lib_buf[300];
	int vertical = 0, show_logo = 1, show_protocols = 0;
	int xwayland, width, i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int k;

		if (a[0] != '-' || !a[1]) {
			fprintf(stderr, "wayfetch: unknown option '%s'\n"
			        "usage: wayfetch [-u] [-n] [-p] [-v]\n", a);
			return 1;
		}

		for (k = 1; a[k]; k++) {
			switch (a[k]) {
			case 'v':
				printf("wayfetch %s\n", WAYFETCH_VERSION);
				return 0;
			case 'u':
				vertical = 1;
				break;
			case 'n':
				show_logo = 0;
				break;
			case 'p':
				show_protocols = 1;
				break;
			default:
				fprintf(stderr, "wayfetch: unknown option '-%c'\n"
				        "usage: wayfetch [-u] [-n] [-p] [-v]\n", a[k]);
				return 1;
			}
		}
	}

	wl_list_init(&app.outputs);

	app.display = wl_display_connect(NULL);
	if (!app.display)
		return wayland_error(&app);

	app.registry = wl_display_get_registry(app.display);
	if (!app.registry)
		return wayland_error(&app);

	wl_registry_add_listener(app.registry, &registry_listener, &app);

	if (wl_display_roundtrip(app.display) < 0)
		return wayland_error(&app);

	if (app.have_output && wl_display_roundtrip(app.display) < 0)
		return wayland_error(&app);

	detect_compositor(wl_display_get_fd(app.display), &ci);

	wc = env_nonempty("WAYFETCH_WC");
	if (!wc)
		wc = ci.name[0] ? ci.name : NULL;
	if (!wc)
		wc = env_nonempty("XDG_CURRENT_DESKTOP");
	if (!wc)
		wc = env_nonempty("XDG_SESSION_DESKTOP");

	lib = env_nonempty("WAYFETCH_LIB");
	if (!lib && ci.libs[0]) {
		snprintf(lib_buf, sizeof(lib_buf), "%s%s", ci.libs,
		         ci.guessed ? " (guess)" : "");
		lib = lib_buf;
	}

	/* Xwayland: process/socket first, then the global, then $DISPLAY */
	if (ci.xwayland == 1 || app.have_xwayland_shell)
		xwayland = 1;
	else if (ci.xwayland == 0)
		xwayland = 0;
	else
		xwayland = env_nonempty("DISPLAY") != NULL;

	init_logo();
	width = term_cols();
	if (show_logo && !vertical)
		width -= logo_w + LOGO_GAP;

	qsort(app.protocols, (size_t)app.nprotocols, sizeof(*app.protocols),
	      compare_names);

	add_line("", "wayland&wayland");
	add_line("", "---------------");
	add_line("wc", "%s", wc ? wc : "unknown");
	add_line("lib", "%s", lib ? lib : "unknown");
	if (show_protocols)
		add_list("protocols", app.protocols, app.nprotocols, width);
	add_line("xwayland", "%s", xwayland ? "true" : "false");
	add_monitors(&app);

	print_all(use_color(), vertical, show_logo);

	destroy_outputs(&app);
	free_protocols(&app);
	wl_registry_destroy(app.registry);
	wl_display_disconnect(app.display);

	return 0;
}
