# ESP8266_JBD_BMS_Monitor
This project provides Platform.io code for building a JBD BMS monitor. It:
- Sends read commands to the JBD BMS and receives all key statistics, including battery voltage, current, remaining capacity, number of cycles, BMS protection status, state of charge, NTC temperatures, individual cell voltages and cell delta voltage.
- Sends these statistics to InfluxDB and displays the key metrics on a TFT screen.
- Turns an optional external active cell balancer on and off when certain cell voltage thresholds are met (by defaut it will turn on when any cell is above 3.4v, and off when the highest cell drops below 3.35v).
