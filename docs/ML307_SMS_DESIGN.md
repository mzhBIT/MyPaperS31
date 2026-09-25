# ML307A SMS design

## Scope

The Messages application uses the ML307A standard AT SMS commands while the
same USB composite device continues to provide the RNDIS 4G data connection.
Received messages remain in the modem-selected SMS store. Successfully sent
messages are mirrored to `/sdcard/.reader/sms/sent.tsv`, because `CMGS` does not
create a sent-folder entry on this modem firmware. The file stores hexadecimal
UTF-8 fields so tabs and newlines in message text cannot corrupt the records.

## USB ownership

- Interfaces 0/1 remain owned by the RNDIS driver.
- Interface 2 (`bulk IN 0x81`, `bulk OUT 0x0a`) is the persistent SMS AT port.
- The CDC watcher records the device address during enumeration but deliberately
  does not open interface 2 during the initial raw-USB dial stage.
- After the dial client releases its interface, device and EP0 ownership, the
  RNDIS data plane starts and `ml307_sms_transport_enable()` opens interface 2
  through the same `iot_usbh_cdc` host stack. This avoids the former dual-client
  EP0 race.

All AT transactions are serialized with one mutex. Receive callbacks only move
bytes into a ring-backed response buffer and signal an event; parsing and UI work
never run inside the USB callback.

## AT setup and commands

The service lazily configures the modem on the first operation:

1. `ATE0`
2. `AT+CMEE=2`
3. `AT+CMGF=1`
4. Select the device firmware's only advertised character set with
   `AT+CSCS="IRA"`; send Unicode as SMS-SUBMIT PDUs
5. `AT+CPMS="SM","SM","SM"` with `ME` fallback
6. `AT+CNMI=2,1,0,0,0`

The UI uses `CMGL`, `CMGR`, `CMGS` and `CMGD`. A `+CMTI` URC sets a lightweight
notification flag and the open Messages page refreshes when idle.

Chinese phonebook fields and bodies are converted between UTF-8 and UCS-2 hex.
One outgoing message is currently limited to 70 UTF-16 code units. Concatenated
SMS/PDU segmentation is intentionally left for a later extension.

## UI

The second home page contains a 150x150 monochrome icon extracted from the
`Regular-S` symbol in `Assets/messages-square.svg`. The page provides:

- phone-number-normalized conversation list with unread marker, timestamp,
  message count and latest preview;
- combined received and locally persisted sent history inside each conversation;
  received bubbles align left and sent bubbles align right, with vertical scrolling;
- tap a received bubble to select it for deletion; reply stays in the footer;
- vertical paging, refresh and compose actions;
- message detail, reply and delete actions;
- the shared floating Chinese/English keyboard for recipient and body entry;
- background AT workers so modem response time does not block touch handling.
