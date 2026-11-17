#ifndef WAYFETCH_DETECT_H
#define WAYFETCH_DETECT_H

struct compositor_info {
	char name[128];   /* "" if unknown */
	char libs[256];   /* e.g. "wlroots 0.19, scenefx 0.5" */
	int  guessed;     /* libs come from the name table, not the process */
	int  xwayland;    /* 1 yes, 0 no, -1 unknown */
};

/* Detect the compositor from the Wayland socket fd (SO_PEERCRED + /proc). */
int detect_compositor(int fd, struct compositor_info *ci);

#endif
