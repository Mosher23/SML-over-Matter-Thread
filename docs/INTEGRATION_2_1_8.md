# Companion integration v2.1.8

Integration-only update; firmware v1.21 can remain installed.

The **Meter ID** diagnostic now formats recognized ten-byte DIN 43863-5
electricity-meter IDs as the readable identifier printed on the meter. For
example, a tester's Landis+Gyr E320 reports `0A014C475A0004D70208`, which is
displayed as **`1LGZ0081199624`**. This fixes presentation of a correctly received
ID; it does not change the OBIS mapping or firmware validation.

The original firmware value remains available in the sensor's **`raw_meter_id`**
attribute. Already readable IDs and unrecognized formats are preserved.
Conversion requires exactly ten bytes, encoding `09` or `0A`, electricity
sector `01`, three uppercase ASCII manufacturer letters, a production block
other than `FF`, and a serial number no greater than `99999999`. The production
block is hexadecimal; the serial is big-endian and displayed as eight decimal
digits, preserving leading zeros. This is not a Landis+Gyr-only substitution.

The entity's unique ID is unchanged. No firmware OTA, device reset, or Matter
re-pairing is required. Update the companion and restart Home Assistant.
Power, energy, manufacturer, OBIS-source diagnostics, and the seven default
diagnostics retain their existing behavior.

References:

- [Landis+Gyr E320 meter-ID structure](https://www.ewv-hamm-netz.de/fileadmin/user-storage/EWV/Messstellenbetrieb/Benutzerhandbuch-E320-eHz.pdf)
- [VDE/FNN MUC specification, section 7.2 and figure 20 (encoding 09)](https://www.vde.com/resource/blob/951984/92182c8da40233071416bce882d4b784/fnn-lh-muc-1-01-2011-07-04-data.pdf)
- [DIN 43863-5 field description published by Erlanger Stadtwerke](https://netze.estw.de/erlangenGips/Erlangen/__attic__20210120_155237__estw1.de/Kopfnavigation/Netze/Messwesen/Messwesen/Herstelleruebergreifende-Identifikationsnummer-fuer-Messeinrichtungen.pdf)
