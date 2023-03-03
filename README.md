# ESP8266_JBD_BMS_Monitor
This project provides Platform.io code for building an ESP8266 based monitor for JBD BMS.

- Sends read commands to the JBD BMS and receives all key statistics, including battery voltage, current, remaining capacity, number of cycles, BMS protection status, state of charge, NTC temperatures, individual cell voltages and cell delta voltage.
- Sends these statistics to InfluxDB and displays the key metrics on a TFT screen.
- Turns an optional external active cell balancer (e.g. Hankzor) on and off when certain cell voltage thresholds are met (by default it will turn on when any cell is above 3.4v, and off when the highest cell drops below 3.35v).


### Notes
- The file include/secrets.h should be populated with information relevant to your setup (WiFi SSID, password, and InfluxDB details)
- Data can be sent to InfluxDB using the HTTP API or UDP. Both can be disabled by commenting out the DEFINE statements at the top of main.cpp
- A generic ILI9341 TFT screen can be connected through the ESP's standard SPI interface. The TFT's reset pin should be connected to the ESP's reset pin.
- The active-high input for a relay connected across the balancer's "run" pads should be connected to D0/GPIO16. Note this pin is high at boot time, so may turn the balancer on briefly when the ESP is reset.
- This is designed to work in conjunction with my UART multiplexer, allowing simultaneous Bluetooth connectivity and ESP monitoring: https://github.com/octal-ip/STM32_JBD_UART_Mux
- The code includes TelnetPrint, which can be used to view realtime statistics through telnet port 23 of the ESP.
- SoftwareSerial is implemented for the interface to the JBD BMS on ports D1 (TX) and D2 (RX), leaving the hardware serial available for debugging.
