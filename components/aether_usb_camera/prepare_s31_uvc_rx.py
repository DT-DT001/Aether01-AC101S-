"""Prepare the pinned UVC driver with ISO frame handling and receive diagnostics."""

import argparse
import hashlib
from pathlib import Path


ISOC_CHANGES = (
    ('#include "esp_log.h"', '#include "esp_log.h"\n#include "camera_uvc_rx.h"\n#include "camera_uvc_packet.h"'),
    (
        '    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;',
        '    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;\n'
        '    aether_usb_camera_rx_status_t sample = {.transfers = 1};',
    ),
    (
        '        return; // If the streaming was turned off, we don\'t have to do anything',
        '''        goto done; // Streaming has already stopped
    }

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        // HCD does not update the packet descriptors when the whole URB fails.
        ++sample.errors;
        sample.transfer_status = transfer->status;
        uvc_stream->single_thread.skip_current_frame = true;
        goto requeue;''',
    ),
    (
        '            return; // No need to process the rest',
        '            ++sample.errors;\n            goto done;',
    ),
    (
        '            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);',
        '            ++sample.errors;\n'
        '            ESP_LOGD(TAG, "usb err %d", isoc_desc->status);',
    ),
    (
        '            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue',
        '            ++sample.skipped;\n            goto next_isoc_packet;',
    ),
    (
        '    const uint8_t *payload = transfer->data_buffer;',
        '    const uint8_t *payload = transfer->data_buffer;\n'
        '    const usb_device_desc_t *device_desc = NULL;',
    ),
    (
        '        // Check for start of new frame',
        '''        const int actual_bytes = isoc_desc->actual_num_bytes;
        if (actual_bytes == 0) {
            ++sample.empty;
            goto next_isoc_packet;
        }
        if (actual_bytes < 0 || actual_bytes > isoc_desc->num_bytes) {
            ++sample.errors;
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }
        if (!sample.packets) {
            const size_t available = (size_t)actual_bytes;
            const size_t copy_bytes = available < sizeof(sample.first_header) ?
                                      available : sizeof(sample.first_header);
            memcpy(sample.first_header, payload, copy_bytes);
        }
        ++sample.packets;
        if ((uint32_t)actual_bytes > sample.max_packet_bytes) {
            sample.max_packet_bytes = actual_bytes;
        }

        // Check for start of new frame''',
    ),
    (
        '        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {',
        '''        bool allow_missing_eoh = false;
        if ((size_t)actual_bytes >= sizeof(*payload_header) && !payload_header->bmHeaderInfo.end_of_header) {
            if (!device_desc) {
                (void)usb_host_get_device_descriptor(uvc_stream->constant.dev_hdl, &device_desc);
            }
            // GENERAL - UVC omits EOH on its ISO video payloads.
            allow_missing_eoh = device_desc && device_desc->idVendor == 0x1908 && device_desc->idProduct == 0x3283;
        }
        if (!uvc_frame_payload_header_validate_with_quirks(payload_header, actual_bytes, allow_missing_eoh)) {''',
    ),
    (
        '''            ESP_LOGD(TAG, "invalid UVC payload header, %02x, %02x, len:%d", payload[0], payload[1], isoc_desc->actual_num_bytes);
            if (isoc_desc->actual_num_bytes != 0) {
                // Only skip the frame if we received some data (otherwise it's just an empty packet, which is not an issue by itself)
                uvc_stream->single_thread.skip_current_frame = true;
            }''',
        '''            ++sample.invalid_headers;
            uvc_stream->single_thread.skip_current_frame = true;''',
    ),
    (
        '''        if (payload_data_len == 0) {
            // This is a zero-length packet, skip it
            ESP_LOGD(TAG, "zero-length packet, skipping");
            goto next_isoc_packet;
        }''',
        '''        if (payload_header->bmHeaderInfo.end_of_frame) {
            ++sample.frame_ends;
        } else if (payload_data_len == 0) {
            goto next_isoc_packet;
        }''',
    ),
    (
        '        if (start_of_frame) {\n',
        '''        if (start_of_frame) {
            if (payload_data_len == 0) {
                // A header-only EOF may finish a frame with the same FID.
                // Without matching data, leave the next real FID transition intact.
                goto next_isoc_packet;
            }
            ++sample.frame_ids;
''',
    ),
    (
        '                    (payload_data[0] != JPEG_MARKER || payload_data[1] != JPEG_SOI)) {',
        '                    (payload_data_len < 2 || payload_data[0] != JPEG_MARKER || payload_data[1] != JPEG_SOI)) {\n'
        '                ++sample.invalid_soi;',
    ),
    (
        '        if (!uvc_stream->single_thread.skip_current_frame) {',
        '        if (payload_data_len && !uvc_stream->single_thread.skip_current_frame) {',
    ),
    (
        '    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {\n'
        '        usb_host_transfer_submit(transfer); // Restart the transfer\n    }',
        'requeue:\n    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {\n'
        '        sample.submit_error = usb_host_transfer_submit(transfer);\n    }\n'
        'done:\n    aether_uvc_rx_observe(uvc_stream, &sample);',
    ),
)

FRAME_CHANGES = (
    ('#include "uvc_frame_priv.h"', '#include "uvc_frame_priv.h"\n#include "camera_uvc_packet.h"'),
    (
        'bool uvc_frame_payload_header_validate(const uvc_payload_header_t *hdr, size_t packet_len)\n{',
        '''bool uvc_frame_payload_header_validate(const uvc_payload_header_t *hdr, size_t packet_len)
{
    return uvc_frame_payload_header_validate_with_quirks(hdr, packet_len, false);
}

bool uvc_frame_payload_header_validate_with_quirks(const uvc_payload_header_t *hdr,
                                                  size_t packet_len, bool allow_missing_eoh)
{
    (void)allow_missing_eoh;''',
    ),
    (
        '    if (!hdr->bmHeaderInfo.end_of_header) {',
        '    if (!allow_missing_eoh && !hdr->bmHeaderInfo.end_of_header) {',
    ),
)

SOURCES = {
    'uvc_isoc.c': ('bc22b9d1ab94cdf5e6c759af99f395f910481c442039f99b0f75156ef1b4ea67', ISOC_CHANGES),
    'uvc_frame.c': ('40c1dc3abd4dfcdf44dd369beb89be4781404ab81e1732c41c5da4eeef76d97d', FRAME_CHANGES),
    'uvc_descriptor_parsing.c': (
        '7479bba7b7c24d744da54b0fe90ef69767ce95f88573bd6d8652c70268589837',
        ((
            '        if (current_mps >= last_mps && current_mps <= dwMaxPayloadTransferSize) {',
            '        // Keep the first alternate when capacity is identical.\n'
            '        if (current_mps > last_mps && current_mps <= dwMaxPayloadTransferSize) {',
        ),),
    ),
}


def prepare(source: Path, output: Path) -> None:
    expected_hash, changes = SOURCES[source.name]
    original = source.read_bytes().replace(b'\r\n', b'\n')
    if hashlib.sha256(original).hexdigest() != expected_hash:
        raise ValueError(f'S31 UVC receive overlay requires unmodified usb_host_uvc 2.6.0 {source.name}')
    contents = original.decode('utf-8')
    for anchor, replacement in changes:
        if contents.count(anchor) != 1:
            raise ValueError(f'UVC receive overlay anchor mismatch: {anchor.splitlines()[0]}')
        contents = contents.replace(anchor, replacement, 1)
    generated = contents.encode('utf-8')
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != generated:
        output.write_bytes(generated)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
