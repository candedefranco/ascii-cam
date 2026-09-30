#ifndef ASCII_CAM_CAMERA_H
#define ASCII_CAM_CAMERA_H

#include <stddef.h>
#include <stdint.h>

enum {
    CAM_OK = 0,
    CAM_ERR_DENIED = -1,   /* the user (or macOS) denied camera access */
    CAM_ERR_NODEVICE = -2, /* no camera found */
    CAM_ERR_OPEN = -3,     /* the camera exists but could not be opened */
};

/* Starts capturing from the default camera. Returns CAM_OK or a CAM_ERR_* code. */
int cam_open(void);

/*
 * Copies the most recent frame as packed RGB (3 bytes per pixel) into *buf,
 * growing it with realloc when needed. Returns 1 if the frame is new since the
 * previous call, 0 if there is no new frame yet.
 */
int cam_copy(uint8_t **buf, size_t *cap, int *w, int *h);

void cam_close(void);

#endif
