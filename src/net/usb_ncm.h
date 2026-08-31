#ifndef PM220_USB_NCM_H
#define PM220_USB_NCM_H

void usb_ncm_init(void);
void usb_ncm_poll(void);
void usb_ncm_ensure_mac(void);
void usb_ncm_log(void);

#endif
