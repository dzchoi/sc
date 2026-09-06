/* See LICENSE for license details. */
#include <stdlib.h>
#include <stdint.h>
#include <png.h>
#include <X11/Xatom.h>

#include "icon.h"

static const unsigned char icon_png[] = {
#include "icon-data.h"
};

void
xseticon(Display *dpy, Window win)
{
	png_image image = {.version = PNG_IMAGE_VERSION};
	if (!png_image_begin_read_from_memory(&image, icon_png, sizeof(icon_png)))
		return;

	image.format = PNG_FORMAT_RGBA;
	size_t pixels = (size_t)image.width * image.height;
	size_t icon_size = pixels + 2;
	unsigned long *icon = malloc(icon_size * sizeof(*icon));
	if (!icon) {
		png_image_free(&image);
		return;
	}

	png_bytep rgba = (png_bytep)icon;
	if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
		free(icon);
		png_image_free(&image);
		return;
	}

	/*
	 * Expand from the last pixel so each RGBA tuple is read before its wider
	 * Xlib property element can overwrite the shared buffer.
	 */
	for (size_t i = pixels; i-- > 0;) {
		png_bytep pixel = rgba + i * 4;
		uint32_t argb = (uint32_t)pixel[3] << 24
		              | (uint32_t)pixel[0] << 16
		              | (uint32_t)pixel[1] << 8
		              | pixel[2];
		icon[i + 2] = argb;
	}
	icon[0] = image.width;
	icon[1] = image.height;

	Atom netwmicon = XInternAtom(dpy, "_NET_WM_ICON", False);
	XChangeProperty(dpy, win, netwmicon, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)icon, (int)icon_size);
	free(icon);
	png_image_free(&image);
}
