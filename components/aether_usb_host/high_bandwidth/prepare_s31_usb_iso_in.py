"""Prepare the pinned Espressif HCD with S31 high-bandwidth ISO IN support."""

import argparse
import hashlib
from pathlib import Path


USB_160_HCD_SHA256 = "f7be5cff2be70176baab93e1b4e643e1d5bd3cd56383f01cc79392c76c5adc15"

CHANGES = (
    (
        "    usb_dwc_hal_ep_char_t ep_char;\n    // Port related",
        "    usb_dwc_hal_ep_char_t ep_char;\n"
        "    uint8_t hs_iso_in_transactions; // Nonzero only for high-bandwidth ISO IN\n"
        "    // Port related",
    ),
    (
        "static bool pipe_alloc_hcd_support_verification(usb_dwc_hal_context_t *hal, const usb_ep_desc_t *ep_desc)",
        "static bool pipe_alloc_hcd_support_verification(usb_dwc_hal_context_t *hal, const usb_ep_desc_t *ep_desc,\n"
        "                                                 uint8_t hs_iso_in_transactions)",
    ),
    (
        """    if (USB_EP_DESC_GET_MPS(ep_desc) > limit) {
        ESP_LOGE(HCD_DWC_TAG, "EP MPS (%d) exceeds supported limit (%d)",
                 USB_EP_DESC_GET_MPS(ep_desc),
                 limit);""",
        """    const int mps = USB_EP_DESC_GET_MPS(ep_desc);
    const int payload = hs_iso_in_transactions ? mps * hs_iso_in_transactions : mps;
    if (payload > limit) {
        ESP_LOGE(HCD_DWC_TAG, "EP payload (%d) exceeds supported FIFO limit (%d)", payload, limit);""",
    ),
    (
        """    // Default pipes have a NULL ep_desc thus should skip the HCD support verification
    if (!is_default && !pipe_alloc_hcd_support_verification(port->hal, pipe_config->ep_desc)) {""",
        """    uint8_t hs_iso_in_transactions = 0;
    if (type == USB_TRANSFER_TYPE_ISOCHRONOUS && pipe_config->dev_speed == USB_SPEED_HIGH &&
            USB_EP_DESC_GET_EP_DIR(pipe_config->ep_desc)) {
        const uint8_t multiplier = USB_EP_DESC_GET_MULT(pipe_config->ep_desc);
        if (multiplier > 2) {
            ESP_LOGE(HCD_DWC_TAG, "Reserved ISO IN endpoint multiplier");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (multiplier != 0) {
            const unsigned mps = USB_EP_DESC_GET_MPS(pipe_config->ep_desc);
            if (mps == 0 || mps > 1024) {
                ESP_LOGE(HCD_DWC_TAG, "Invalid high-bandwidth ISO IN packet size");
                return ESP_ERR_NOT_SUPPORTED;
            }
            hs_iso_in_transactions = multiplier + 1;
        }
    }
    // Default pipes have a NULL ep_desc thus should skip the HCD support verification
    if (!is_default && !pipe_alloc_hcd_support_verification(port->hal, pipe_config->ep_desc,
                                                            hs_iso_in_transactions)) {""",
    ),
    (
        "    pipe->chan_obj = chan_obj;\n    usb_dwc_hal_ep_char_t ep_char;",
        "    pipe->chan_obj = chan_obj;\n"
        "    pipe->hs_iso_in_transactions = hs_iso_in_transactions;\n"
        "    usb_dwc_hal_ep_char_t ep_char;",
    ),
    (
        "    pipe->port->num_pipes_idle--;\n    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);",
        """    pipe->port->num_pipes_idle--;
    if (pipe->hs_iso_in_transactions) {
        // HCCHAR.eC survives channel release. Restore its reset value before reuse.
        pipe->chan_obj->regs->hcchar_reg.ec = 0;
    }
    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);""",
    ),
    (
        """        start_idx = buffer_to_exec->flags.isoc.start_idx;
        desc_list_len = XFER_LIST_LEN_ISOC;
        break;""",
        """        start_idx = buffer_to_exec->flags.isoc.start_idx;
        desc_list_len = XFER_LIST_LEN_ISOC;
        if (pipe->hs_iso_in_transactions) {
            // One QTD spans a microframe and already holds its aggregate size.
            // Program multi-count and the initial PID before enabling the channel.
            // HCTSIZ encodes DATA2 as 1 and DATA1 as 2.
            pipe->chan_obj->regs->hcchar_reg.ec = pipe->hs_iso_in_transactions;
            pipe->chan_obj->regs->hctsiz_reg.pid = pipe->hs_iso_in_transactions == 3 ? 1 : 2;
            // The HS controller derives HCDMA.CTD from the microframe.
            // Keep flags.isoc.start_idx for descriptor placement and parsing.
            start_idx = 0;
        }
        break;""",
    ),
    (
        """        if (urb->transfer.num_isoc_packets * pipe->ep_char.periodic.interval > XFER_LIST_LEN_ISOC - XFER_LIST_ISOC_MARGIN) {
            fits = false;
        }
        break;""",
        """        if (urb->transfer.num_isoc_packets * pipe->ep_char.periodic.interval > XFER_LIST_LEN_ISOC - XFER_LIST_ISOC_MARGIN) {
            fits = false;
        }
        if (fits && pipe->hs_iso_in_transactions) {
            const int payload = pipe->ep_char.mps * pipe->hs_iso_in_transactions;
            for (int i = 0; i < urb->transfer.num_isoc_packets; ++i) {
                if (urb->transfer.isoc_packet_desc[i].num_bytes > payload) {
                    fits = false;
                    break;
                }
            }
        }
        break;""",
    ),
)


def prepare(source: Path, output: Path) -> None:
    original = source.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(original).hexdigest() != USB_160_HCD_SHA256:
        raise ValueError("S31 ISO IN overlay requires the unmodified espressif/usb 1.6.0 hcd_dwc.c")
    contents = original.decode("utf-8")
    for anchor, patch in CHANGES:
        if contents.count(anchor) != 1:
            raise ValueError(f"S31 ISO IN overlay anchor mismatch: {anchor.splitlines()[0]}")
        contents = contents.replace(anchor, patch, 1)
    generated = contents.encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != generated:
        output.write_bytes(generated)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
