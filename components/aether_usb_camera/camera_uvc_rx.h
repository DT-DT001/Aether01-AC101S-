/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "aether_usb_camera.h"

/* Called by the UVC event task once per completed isochronous transfer. */
void aether_uvc_rx_observe(const void *stream, const aether_usb_camera_rx_status_t *sample);
