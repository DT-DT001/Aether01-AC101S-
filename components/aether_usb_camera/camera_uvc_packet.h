/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include "usb_types_uvc.h"

bool uvc_frame_payload_header_validate_with_quirks(const uvc_payload_header_t *header,
                                                  size_t packet_len, bool allow_missing_eoh);
