# Changelog

## 0.3.1 (2026-11-22)

- A call no longer goes silent and stays open on the phone when the phone app changes network, for example from Wi-Fi to mobile data. The app's new address is now accepted instead of refused
- Audio is sent to wherever the phone's audio really comes from
- When no audio comes from the phone for 30 seconds during a call, the call is ended on both sides

## 0.3.0 (2026-11-16)

- SMS that arrive on the SIM show up in the chat of every phone that is online, and wait for a phone that is offline
- Received SMS stay on the SIM when QModem's SMS page is installed, otherwise they are deleted once a phone has them
- Texts from phones go out through ModemManager or the QModem at-daemon when they run the modem
- A failed send no longer leaves the modem stuck, which could cut the RJ11 phone off from it
- A text the modem took but never confirmed is not reported as failed, so the app does not send it twice
- A SIM full of long texts no longer stops new ones reaching the phones

## 0.2.0 (2026-11-13)

- Texts from SIP phones go out as SMS, with long texts and Unicode
- The on/off switch is a coloured button that works with every LuCI theme

## 0.1.0 (2026-11-10)

- First release: SIP phones and apps make and take calls through the SIM, next to the RJ11 phone
