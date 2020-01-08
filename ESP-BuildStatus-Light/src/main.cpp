#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <SPI.h>
#include "LiquidCrystal.h"
#include <FS.h>

#define LED_COUNT   10
#define LED_START_BYTE 0xE0 //first byte of the 32 bits for each APA102c LED. Needs to start with 0b111xxxxx where xxxxx is the brightness
#define LED_LUM 0x07
#define LED_SPI_MOSI  13
#define LED_SPI_SLK   14

#define LCD2_RS     16
#define LCD2_EN     2
#define LCD2_D4     5
#define LCD2_D5     4
#define LCD2_D6     13
#define LCD2_D7     15

#define SETTINGS_FILE "settings.txt"

struct Settings
{
  String ssid;
  String pw;
  bool useDNS;
  String ipAddress;
  String subnet;
};

LiquidCrystal lcd(LCD2_RS, LCD2_EN, LCD2_D4, LCD2_D5, LCD2_D6, LCD2_D7);

//APA 102c start of frame: 0x00000000
void sendLED_SoF()
{
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
}

//APA 102 end of frame: 0xFF00
void sendLED_EoF()
{
  SPI.transfer(0xFF);
  SPI.transfer(0x00);
}

void setFullDisplayColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t lum, uint8_t ledCount)
{
   sendLED_SoF();
  
  for(uint8_t i = 0; i < LED_COUNT; i++)
  {
    SPI.transfer(LED_START_BYTE | (lum & 0b00011111));
    SPI.transfer(blue); //Blue
    SPI.transfer(green); //Green
    SPI.transfer(red); //Red
  }

  sendLED_EoF(); 
}

void blankDisplay()
{
   setFullDisplayColor(0, 0, 0, 0, LED_COUNT);
}

void initLCD()
{

}

void initLED_SPI()
{

}

void initFS()
{

}

void initWifi()
{

}

void initHTTPServer()
{
  
}

Settings getSettings()
{

}

void saveSettings(Settings settings)
{

}

void restart()
{

}


void setup() {
  // put your setup code here, to run once:
}

void loop() {
  // put your main code here, to run repeatedly:

}