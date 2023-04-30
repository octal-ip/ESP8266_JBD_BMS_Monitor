#define UDP_MODE
//#define HTTP_MODE
#define TFT_ENABLE

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <RunningAverage.h>
#include <TelnetPrint.h>

#ifdef TFT_ENABLE
  /* If the TFT is to be enabled:
  - Uncomment the following line in .pio\libdeps\nodemcuv2\TFT_eSPI\User_Setup_Select.h
      #include <User_Setups/Setup1_ILI9341.h>
  - And ensure the following configuration is placed in .pio\libdeps\nodemcuv2\TFT_eSPI\User_Setups\Setup1_ILI9341.h
      #define ILI9341_DRIVER
      #define TFT_CS   PIN_D8  // Chip select control pin D8
      #define TFT_DC   PIN_D3  // Data Command control pin
      #define TFT_RST  -1    // Set TFT_RST to -1 if the display RESET is connected to NodeMCU RST or 3.3V
      #define SPI_FREQUENCY  40000000
      #define SPI_READ_FREQUENCY  20000000

    Additionally, the below three variables should be asjusted to suit your battery.
    This will allow the ring meters to render with the correct scale.

    Example for a 16S high current battery:
      int maxCurrent = 150;
      int minVoltage = 45;
      int maxVoltage = 58;
  */
  int maxCurrent = 100;
  int minVoltage = 45;
  int maxVoltage = 58;

  #include <SPI.h>
  #include <TFT_eSPI.h>
  #define BLUE2RED 3
  #define GREEN2RED 4
  #define RED2GREEN 5
  #define BUFF_SIZE 64
  #define TFT_GREY 0x2104 // Dark grey 16 bit colour
  TFT_eSPI tft = TFT_eSPI();
#endif

#ifdef HTTP_MODE
  #include <ESP8266HTTPClient.h>
#endif

#include <secrets.h> //Edit this file to include the following details:
/*
#define SECRET_SSID "<ssid>>"
#define SECRET_PASS "<password>"
#define SECRET_INFLUXDB "http://<IP Address>:8086/write?db=<db name>&u=<user name>&p=<password>"
#define SECRET_INFLUX_IP_OCTET1 <first IP octet>
#define SECRET_INFLUX_IP_OCTET2 <second IP octet>
#define SECRET_INFLUX_IP_OCTET3 <third IP octet>
#define SECRET_INFLUX_IP_OCTET4 <last IP octet>
*/

#define JBD_RX D2
#define JBD_TX D1
#define BALANCER D0

SoftwareSerial JBDSoftSerial;

byte failures = 0; //Automatically restart the ESP if too many failures occurr in a row.
byte packetStatus = 0;
byte packetCount = 0;
byte packetLength = 0;
byte incomingByte = 0;
byte avSamples = 240;
byte systemCycles = 0;
byte battCycles = 0;
char post[70];
char StatString[8] = {0x0};
unsigned long lastUnlock = 0;
unsigned long lastRequest = 0;
unsigned long connectTime = 0;
byte statSwitch = 0;

float voltage = 0.0;
float current = 0.0;
float capacityRem = 0.0;
float capacityTyp = 0.0;
float cycles = 0.0;
byte protectionStatus = 0;
byte percentRem = 0;
byte NTCs = 0;
byte cells = 16; //Default to 16 cells. This will be automatically updated.
byte cellCount = 0;
float lowestCell = 0.0;
float highestCell = 0.0;
byte lowestCellNumber = 0;
byte highestCellNumber = 0;
float cellVoltageDelta = 0.0;
float temps[4] = {0.0, 0.0, 0.0, 0.0};
bool balancerOn = 0;

#ifdef TFT_ENABLE
  //Record the last updates to the TFT screen to avoid updating the screen when no change has occurred. This improves speed and reduces flickering.
  float lastCurrent = 0.0, lastVoltage = 0.0, lastHighestCell = 0.0, lastLowestCell = 0.0, lastCellVoltageDelta = 0.0, lastTemp1 = 0.0, lastTemp2 = 0.0, lastPercentRem = 0.0;
  bool lastBalancerOn = 0;
#endif

RunningAverage currentAv(avSamples); //Average out the current readings as these can change rapidly between samples sent to InfluxDB. The other metrics change slowly, if at all.

char packetbuff[100] = {0x0};

#ifdef UDP_MODE
  WiFiUDP udp;
  IPAddress influxhost = {SECRET_INFLUX_IP_OCTET1, SECRET_INFLUX_IP_OCTET2, SECRET_INFLUX_IP_OCTET3, SECRET_INFLUX_IP_OCTET4}; // the IP address of the InfluxDB host
  int influxport = 8092; // the port that the InfluxDB UDP plugin is listening on
#endif

struct BMSCommands{
   const char *name;
   byte request[7];
   unsigned long LastRequest;
   unsigned long RequestFrequency;
   long value;
};

const int BMSRequestsNum = 13;

BMSCommands BMSRequests[BMSRequestsNum] = {
  //Request name, address, LastRequest, RequestFrequency, value)
  {"System_status", {0xdd, 0xa5, 0x03, 0x00, 0xff, 0xfd, 0x77}, 0, 500, 0},
  {"Battery_status", {0xdd, 0xa5, 0x04, 0x00, 0xff, 0xfc, 0x77}, 0, 2000, 0},
  {"Charge_over_temp_release", {0xdd, 0xa5, 0x19, 0x00, 0xff, 0xe7, 0x77}, 0, 60000, 0},
  {"Charge_under_temp_release", {0xdd, 0xa5, 0x1b, 0x00, 0xff, 0xe5, 0x77}, 0, 60000, 0},
  {"Discharge_over_temp_release", {0xdd, 0xa5, 0x1d, 0x00, 0xff, 0xe3, 0x77}, 0, 60000, 0},
  {"Discharge_under_temp_release", {0xdd, 0xa5, 0x1f, 0x00, 0xff, 0xe1, 0x77}, 0, 60000, 0},
  {"Batt_over_volt_trig", {0xdd, 0xa5, 0x20, 0x00, 0xff, 0xe0, 0x77}, 0, 60000, 0},
  {"Batt_over_volt_release", {0xdd, 0xa5, 0x21, 0x00, 0xff, 0xdf, 0x77}, 0, 60000, 0},
  {"Batt_under_volt_release", {0xdd, 0xa5, 0x23, 0x00, 0xff, 0xdd, 0x77}, 0, 60000, 0},
  {"Cell_over_volt_release", {0xdd, 0xa5, 0x25, 0x00, 0xff, 0xdb, 0x77}, 0, 60000, 0},
  {"Cell_under_volt_release", {0xdd, 0xa5, 0x27, 0x00, 0xff, 0xd9, 0x77}, 0, 60000, 0},
  {"Charge_over_current_trig", {0xdd, 0xa5, 0x28, 0x00, 0xff, 0xd8, 0x77}, 0, 60000, 0},
  {"Discharge_over_current_release", {0xdd, 0xa5, 0x29, 0x00, 0xff, 0xd7, 0x77}, 0, 60000, 0}
};

void setup() {
  Serial.begin(115200);

  JBDSoftSerial.begin(9600, SWSERIAL_8N1, JBD_RX, JBD_TX, false, 50, 500);
  JBDSoftSerial.enableRx(true);

  Serial.println();
  Serial.print("Connecting to "); Serial.println(SECRET_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_SSID, SECRET_PASS);

  #ifdef TFT_ENABLE
    tft.begin();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Status:", 2, 170, 2);
    tft.drawString("High Cell:", 2, 185, 2);
    tft.drawString("Low Cell:", 2, 200, 2);
    tft.drawString("Delta V:", 2, 215, 2);
    tft.drawString("Power:", 180, 170, 2);
    tft.drawString("Temp1:", 180, 185, 2);
    tft.drawString("Temp2:", 180, 200, 2);
    tft.drawString("SOC:", 180, 215, 2);
    tft.drawFastHLine(0, 160, 319, TFT_WHITE);
  #endif

  connectTime = millis();
  Serial.print("Waiting for WiFi to connect");

  while (!WiFi.isConnected() && (unsigned long)(millis() - connectTime) < 5000) { //Wait for the wifi to connect for up to 5 seconds.
    delay(100);
    Serial.print(".");
  }

  if (!WiFi.isConnected()) {
    Serial.println();
    Serial.println("WiFi didn't connect, restarting...");
    ESP.restart(); //Restart if the WiFi still hasn't connected.
  }
  else {
    Serial.println();
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
  }

  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[MAC]
  ArduinoOTA.setHostname("esp8266-JBD-BMS-monitor");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  // ****End ESP8266 OTA and Wifi Configuration****

  //Telnet log is accessible at port 23
  TelnetPrint.begin();

  pinMode(BALANCER, OUTPUT);
}

#ifdef TFT_ENABLE
  unsigned int rainbow(byte value)
  {
    // Value is expected to be in range 0-127
    // The value is converted to a spectrum colour from 0 = blue through to 127 = red

    byte red = 0; // Red is the top 5 bits of a 16 bit colour value
    byte green = 0;// Green is the middle 6 bits
    byte blue = 0; // Blue is the bottom 5 bits

    byte quadrant = value / 32;

    if (quadrant == 0) {
      blue = 31;
      green = 2 * (value % 32);
      red = 0;
    }
    if (quadrant == 1) {
      blue = 31 - (value % 32);
      green = 63;
      red = 0;
    }
    if (quadrant == 2) {
      blue = 0;
      green = 63;
      red = value % 32;
    }
    if (quadrant == 3) {
      blue = 0;
      green = 63 - 2 * (value % 32);
      red = 31;
    }
    return (red << 11) + (green << 5) + blue;
  }

  float mapf(float x, float in_min, float in_max, float out_min, float out_max)
  {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  }

  int ringMeter(float value, int vmin, int vmax, int x, int y, int r, const char *units, byte scheme)
  {
    // Minimum value of r is about 52 before value text intrudes on ring
    // drawing the text first is an option
    
    x += r; y += r;   // Calculate coords of centre of ring

    int w = r / 3;    // Width of outer ring is 1/4 of radius
    
    int angle = 150;  // Half the sweep angle of meter (300 degrees)

    int v = mapf(value, vmin, vmax, -angle, angle); // Map the value to an angle v

    byte seg = 3; // Segments are 3 degrees wide = 100 segments for 300 degrees
    byte inc = 6; // Draw segments every 3 degrees, increase to 6 for segmented ring

    // Variable to save "value" text colour from scheme and set default
    int colour = TFT_BLUE;
  
    // Draw colour blocks every inc degrees
    for (int i = -angle+inc/2; i < angle-inc/2; i += inc) {
      // Calculate pair of coordinates for segment start
      float sx = cos((i - 90) * 0.0174532925);
      float sy = sin((i - 90) * 0.0174532925);
      uint16_t x0 = sx * (r - w) + x;
      uint16_t y0 = sy * (r - w) + y;
      uint16_t x1 = sx * r + x;
      uint16_t y1 = sy * r + y;

      // Calculate pair of coordinates for segment end
      float sx2 = cos((i + seg - 90) * 0.0174532925);
      float sy2 = sin((i + seg - 90) * 0.0174532925);
      int x2 = sx2 * (r - w) + x;
      int y2 = sy2 * (r - w) + y;
      int x3 = sx2 * r + x;
      int y3 = sy2 * r + y;

      if (i < v) { // Fill in coloured segments with 2 triangles
        switch (scheme) {
          case 0: colour = TFT_RED; break; // Fixed colour
          case 1: colour = TFT_GREEN; break; // Fixed colour
          case 2: colour = TFT_BLUE; break; // Fixed colour
          case 3: colour = rainbow(map(i, -angle, angle, 0, 127)); break; // Full spectrum blue to red
          case 4: colour = rainbow(map(i, -angle, angle, 70, 127)); break; // Green to red (high temperature etc)
          case 5: colour = rainbow(map(i, -angle, angle, 127, 63)); break; // Red to green (low battery etc)
          default: colour = TFT_BLUE; break; // Fixed colour
        }
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, colour);
        tft.fillTriangle(x1, y1, x2, y2, x3, y3, colour);
        //text_colour = colour; // Save the last colour drawn
      }
      else // Fill in blank segments
      {
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, TFT_GREY);
        tft.fillTriangle(x1, y1, x2, y2, x3, y3, TFT_GREY);
      }
    }
    // Convert value to a string
    char buf[10];
    byte len = 4; if (value > 999) len = 6;
    dtostrf(value, len, 1, buf);
    buf[len] = ' '; buf[len+1] = 0; // Add blanking space and terminator, helps to centre text too!
    // Set the text colour to default
    tft.setTextSize(1);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    // Uncomment next line to set the text colour to the last segment value!
    tft.setTextColor(colour, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    // Print value, if the meter is large then use big font 8, othewise use 4
    if (r > 84) {
      tft.setTextPadding(55*4); // Allow for 4 digits each 55 pixels wide
      tft.drawString(buf, x, y, 8); // Value in middle
    }
    else {
      tft.setTextPadding(4 * 14); // Allow for 4 digits each 14 pixels wide
      tft.drawString(buf, x, y, 4); // Value in middle
    }
    tft.setTextSize(1);
    tft.setTextPadding(0);
    // Print units, if the meter is large then use big font 4, othewise use 2
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (r > 84) tft.drawString(units, x, y + 60, 4); // Units display
    else tft.drawString(units, x, y + 15, 2); // Units display

    // Calculate and return right hand side x coordinate
    return x + r;
  }

void updateTFT() {
    tft.setTextSize(1);
    tft.setTextPadding(82);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    if (current != lastCurrent || voltage != lastVoltage) {
      dtostrf(abs(current * voltage), 1, 0, StatString);
      sprintf(post, "%sW", StatString);
      tft.drawString(post, 245, 170, 2);
    }

    if (current != lastCurrent || balancerOn != lastBalancerOn) {
      ringMeter(abs(current), 0, maxCurrent, 4, 5, 76, "Amps", GREEN2RED); // Draw ring meter
      tft.setTextDatum(TL_DATUM);
      tft.setTextSize(1);
      tft.setTextPadding(100);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      if (current > 0 && balancerOn == 0) {
        tft.drawString("Charging", 68, 170, 2);
      }
      else if (current > 0 && balancerOn == 1) {
        tft.drawString("Charging (B)", 68, 170, 2);
      }
      else if (current < 0 && balancerOn == 0) {
        tft.drawString("Discharging", 68, 170, 2);
      }
      else if (current < 0 && balancerOn == 1) {
        tft.drawString("Discharging (B)", 68, 170, 2);
      }
      else if (current == 0 && balancerOn == 0) {
        tft.drawString("Standby", 68, 170, 2);
      }
      else if (current == 0 && balancerOn == 1) {
        tft.drawString("Standby (B)", 68, 170, 2);
      }
      lastCurrent = current;
      lastBalancerOn = balancerOn;
    }
    
    if (voltage != lastVoltage) {
      ringMeter(voltage, minVoltage, maxVoltage, 167, 5, 76, "Volts", BLUE2RED); // Draw ring meter
      lastVoltage = voltage;
    }

    tft.setTextSize(1);
    tft.setTextPadding(82);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    if (highestCell != lastHighestCell) {
      dtostrf(highestCell, 1, 3, StatString);
      sprintf(post, "%d: %sv", highestCellNumber, StatString);
      tft.drawString(post, 68, 185, 2);
      lastHighestCell = highestCell;
    }

    if (lowestCell != lastLowestCell) {
      dtostrf(lowestCell, 1, 3, StatString);
      sprintf(post, "%d: %sv", lowestCellNumber, StatString);
      tft.drawString(post, 68, 200, 2);
      lastLowestCell = lowestCell;
    }

    if (cellVoltageDelta != lastCellVoltageDelta) {
      dtostrf(cellVoltageDelta, 1, 3, StatString);
      sprintf(post, "%sv", StatString);
      tft.drawString(post, 97, 215, 2);
      lastCellVoltageDelta = cellVoltageDelta;
    }

    if (temps[1] != lastTemp1) {
      dtostrf(temps[1], 1, 1, StatString);
      sprintf(post, "%sc", StatString);
      tft.drawString(post, 245, 185, 2);
      lastTemp1 = temps[1];
    }
    
    if (temps[2] != lastTemp2) {
      dtostrf(temps[2], 1, 1, StatString);
      sprintf(post, "%sc", StatString);
      tft.drawString(post, 245, 200, 2);
      lastTemp2 = temps[2];
    }

    if (percentRem != lastPercentRem) {
      dtostrf(percentRem, 1, 0, StatString);
      sprintf(post, "%s%%", StatString);
      tft.drawString(post, 245, 215, 2);
      lastPercentRem = percentRem;
    }
  }
#endif

void postData (const char *postData) {
  ArduinoOTA.handle();
  TelnetPrint.print("Posting to InfluxDB: "); TelnetPrint.println(postData);

  #ifdef UDP_MODE
    udp.beginPacket(influxhost, influxport);
    udp.printf(postData);
    udp.endPacket();
    delay(5); //This is required to allow the UDP transmission to complete
  #endif

  #ifdef HTTP_MODE
    WiFiClient client;
    HTTPClient http;
    http.begin(client, SECRET_INFLUXDB);
    http.addHeader("Content-Type", "text/plain");
    
    int httpResponseCode = http.POST(postData);
    //httpResponseCode = 200;
    yield(); //For some reason this delay is critical to the stability of the ESP.


    if (httpResponseCode >= 200 && httpResponseCode < 300){ //If the HTTP post was successful
      String response = http.getString(); //Get the response to the request
      //Serial.print("HTTP POST Response Body: "); Serial.println(response);
      TelnetPrint.print("HTTP POST Response Code: "); TelnetPrint.println(httpResponseCode);

      if (failures >= 1) {
        failures--; //Decrement the failure counter.
      }
    }

    else {
      TelnetPrint.print("Error sending HTTP POST: "); TelnetPrint.println(httpResponseCode);
      if (httpResponseCode <= 0) {
        failures++; //Incriment the failure counter if the server couldn't be reached.
      }
    }
    http.end();
    client.stop();
  #endif
}


void processPacket() //This function deciphers the BMS data.
{
  int checkSumCalc = 0x0;
  int checkSumData = 0x1;
  
  for (int i = 2; i < (packetCount-3); i++) //Add all the data together, skipping the header and command code at the start (first 2 bytes), and the checksum and footer at the end (last 3 bytes).
  {
      //Serial.printf("%x ", packetbuff[i]);
      checkSumCalc += packetbuff[i];
  }
  
  checkSumCalc = 65536 - checkSumCalc;
  checkSumData = (packetbuff[packetCount-3] << 8 | packetbuff[packetCount-2]); //OR the checksum bytes provided in the data packet together to find the total.
  
  
  if (checkSumCalc == checkSumData) {
    //TelnetPrint.print("Checksum "); TelnetPrint.print(checkSumCalc);  TelnetPrint.println(F(" matches. "));
    //Second byte: 0x3 is BMS status information. 0x4 is battery voltages.
    if (packetbuff[1] == 0x3) {
      voltage = (packetbuff[4] << 8 | packetbuff[5])/100.0;
      TelnetPrint.printf("Pack voltage: %0.3fv \r\n", voltage);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(voltage, 1, 2, StatString);
        sprintf(post, "Voltage,sensor=bms value=%s", StatString);
        postData(post);
      }

      current = 0.0;
      int current2s = packetbuff[7] | (packetbuff[6] << 8); //Combine both values.
      if (current2s >> 15 == 1) { //Find the most significant bit to determine if the number is negative (2's complement).
        current = (0 - (0x10000 - current2s))/100.0; //Calculate the negative number (2's complement).
      }
      else { //Otherwise calculate the positive number.
        current = current2s/100.0;
      }
      currentAv.addValue(current);
      TelnetPrint.printf("Current: %0.2f \r\n", current);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(currentAv.getAverage(), 1, 2, StatString);
        sprintf(post, "Current,sensor=bms value=%s", StatString);
        postData(post);
        currentAv.clear();
      }

      capacityRem = (packetbuff[8] << 8 | packetbuff[9])/100.0;
      TelnetPrint.printf("Remaining capacity: %0.2f \r\n", capacityRem);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(capacityRem, 1, 2, StatString);
        sprintf(post, "Capacity_Remaining,sensor=bms value=%s", StatString);
        postData(post);
      }
      
      capacityTyp = (packetbuff[10] << 8 | packetbuff[11])/100.0;
      TelnetPrint.printf("Typical capacity: %0.2f \r\n", capacityTyp);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(capacityTyp, 1, 2, StatString);
        sprintf(post, "Capacity_Typical,sensor=bms value=%s", StatString);
        postData(post);
      }

      
      cycles = (packetbuff[12] << 8 | packetbuff[13]);
      TelnetPrint.printf("Cycles: %0.2f \r\n", cycles);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(cycles, 1, 2, StatString);
        sprintf(post, "Cycles,sensor=bms value=%s", StatString);
        postData(post);
      }

      protectionStatus = (packetbuff[20] << 8 | packetbuff[21]);
      TelnetPrint.printf("Protection Status: %d \r\n", protectionStatus);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(protectionStatus, 1, 2, StatString);
        sprintf(post, "Protection_Status,sensor=bms value=%s", StatString);
        postData(post);
      }

      percentRem = packetbuff[23];
      TelnetPrint.printf("Percent Remaining: %d%% \r\n", percentRem);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(percentRem, 1, 2, StatString);
        sprintf(post, "Percent_Remaining,sensor=bms value=%s", StatString);
        postData(post);
      }
      
      cells = packetbuff[25];
      TelnetPrint.printf("Cells: %d \r\n", cells);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(cells, 1, 0, StatString);
        sprintf(post, "Cells,sensor=bms value=%s", StatString);
        postData(post);
        systemCycles = 0;
      }

      NTCs = packetbuff[26];
      TelnetPrint.printf("NTCs: %d \r\n", NTCs);
      if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
        dtostrf(NTCs, 1, 2, StatString);
        sprintf(post, "NTCs,sensor=bms value=%s", StatString);
        postData(post);
      }
      
      for (int i = 0; i < NTCs; i++) {
          temps[i] = ((packetbuff[(2*i)+27] << 8 | packetbuff[(2*i)+28]) * 0.1) - 273.1;
          TelnetPrint.printf("NTC %d temp: %0.1fc \r\n", i, temps[i]);
          if (systemCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
            dtostrf(temps[i], 1, 2, StatString);
            sprintf(post, "NTC_%d,sensor=bms value=%s", i, StatString);
            postData(post);
          }
      }

      TelnetPrint.println(); TelnetPrint.println();
      systemCycles++;
    }

    else if (packetbuff[1] == 0x4) {
      cellCount = (packetCount - 7)/2; // The number of cells is the size of the array in 16 bit pairs, minus header, footer and checksum.
      TelnetPrint.printf("Calculated cell count: %d \r\n", cellCount);
      float cellVoltages[cellCount];
      lowestCell = 10.0;
      highestCell = 0.0;
      for (int i = 0; i < cellCount; i++) {
        cellVoltages[i] = (packetbuff[(2*i)+4] << 8 | packetbuff[(2*i)+5])/1000.0;
        
        if (cellVoltages[i] < lowestCell) {
          lowestCell = cellVoltages[i];
          lowestCellNumber = i + 1;
        }
        if (cellVoltages[i] > highestCell) {
          highestCell = cellVoltages[i];
          highestCellNumber = i + 1;
        }

        TelnetPrint.printf("Cell %d: %0.3fv \r\n", (i+1), cellVoltages[i]);
        TelnetPrint.printf("Lowest cell: %d  Highest cell: %d \r\n", lowestCellNumber, highestCellNumber);

        if (battCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.
          dtostrf(cellVoltages[i], 1, 3, StatString);
          sprintf(post, "Cell_%d_Voltage,sensor=bms value=%s", (i+1), StatString);
          postData(post);
        }
      }

      cellVoltageDelta = highestCell - lowestCell;
      if (battCycles >= avSamples) { //If we have enough samples added to the running average, send the data to InfluxDB.  
        dtostrf(cellVoltageDelta, 1, 3, StatString);
        sprintf(post, "Cell_Voltage_Delta,sensor=bms value=%s", StatString);
        postData(post);
        battCycles = 0;
      }

      TelnetPrint.println(); TelnetPrint.println();
      battCycles++;
    }

    else {
      for (int i = 0; i < BMSRequestsNum; i++) {
        if (packetbuff[1] == BMSRequests[i].request[2]) { //Check if the packet buffer contains an identifier in our lists of BMS requests.
          BMSRequests[i].value = (packetbuff[4] << 8 | packetbuff[5]);
          if (i ==2 || i == 3 || i == 4 || i == 5) { //Kelvin values for temperature must be converted to celsius.
            BMSRequests[i].value = (BMSRequests[i].value/10) - 273.15;
          }
          TelnetPrint.printf("%s: %ld\r\n", BMSRequests[i].name, BMSRequests[i].value);
          break;
        }
      }
    } 
  }
  else {
      TelnetPrint.printf("Checksum doesn't match. Calulated %d, received %d. \r\n", checkSumCalc, checkSumData);
  }
}

void receiveData() {
  if (JBDSoftSerial.available()) {
    incomingByte = JBDSoftSerial.read();
	
	  //Receiving data requires a state machine to track the start of packet, packet type, reception in progress and reception completed.
    // packetStatus = 0 means we're waiting for the start of a packet to arrive.
    // packetStatus = 1 means that we've found what looks like a packet header, but need to confirm with the next byte.
    // packetStatus = 2 means we've received a data packet containing system parameters (e.g. high temperature cut-off)
    // packetStatus = 3 means we've received a system status packet
    // packetStatus = 4 means we've received a battery status packet.

    //TelnetPrint.printf("Received: %X \r \n", incomingByte);
    if (incomingByte == 0xdd && packetStatus == 0) {
      //TelnetPrint.println(F("0xdd found"));
      packetbuff[0] = incomingByte;
      packetCount++;
      packetStatus = 1; //Possible start of packet found.
    }
    else if (packetStatus == 1) {
      if (incomingByte >= 0x03 && incomingByte <= 0x29) {
        //TelnetPrint.printf("Start of packet found: %X \r \n", incomingByte);
        if (incomingByte == 0x03) {
          packetStatus = 3; //Start of system status packet confirmed.
        }
        else if (incomingByte == 0x04) {
          packetStatus = 4; //Start of battery status packet confirmed.
		}
        else {
          packetStatus = 2; //Start of parameter packet confirmed.
        }
        packetbuff[packetCount] = incomingByte;
        packetCount++;
        packetLength = 100;
      }
      else {
        JBDSoftSerial.flush();
        packetStatus = 0;
        packetCount = 0;
        //TelnetPrint.println(F("Not a status or parameter packet, ignoring."));
      }
    }
    else if (packetCount > packetLength && (packetStatus == 2 || packetStatus == 3 || packetStatus == 4)) { //If more than the expected number of bytes have been received, we must have missed the last byte of the packet. Restart.
      JBDSoftSerial.flush();
      packetStatus = 0;
      packetCount = 0;
      TelnetPrint.println(F("Too many bytes received - resetting."));
    }
    else if (incomingByte == 0x77 && packetCount == packetLength && (packetStatus == 2 || packetStatus == 3 || packetStatus == 4)) { //End of packet found.
      //TelnetPrint.printf("End of packet found: %X \r \n", incomingByte);
      packetbuff[packetCount] = incomingByte;
      packetCount++;
      processPacket();
      packetCount = 0;
      packetStatus = 0;
      JBDSoftSerial.flush();
    }
    else if (packetStatus == 2 || packetStatus == 3 || packetStatus == 4) {
      packetbuff[packetCount] = incomingByte;
      packetCount++;
      if (packetCount == 4) {
        packetLength = incomingByte + 6; //The 4th byte indicates the expected packet length, minus the header, checksum and footer.
        Serial1.print("Expected length of packet: "); Serial1.println(packetLength);
      }
    }
  }
}

void sendRequest() {
  if ((unsigned long)(millis() - lastRequest) >= 500) { // Don't send commands to the BMS too quickly.
    if ((unsigned long)(millis() - BMSRequests[statSwitch].LastRequest) >= BMSRequests[statSwitch].RequestFrequency && packetStatus == 0) { //Only request each stat as often as we need, and don't request when a receive is already underway.
      for (int j = 0; j < 7; j++) {
        //TelnetPrint.printf("0x%X ", BMSRequests[statSwitch].request[j]);
        JBDSoftSerial.write(BMSRequests[statSwitch].request[j]);
      }
      TelnetPrint.println();
      BMSRequests[statSwitch].LastRequest = millis();
      lastRequest = millis();
    }
    if (statSwitch < BMSRequestsNum - 1) {
      statSwitch++;
    }
    else {
      statSwitch = 0;
    }
  }
}


void loop() {
  ArduinoOTA.handle();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting to reconnect... ");
    failures++;
    WiFi.disconnect();
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    connectTime = millis();
    while (!WiFi.isConnected() && (unsigned long)(millis() - connectTime) < 5000) { //Wait for the wifi to connect for up to 5 seconds.
      delay(100);
      Serial.print(".");
    }
  }
    
  if (failures >= 40) {  //Reboot the ESP if there's been too many problems retrieving or sending the data.
    Serial.print("Too many failures, rebooting...");
    TelnetPrint.print("Failure counter has reached: "); TelnetPrint.print(failures); TelnetPrint.println(F(". Rebooting..."));
    ESP.restart();
  }

  if ((unsigned long)(millis() - lastUnlock) >= 20000) { //This command must be sent regularly to allow querying of parameters.
    JBDSoftSerial.write(0xdd); JBDSoftSerial.write(0x5a); JBDSoftSerial.write(0x0); JBDSoftSerial.write(0x02); JBDSoftSerial.write(0x56); JBDSoftSerial.write(0x78); JBDSoftSerial.write(0xff); JBDSoftSerial.write(0x30); JBDSoftSerial.write(0x77);
    lastUnlock = millis();

    if (highestCell >= 3.4 && balancerOn == 0){ //Turn the external balancer ON if the highest cell voltage is above x
      digitalWrite(BALANCER, 1);
      balancerOn = 1;
    }
    else if (highestCell < 3.35 && balancerOn == 1){ //Turn the external balancer OFF if the highest cell voltage is above
      digitalWrite(BALANCER, 0);
      balancerOn = 0;
    }
  }

  sendRequest();
  receiveData();
  delay(1); //For some reason this is critical to the stability of the ESP
  #ifdef TFT_ENABLE
    updateTFT();
  #endif
}
