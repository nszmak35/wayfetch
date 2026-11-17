#define _GNU_SOURCE

#include "detect.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LIBS 12
#define SCAN_LIMIT (512L * 1024 * 1024)

struct lib {
	char name[32];
	char ver[24];
};

struct libset {
	struct lib v[MAX_LIBS];
	int n;
};

/* soname prefix after "lib" -> displayed name */
static const struct { const char *prefix, *shown; } soname_table[] = {
	{ "wlroots",              "wlroots" },
	{ "scenefx",              "scenefx" },
	{ "weston",               "libweston" },
	{ "neuswc",               "neuswc" },
	{ "swc",                  "swc" },
	{ "mutter",               "mutter" },
	{ "kwin",                 "kwin" },
	{ "aquamarine",           "aquamarine" },
	{ "mirserver",            "mir" },
	{ "louvre",               "louvre" },
	{ "Qt6WaylandCompositor", "qtwayland-compositor" },
	{ "Qt5WaylandCompositor", "qtwayland-compositor" },
};

/* strings that survive static linking */
static const struct { const char *needle, *shown; } static_table[] = {
	{ "wlr_backend_autocreate",      "wlroots" },
	{ "wlr_backend_start",           "wlroots" },
	{ "wlr_scene_create",            "wlroots" },
	{ "scenefx",                     "scenefx" },
	{ "fx_renderer",                 "scenefx" },
	{ "swc_initialize",              "swc" },
	{ "weston_compositor_create",    "libweston" },
	{ "meta_wayland_compositor",     "mutter" },
	{ "smithay",                     "smithay" },
	{ "aquamarine",                  "aquamarine" },
};

/* fallback when /proc/<pid>/maps is unavailable */
static const struct { const char *wc, *lib; } guess_table[] = {
	{ "sway",        "wlroots" },
	{ "swayfx",      "wlroots, scenefx" },
	{ "dwl",         "wlroots" },
	{ "river",       "wlroots" },
	{ "labwc",       "wlroots" },
	{ "wayfire",     "wlroots" },
	{ "cage",        "wlroots" },
	{ "niri",        "smithay" },
	{ "weston",      "libweston" },
	{ "gnome-shell", "mutter" },
	{ "kwin_wayland","kwin" },
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void
add_lib(struct libset *s, const char *name, const char *ver)
{
	int i;

	for (i = 0; i < s->n; i++) {
		if (strcmp(s->v[i].name, name) != 0)
			continue;
		if (!s->v[i].ver[0] && ver && *ver)
			snprintf(s->v[i].ver, sizeof(s->v[i].ver), "%s", ver);
		return;
	}

	if (s->n >= MAX_LIBS)
		return;

	snprintf(s->v[s->n].name, sizeof(s->v[s->n].name), "%s", name);
	snprintf(s->v[s->n].ver, sizeof(s->v[s->n].ver), "%s", ver ? ver : "");
	s->n++;
}

static void
classify_soname(struct libset *s, const char *base)
{
	const char *rest;
	int i;

	if (strncmp(base, "lib", 3) != 0)
		return;
	rest = base + 3;

	for (i = 0; i < N(soname_table); i++) {
		size_t len = strlen(soname_table[i].prefix);
		char ver[24] = "";

		if (strncmp(rest, soname_table[i].prefix, len) != 0)
			continue;

		if (rest[len] == '-' && isdigit((unsigned char)rest[len + 1])) {
			size_t k = 0;
			const char *p = rest + len + 1;

			while ((isdigit((unsigned char)*p) || *p == '.') &&
			       k < sizeof(ver) - 1)
				ver[k++] = *p++;
			while (k > 0 && ver[k - 1] == '.')
				k--;
			ver[k] = '\0';
		} else if (rest[len] != '.') {
			continue;
		}

		add_lib(s, soname_table[i].shown, ver);
		return;
	}
}

static void
scan_maps(struct libset *s, int pid)
{
	char path[64], line[4096];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	f = fopen(path, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *p = line, *base;
		int i;

		for (i = 0; i < 5 && *p; i++) {
			while (*p && !isspace((unsigned char)*p))
				p++;
			while (*p && isspace((unsigned char)*p))
				p++;
		}
		if (*p != '/')
			continue;

		p[strcspn(p, "\n")] = '\0';
		base = strrchr(p, '/');
		classify_soname(s, base ? base + 1 : p);
	}

	fclose(f);
}

static void
scan_binary(struct libset *s, int pid)
{
	char path[64];
	FILE *f;
	size_t maxneedle = 0, have = 0;
	long total = 0;
	unsigned char *buf;
	const size_t chunk = 1 << 20;
	int i;

	for (i = 0; i < N(static_table); i++) {
		size_t l = strlen(static_table[i].needle);
		if (l > maxneedle)
			maxneedle = l;
	}

	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	f = fopen(path, "rb");
	if (!f)
		return;

	buf = malloc(chunk + maxneedle);
	if (!buf) {
		fclose(f);
		return;
	}

	for (;;) {
		size_t got = fread(buf + have, 1, chunk, f);
		size_t len = have + got;

		if (got == 0)
			break;

		for (i = 0; i < N(static_table); i++) {
			if (memmem(buf, len, static_table[i].needle,
			           strlen(static_table[i].needle)))
				add_lib(s, static_table[i].shown, "");
		}

		total += (long)got;
		if (total > SCAN_LIMIT)
			break;

		/* keep a tail in case a string spans two chunks */
		have = len < maxneedle ? len : maxneedle - 1;
		memmove(buf, buf + len - have, have);
	}

	free(buf);
	fclose(f);
}

static pid_t
peer_pid(int fd)
{
#ifdef SO_PEERCRED
	struct ucred cr;
	socklen_t len = sizeof(cr);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 &&
	    cr.pid > 0)
		return cr.pid;
#else
	(void)fd;
#endif
	return -1;
}

static int
read_small(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return 0;
	n = fread(out, 1, size - 1, f);
	fclose(f);
	out[n] = '\0';
	return (int)n;
}

static int
read_stat(int p, char *comm, size_t csz, int *ppid)
{
	char path[64], buf[1024], state;
	char *l, *r;
	size_t n;

	snprintf(path, sizeof(path), "/proc/%d/stat", p);
	if (read_small(path, buf, sizeof(buf)) <= 0)
		return 0;

	l = strchr(buf, '(');
	r = strrchr(buf, ')');
	if (!l || !r || r < l)
		return 0;

	n = (size_t)(r - l - 1);
	if (n >= csz)
		n = csz - 1;
	memcpy(comm, l + 1, n);
	comm[n] = '\0';

	return sscanf(r + 1, " %c %d", &state, ppid) == 2;
}

/* Is Xwayland (or xwayland-satellite) a descendant of the compositor? */
static int
has_xwayland_child(int pid)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int found = 0;

	if (!d)
		return -1;

	while (!found && (e = readdir(d))) {
		char comm[64];
		int p, cur, pp, depth;

		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		p = atoi(e->d_name);

		if (!read_stat(p, comm, sizeof(comm), &pp))
			continue;
		if (strcmp(comm, "Xwayland") != 0 &&
		    strncmp(comm, "xwayland-sat", 12) != 0)
			continue;

		for (cur = pp, depth = 0; cur > 1 && depth < 4; depth++) {
			if (cur == pid) {
				found = 1;
				break;
			}
			if (!read_stat(cur, comm, sizeof(comm), &pp))
				break;
			cur = pp;
		}
	}

	closedir(d);
	return found;
}

/* Lazy start: Xwayland not running yet, but the compositor owns the X11 socket. */
static int
has_x11_socket(int pid)
{
	unsigned long inodes[32];
	int n = 0, i;
	char line[1024], path[64];
	FILE *f;
	DIR *d;
	struct dirent *e;

	f = fopen("/proc/net/unix", "r");
	if (!f)
		return -1;

	while (n < 32 && fgets(line, sizeof(line), f)) {
		unsigned long ino;
		int off = 0;

		if (sscanf(line, "%*s %*s %*s %*s %*s %*s %lu %n",
		           &ino, &off) < 1 || off == 0)
			continue;
		if (strstr(line + off, ".X11-unix/X"))
			inodes[n++] = ino;
	}
	fclose(f);

	snprintf(path, sizeof(path), "/proc/%d/fd", pid);
	d = opendir(path);
	if (!d)
		return -1;

	while ((e = readdir(d))) {
		char link[320], target[96];
		unsigned long ino;
		ssize_t len;

		snprintf(link, sizeof(link), "%s/%s", path, e->d_name);
		len = readlink(link, target, sizeof(target) - 1);
		if (len <= 0)
			continue;
		target[len] = '\0';

		if (sscanf(target, "socket:[%lu]", &ino) != 1)
			continue;
		for (i = 0; i < n; i++) {
			if (inodes[i] == ino) {
				closedir(d);
				return 1;
			}
		}
	}

	closedir(d);
	return 0;
}

static int
check_xwayland(int pid)
{
	int child = has_xwayland_child(pid);
	int sock;

	if (child == 1)
		return 1;

	sock = has_x11_socket(pid);
	if (sock == 1)
		return 1;
	if (child < 0 || sock < 0)
		return -1;

	return 0;
}

static int
is_interpreter(const char *n)
{
	static const char *const list[] = {
		"python", "perl", "ruby", "node", "lua", "java", "sh", "bash",
		"dash", "zsh", "env",
	};
	int i;

	for (i = 0; i < N(list); i++) {
		size_t l = strlen(list[i]);
		if (strncmp(n, list[i], l) == 0 &&
		    (n[l] == '\0' || isdigit((unsigned char)n[l]) || n[l] == '.'))
			return 1;
	}
	return 0;
}

/* .sway-wrapped -> sway (Nix wrappers) */
static void
clean_name(char *n)
{
	size_t l = strlen(n);
	const char *suffix = "-wrapped";
	size_t sl = strlen(suffix);

	if (l > sl && strcmp(n + l - sl, suffix) == 0)
		n[l - sl] = '\0';
	if (n[0] == '.' && n[1])
		memmove(n, n + 1, strlen(n));
}

static void
detect_name(struct compositor_info *ci, int pid)
{
	char path[64], target[4096], name[128] = "", cmd[4096];
	ssize_t n;
	char *base;

	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	n = readlink(path, target, sizeof(target) - 1);
	if (n > 0) {
		char *del;

		target[n] = '\0';
		del = strstr(target, " (deleted)");
		if (del)
			*del = '\0';
		base = strrchr(target, '/');
		snprintf(name, sizeof(name), "%.127s", base ? base + 1 : target);
	}

	if (name[0] && is_interpreter(name)) {
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		n = read_small(path, cmd, sizeof(cmd));
		if (n > 0) {
			const char *a = cmd;
			int i;

			/* skip argv[0], take the first non-option argument */
			a += strlen(a) + 1;
			for (i = 0; a < cmd + n && i < 8; i++, a += strlen(a) + 1) {
				if (*a && *a != '-' && strcmp(a, "-c") != 0) {
					base = strrchr(a, '/');
					snprintf(name, sizeof(name), "%.127s",
					         base ? base + 1 : a);
					break;
				}
			}
		}
	}

	if (!name[0]) {
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		if (read_small(path, name, sizeof(name)) > 0)
			name[strcspn(name, "\n")] = '\0';
	}

	clean_name(name);
	snprintf(ci->name, sizeof(ci->name), "%s", name);
}

static void
format_libs(struct compositor_info *ci, const struct libset *s)
{
	size_t off = 0;
	int i;

	ci->libs[0] = '\0';
	for (i = 0; i < s->n && off < sizeof(ci->libs) - 1; i++) {
		int w = snprintf(ci->libs + off, sizeof(ci->libs) - off,
		                 "%s%s%s%s", i ? ", " : "", s->v[i].name,
		                 s->v[i].ver[0] ? " " : "", s->v[i].ver);
		if (w < 0)
			break;
		off += (size_t)w;
	}
}

int
detect_compositor(int fd, struct compositor_info *ci)
{
	struct libset s = {0};
	pid_t pid;
	int i;

	memset(ci, 0, sizeof(*ci));
	ci->xwayland = -1;

	pid = peer_pid(fd);
	if (pid <= 0)
		return -1;

	detect_name(ci, pid);
	ci->xwayland = check_xwayland(pid);
	scan_maps(&s, pid);
	scan_binary(&s, pid);
	format_libs(ci, &s);

	if (!ci->libs[0] && ci->name[0]) {
		for (i = 0; i < N(guess_table); i++) {
			if (strcmp(ci->name, guess_table[i].wc) == 0) {
				snprintf(ci->libs, sizeof(ci->libs), "%s",
				         guess_table[i].lib);
				ci->guessed = 1;
				break;
			}
		}
	}

	return ci->name[0] || ci->libs[0] ? 0 : -1;
}
