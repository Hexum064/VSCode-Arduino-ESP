#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <SPI.h>
#include "LiquidCrystal.h"
#include <FS.h>


#define SERIAL_SPEED 115200
#define MAX_INPUT_LEN 256

#define LED_SPI_SPEED 1000000
#define LED_COUNT   24
#define LED_START_BYTE 0xE0 //first byte of the 32 bits for each APA102c LED. Needs to start with 0b111xxxxx where xxxxx is the brightness
#define LED_LUM 0x07
#define LED_SPI_MOSI  13
#define LED_SPI_SLK   14

#define LCD_COLS 20
#define LCD_ROWS 4

#define LCD_RS     16
#define LCD_EN     2
#define LCD_D4     5
#define LCD_D5     12
#define LCD_D6     4
#define LCD_D7     15

#define SETTINGS_FILE "settings.txt"
#define USER_ID_FILE "userIds.txt"
#define PASSWORD_FILE "password.bin"

#define KEY "f72de5a6-2195-4e4b-9e35-76e21c6a4ddb"


#define USER_ID_COUNT 16
#define USER_ID_MAX_LEN 36

#define DEFAULT_USER_ID "18096604-508b-422b-b58c-fe22f43c89d0"

#define SERVER_PORT 80
#define MAX_WIFI_CONNECT_RETRY_TIME 20

#define SCROLL_SPEED 20 //10 * 100ms = 1s
#define IP_DISPLAY_TIME 30 //30 * 100ms = 3s
#define FLASH_SPEED 5 //5 * 100ms = 0.5s

#define MAX_MESSAGE_LEN 240 //characters

struct Settings
{
  String ssid;
  String pw;
  bool useDHCP;
  String ipAddress;
  String subnet;
  String gateway;
};

enum DisplayStates
{
  StartDisplayingColor,
  FlashingColor,
  DisplayingColor,
  StopDisplayingColor,
  DoNothing,
};

enum DisplayIpStates
{
  StartDisplayingIp,
  DisplayingIp,
  StopDisplayingIp,
  DoNothingIp,
};

class LinkedStrings
{
  public:
    String value;
    LinkedStrings * next;
};

LinkedStrings _messageListHead;
uint _lineCount = 0;
bool _smallMessageDisplayed = false;
os_timer_t _displayTimer;
void timerCallback(void *pArg);
void(* resetFunc) (void) = 0;//declare reset function at address 0
LiquidCrystal _lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
ESP8266WebServer server(SERVER_PORT);
Settings _settings;
String _userIds[USER_ID_COUNT];
bool _wasRestartedSinceSettingsUpdate = true;
os_timer_t _myTimer;
DisplayStates _displayState = DoNothing;
DisplayIpStates _displayIpState = DoNothingIp;
int _flashTime = 0;
int _displayTime = 0;
uint8_t _redVal = 0;
uint8_t _greenVal = 0;
uint8_t _blueVal = 0;
String _displayMessage;
uint _startingMessageIndex = 0;
bool _messageEnabled = false;

/********Utility Method Region*/
String getLine(File file)
{
    String line;
    
    while (file.available())
    {
      line = file.readStringUntil('\n');
      return line;       
    }    
    
    return line;      
}

char * xorString(const char * value, uint len)
{
  uint j = 0;

  char *cypher = (char *) malloc(sizeof(char) * len);
  String key = KEY;
  uint keyLen = key.length();

  for (uint i = 0; i < len; i++)  
  {
    cypher[i] = value[i] ^ key[j];
    j++;

    if (j == keyLen)
    {
      j = 0;
    }
  }

  return cypher;
}

void loadDefaultUserId()
{
  for (uint8_t i = 0; i < USER_ID_COUNT; i++)
  {
    _userIds[i] = DEFAULT_USER_ID;
  }
}

IPAddress convertStringToIPAddress(String ipString)
{
  IPAddress address;
  address.fromString(ipString);

  if (address.isV4())
  {
    return address;
  }
  
  return 0;
}

int findStringIgnoreCase(String source, String toFind, uint startIndex = 0)
{

  uint index = startIndex;
  uint sourceLen = source.length();
  uint toFindLen = toFind.length();

  while(index + toFindLen < sourceLen)
  {

    if (source.substring(index, index + toFindLen).equalsIgnoreCase(toFind))
    {
      return index;
    }

    index++;
  }

  return -1;
}

//Attempts to get a value from the input string for the given key. Returns nothing if not found or value is not surrounded by = and ;
String getValueFromInputString(String input, String key)
{
  String value;
  int index;
  int endIndex;

  //Jump out right away incase the input is empty so we don't have to worry about it causing any problems.
  if (input.isEmpty())
  {
    return value;
  }

  //First find the index of the key
  index = findStringIgnoreCase(input, key + "=");

#ifdef DEBUG
  Serial.printf("Index found at %d\n", index);
#endif

  if (index < 0)
  {
    return value;
  }

  //Need to move to the next index so we are at the start of the value
  index += key.length() + 1;

  //Find the end of the value, should be the next ;
  endIndex = input.indexOf(";", index);

  //If the endIndex <= index, there is no value
  if (endIndex <= index)
  {
    return value;
  }

#ifdef DEBUG

  Serial.print(key);
  Serial.print(": ");
  Serial.println(input.substring(index, endIndex));

#endif

  return input.substring(index, endIndex);

}

/********End Utility Method Region*/



//This method creates a Linked List of strings that represent the lines to display on the LCD.
//
void createDisplayLinesFromMessage()
{
  uint charIndex = 0;
  uint startCharIndex = 0;
  uint lastWhiteSpaceIndex = 0;
  uint len = _displayMessage.length();
  LinkedStrings *currentLine = &_messageListHead;
  char currentChar;

  _startingMessageIndex = 0;
  _messageListHead.value.clear();
  _messageListHead.next = 0;
  _lineCount = 0;
  _smallMessageDisplayed = false;
  _messageEnabled = false;

  while(len)
  {
    currentChar = _displayMessage[charIndex];
    
    if (currentChar == ' ')
    {
      lastWhiteSpaceIndex = charIndex;
    }

    //We have enough characters for a line
    if (currentLine->value.length() == LCD_COLS - 1)
    {
      // Serial.printf("Line %d before: %s\n", _lineCount, currentLine->value.c_str());

      //If we have found another white space before the end of this line, we should truncate the string there.    
      if (lastWhiteSpaceIndex > 0)
      {
    
        currentLine->value = currentLine->value.substring(0, lastWhiteSpaceIndex - startCharIndex);
        
        charIndex = lastWhiteSpaceIndex + 1;
        lastWhiteSpaceIndex = 0;  

      }

      //Settings startCharIndex so we can calculate the end index of the next line if we split on a whit space
      startCharIndex = charIndex;

      //Then we create a new Line and link it
      currentLine->next = new LinkedStrings();
      currentLine = currentLine->next;
      _lineCount++;
    }
    else
    {
      currentLine->value += currentChar;
      charIndex++;

      //We have reached the end of the message
      if (charIndex >= len)
      {
        _lineCount++;

#ifdef DEBUG
          currentLine = &_messageListHead;
          for(uint i = 0; i < _lineCount; i++)
          {

            Serial.println(currentLine->value);
            currentLine = currentLine->next;
            
          }
#endif
        _messageEnabled = true;
        return;
        
      }

    }
  }
}

void scrollMessage()
{
  LinkedStrings *currentLine = &_messageListHead;

  //This avoids re-displaying a message that is less than LCD_ROWS in lenght
  // if (_smallMessageDisplayed)
  // {
  //   return;
  // }

  if (_lineCount >= LCD_ROWS - 1 && _startingMessageIndex > 0)
  {

    //First move to start of message;
    for (uint i = 0; i < _startingMessageIndex; i++)
    {
      currentLine = currentLine->next;
    }
  }
  // else
  // {
  //   _smallMessageDisplayed = true;
  // }
  _lcd.clear();

  for(uint i = 0; i < LCD_ROWS && currentLine; i++)
  {
    _lcd.setCursor(0, i);
    _lcd.write(currentLine->value.c_str());
    currentLine = currentLine->next;
    
  }

  if (_lineCount > LCD_ROWS)
  {
    _startingMessageIndex++;
  }
  //restart one we reached the end. 
  //Because we are going all the way to _lineCount insteadn of _lineCount - COL_ROWS, the last line will scroll to the top before the message repeates.
  if (_startingMessageIndex >= _lineCount)
  {
    _startingMessageIndex = 0;
  }
}





//APA 102c start of frame: 0x00000000
void sendLED_SoF()
{
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
}

//APA 102c end of frame: 0xFF00
void sendLED_EoF()
{
  SPI.transfer(0xFF);
  SPI.transfer(0x00);
}

void setFullDisplayColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t lum = LED_LUM, uint8_t ledCount = LED_COUNT)
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

void clearDisplay()
{
   setFullDisplayColor(0, 0, 0, 0, LED_COUNT);
}












void handleHttpRoot()
{  
  server.send(200, "text/plain", "ESP BuildStatus Light v1.0, 2020");
}

void handleNotFound()
{
  server.send(404, "Oops. Looks like you entered a bad URL.");
}

bool isUserIdValid(String userId)
{
  
  if (!userId.isEmpty())
  {
    for(uint i = 0; i < USER_ID_COUNT; i++)
    {
      if (!_userIds[i].isEmpty() && _userIds[i].equals(userId))
      {
        return true;
      }
    }
  }
  return false;
}

void getDisplayStatus()
{
  String returnMsg = "Red: " + String(_redVal) + " Green: " + String(_greenVal) + " Blue: " + String(_blueVal) + " FlashTime left: " + String(_flashTime)
    + " DisplayTime left: " + String(_displayTime) + " Message: " + _displayMessage;
  server.send(200, returnMsg.c_str());
}



void startSetDisplayColor(uint8_t red, uint8_t green, uint8_t blue)
{
  _flashTime = server.arg("flashtime").toInt();
  _displayTime = server.arg("displaytime").toInt();
  _redVal = red;
  _greenVal = green;
  _blueVal = blue;
  //if neither time was set, default to full on infinite.
  if (!_flashTime && !_displayTime)
  {
    _displayTime = -1;
  }

  _displayState = StartDisplayingColor;

  getDisplayStatus();
}

void setDisplayRed()
{
  startSetDisplayColor(128, 0, 0);
}

void setDisplayGreen()
{
  startSetDisplayColor(0, 128, 0);
}

void setDisplayBlue()
{
  startSetDisplayColor(0, 0, 128);
}

void setDisplayYellow()
{
  startSetDisplayColor(64, 32, 0);
}

void setDisplayPurple()
{
  startSetDisplayColor(64, 0, 32);
}

void setDisplayWhite()
{
  startSetDisplayColor(32, 32, 32);
}

void setDisplayOff()
{
  //Make sure everything is cleared out
  _redVal = 0;
  _greenVal = 0;
  _blueVal = 0;
  _displayTime = 0;
  _flashTime = 0;
  _displayState = StopDisplayingColor;
  clearDisplay();

  getDisplayStatus();
}

void setDisplayColor()
{
  startSetDisplayColor(server.arg("red").toInt() & 0xFF, server.arg("green").toInt() & 0xFF, server.arg("blue").toInt() & 0xFF);
}

void setDisplayMessage()
{
  _displayMessage = server.arg("message");


  
  if (_displayMessage.length() == 0)
  {
    _lcd.clear();
  }
  else
  {
    if (_displayMessage.length() > MAX_MESSAGE_LEN)
    {
      _displayMessage = _displayMessage.substring(0, MAX_MESSAGE_LEN - 1);
    }

    createDisplayLinesFromMessage();
  }

  getDisplayStatus();

}


void handleHTTPRequest(void (*requestHandler)())
{
  if (requestHandler == 0)
  {
    handleNotFound();
    return;
  }

  if (!isUserIdValid(server.arg("userid")))
  {
    server.send(401, "The User Id was missing or was not a valid User Id.");
    return;
  }
  
  requestHandler();

  
}

void handleSetDisplayRed()
{
  handleHTTPRequest(setDisplayRed);
}

void handleSetDisplayGreen()
{
  handleHTTPRequest(setDisplayGreen);
}

void handleSetDisplayBlue()
{
  handleHTTPRequest(setDisplayBlue);
}

void handleSetDisplayYellow()
{
  handleHTTPRequest(setDisplayYellow);
}

void handleSetDisplayPurple()
{
  handleHTTPRequest(setDisplayPurple);
}

void handleSetDisplayWhite()
{
  handleHTTPRequest(setDisplayWhite);
}

void handleSetDisplayOff()
{
  handleHTTPRequest(setDisplayOff);
}

void handleSetDisplayColor()
{
  handleHTTPRequest(setDisplayColor);
}

void handleSetDisplayMessage()
{
  handleHTTPRequest(setDisplayMessage);
}

void handleGetDisplayStatus()
{
  handleHTTPRequest(getDisplayStatus);
}

 







void initLCD()
{
  
  _lcd.begin(20 ,4);
  _lcd.clear();
  _lcd.home();

  Serial.println("LCD Initialized.");
}

void initLED_SPI()
{  
  digitalWrite(14, 1);
  digitalWrite(13, 1);
  //SPI.setFrequency(LED_SPI_SPEED);
  SPI.begin();
  clearDisplay();
  Serial.println("LED display Initialized.");
}

bool initFS()
{
  if (SPIFFS.begin())
  {
    Serial.println("File system mounted.");
    return true;
  }
  else
  {
    Serial.println("Could not mount file system.");
    return false;
  }
}

void initTimer()
{
  os_timer_setfn(&_myTimer, timerCallback, NULL);
  os_timer_arm(&_myTimer, 100, true);
  Serial.println("Timer started.");
}

bool initWifi(Settings settings)
{
  int retrySeconds = 0;
  WiFi.mode(WIFI_STA);

  if (!settings.useDHCP)
  {
#ifdef DEBUG
    Serial.println("IP: '" + settings.ipAddress + "' Gateway: '" + settings.gateway + "' Subnet: '" + settings.subnet + "'");
#endif
    WiFi.config(convertStringToIPAddress(settings.ipAddress), convertStringToIPAddress(settings.gateway), convertStringToIPAddress(settings.subnet));

  }

  WiFi.begin(settings.ssid, settings.pw);
  
  Serial.print("Connecting to '" + settings.ssid + "'.");

  _lcd.setCursor(0, 0);
  _lcd.write("Connecting to ");
  _lcd.setCursor(0, 1);
  _lcd.write(settings.ssid.c_str());
  _lcd.setCursor(0, 2);

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);

    if (retrySeconds++ >= MAX_WIFI_CONNECT_RETRY_TIME)
    {
      _lcd.clear();
      _lcd.write("Could not connect");
      Serial.println("Could not connect.");
      return false;
    }
    else
    {
        _lcd.write(".");
        Serial.print(".");
    }
  }

  Serial.println("Connected.");
  Serial.println("IP: " + WiFi.localIP().toString());

  _lcd.clear();
  _lcd.write("Connected.");
  _displayIpState = StartDisplayingIp;

  if (settings.useDHCP)
  {
    Serial.println("Using DHCP.");
    _lcd.setCursor(0, 2);
    _lcd.write("Using DHCP.");
  }
 

  return true;


}

void initHTTPServer()
{
  if (MDNS.begin("esp-buildstatus-light")) 
  {
    Serial.println("MDNS responder started");
  }
  server.on("/", handleHttpRoot);
  server.on("/Display/Red", handleSetDisplayRed);
  server.on("/Display/Green", handleSetDisplayGreen);
  server.on("/Display/Blue", handleSetDisplayBlue);
  server.on("/Display/Yellow", handleSetDisplayYellow);
  server.on("/Display/Purple", handleSetDisplayPurple);
  server.on("/Display/White", handleSetDisplayWhite);
  server.on("/Display/Off", handleSetDisplayOff);  
  server.on("/Display/Color", handleSetDisplayColor);  
  server.on("/Display/Message", handleSetDisplayMessage); 
  server.on("/Display", handleGetDisplayStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("HTTP Server initialized.");
}









String loadPassword()
{
  File f = SPIFFS.open(PASSWORD_FILE, "r");
  String pwString;
  uint i = 0;
  uint len = f.size();
  char pwChars[len];
  char * newChars;

  if (f)
  {
    while(f.available())
    {
      pwChars[i++] = f.read();
    }

    newChars = xorString(pwChars, len);

    for (uint i = 0; i < len; i++)
    {
      pwString += newChars[i];
    }

    f.close();
  }

  return pwString;

}

bool loadSettings()
{
  File f = SPIFFS.open(SETTINGS_FILE, "r");


  if (!f)
  {
    Serial.println("Settings file could not be read. It may not yet exist.");
    return false;
  }

  _settings.ssid = getLine(f);

  _settings.useDHCP = getLine(f).toInt();
  
  if (!_settings.useDHCP)
  {
    _settings.ipAddress = getLine(f);
    _settings.subnet = getLine(f);
    _settings.gateway = getLine(f);
  }

  f.close();

  _settings.pw = loadPassword();

  Serial.println("Settings loaded.");

  return true;
}



void loadUserIds()
{
  File f = SPIFFS.open(USER_ID_FILE, "r");
  String pw;

  if (!f)
  {
    Serial.println("User Ids file could not be read. Using default Id.");   
    loadDefaultUserId();
    return;
  }

  for (uint8_t i = 0; i < USER_ID_COUNT; i++)
  {
    _userIds[i] = getLine(f);
  }

  f.close();

  Serial.println("User Ids loaded.");   

}










void savePassword(String password)
{
  uint len = password.length();

  char * xorPW = xorString(password.c_str(), len);
  File f = SPIFFS.open(PASSWORD_FILE, "w+");

  for (uint i = 0; i < len; i++)
  {
    f.write(xorPW[i]);
  }

  f.close();

}

void saveSettings(Settings settings)
{
    File f = SPIFFS.open(SETTINGS_FILE, "w+");

    //Using "write" instead of "println" to avoid the "\r"
    f.write(settings.ssid.c_str());
    f.write('\n');
  

    if (settings.useDHCP)
    {
      f.println("1");   
    }
    else
    {
      f.println("0");
      f.write(settings.ipAddress.c_str());
      f.write('\n');
      f.write(settings.subnet.c_str());         
      f.write('\n');
      f.write(settings.gateway.c_str());         
      f.write('\n');      
    }

    f.close();

    savePassword(settings.pw);

    _wasRestartedSinceSettingsUpdate = false;

    Serial.println("Settings saved.");
}

void saveUserIds()
{
    File f = SPIFFS.open(USER_ID_FILE, "w+");

    //Using "write" instead of "println" to avoid the "\r"
    for (uint8_t i = 0; i < USER_ID_COUNT; i++)
    {
       f.write(_userIds[i].c_str());           
       f.write('\n');
    }

    
    f.close();

    Serial.println("User Ids saved.");  
}








void restartHandler()
{
  _wasRestartedSinceSettingsUpdate = true;
  ESP.restart();
}

void setDisplayHandler(String input)
{
  _redVal = getValueFromInputString(input, "RED").toInt();
  _greenVal = getValueFromInputString(input, "GREEN").toInt();
  _blueVal = getValueFromInputString(input, "BLUE").toInt();
  _flashTime = getValueFromInputString(input, "FLASHTIME").toInt();
  _displayTime = getValueFromInputString(input, "DISPLAYTIME").toInt();

  _displayState = StartDisplayingColor;
}

void setMessageHandler(String input)
{
  String msg = getValueFromInputString(input, "MESSAGE");

  if (msg.length() > MAX_MESSAGE_LEN)
  {
    msg = msg.substring(0, MAX_MESSAGE_LEN - 1);
  }

  _displayMessage = msg;

  if (msg.length() == 0)
  {
    _lcd.clear();
  }

  createDisplayLinesFromMessage();
}

void setSettingsHandler(String input)
{
  String ssid;
  String password;
  String useDHCP;
  String ipAddress;
  String subnetMask;
  String gateway;
  bool bUseDHCP;
 
  //Get SSID
  ssid = getValueFromInputString(input, "SSID");

  if (ssid.isEmpty())
  {
    Serial.println("SSID not found in input. Settings not updated.");
    return;
  }

  //Get PW
  password = getValueFromInputString(input, "PW");

  if (password.isEmpty())
  {
    Serial.println("PW not found in input. Settings not updated.");
    return;
  }

  //Get USEDHCP
  useDHCP = getValueFromInputString(input, "USEDHCP");

  if (useDHCP.isEmpty())
  {
    Serial.println("USEDHCP not found in input. Settings not updated.");
    return;
  }

  //Parse useDHCP
  if (useDHCP.equalsIgnoreCase("FALSE"))
  {
    bUseDHCP = false;
  }
  else if (useDHCP.equalsIgnoreCase("TRUE"))
  {
    bUseDHCP = true;
  }
  else
  {
    Serial.println("USEDHCP was not set to either TRUE or FALSE. Settings not updated.");
    return;    
  }

  //We ignore any of the IP settings if we are using DHCP
  if (!bUseDHCP)
  {
    //Get IP
    ipAddress = getValueFromInputString(input, "IP");

    if (ipAddress.isEmpty())
    {
      Serial.println("IP not found in input. Settings not updated.");
      return;
    }  

    if (!convertStringToIPAddress(ipAddress))
    {
      Serial.println("IP was not in a valid v4 format (aaa.bbb.ccc.ddd). Settings not updated.");
      return;      
    }

    //Get SUBNET
    subnetMask = getValueFromInputString(input, "SUBNET");

    if (subnetMask.isEmpty())
    {
      Serial.println("SUBNET not found in input. Settings not updated.");
      return;
    }  

    if (!convertStringToIPAddress(subnetMask))
    {
      Serial.println("SUBNET was not in a valid v4 format (aaa.bbb.ccc.ddd). Settings not updated.");
      return;      
    } 

    //Get GATEWAY
    gateway = getValueFromInputString(input, "GATEWAY");

    if (gateway.isEmpty())
    {
      Serial.println("GATEWAY not found in input. Settings not updated.");
      return;
    }  

    if (!convertStringToIPAddress(gateway))
    {
      Serial.println("GATEWAY was not in a valid v4 format (aaa.bbb.ccc.ddd). Settings not updated.");
      return;      
    }      
  }

  _settings.ssid = ssid;
  _settings.pw = password;
  _settings.useDHCP = bUseDHCP;
  _settings.ipAddress = ipAddress;
  _settings.subnet = subnetMask;
  _settings.gateway = gateway;

  saveSettings(_settings);



}

void helpHandler()
{
  Serial.println("Help: commands are case-insensitive.");
  Serial.println("RESTART - soft reset.");
  Serial.println("HELP - display help.");
  Serial.println("SETSETTINGS - sets the network settings and required additional params:");
  Serial.println("\tSSID=<value>;PW=<password>;USEDHCP=<TRUE/FALSE>;IP=<v4ipaddress>;GATEWAY=<v4gameway>;SUBNET=<v4subnetmask>;");
  Serial.println("\tIP and SUBNET are not required if USEDHCP is false. RESTART should be called after using this command.");
  Serial.println("GETSTATUS - returns current network settings and status, display status, and message.");
  Serial.println("GETUSERIDS - returns the list of User Ids.");
  Serial.printf("SETUSERID - sets a specific user id. IDs are numbered 1 through %d and can be up to %d characters long. Requires additional params:\n", USER_ID_COUNT, USER_ID_MAX_LEN);
  //NOTE: Indexes are labeled 1 to 16 because String.ToInt returns 0 for invalid strings
  Serial.printf("\tINDEX=<1-%d>;ID=<value>;\n", USER_ID_COUNT);
  Serial.printf("\tThe ID cannot be blank and if it is longer than %d it will be truncated.\n", USER_ID_MAX_LEN);
  Serial.println("SETDISPLAY - sets the light display and requires optional params (params can be left blank but will be read as 0):");
  Serial.println("\tRED=<8bitVal>;GREEN=<8bitVal>;BLUE=<8bitVal>;FLASHTIME=<number>;DISPLAYTIME=<number>;");
  Serial.println("\tIf FLASHTIME is < 0, it will flash indefinitely, if it is 0, it will not flash, if it is > 0, it will flash for that many mS * 100.");
  Serial.println("\tWhen FLASHTIME is done. The display may turn on solid. If DISPLAYTIME < 0, it will turn solid indefinitely. If it is 0 it will not turn on.");
  Serial.println("\tIf it is > 0, it will turn on solid for tham many mS * 100.");
  Serial.println("SETMESSAGE - sets the message to display on the LCD and reuqires optional params (blank params will clear the LCD):");
  Serial.printf("\tMESSAGE=<message>; MESSAGE will be automatically capped at %d characters.\n", MAX_MESSAGE_LEN);
}

void getStatusHandler()
{
  if (!_wasRestartedSinceSettingsUpdate)
  {
    Serial.println("NOTE: Current settings have not been implemented. RESTART required.");
  }

  Serial.println("Settings:");
  Serial.println("\tSSID: " + _settings.ssid);
  Serial.print("\tUse DHCP: ");
  Serial.println(_settings.useDHCP ? "TRUE" : "FALSE");
  
  if (!_settings.useDHCP)
  {
    Serial.println("\tIP Address: " + _settings.ipAddress);
    Serial.println("\tSubnet Mask: " + _settings.subnet);
    Serial.println("\tGateway: " + _settings.gateway);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("WiFi Status: Connected");
    if (_settings.useDHCP)
    {
      Serial.println("\tAcquired IP Address: " + WiFi.localIP().toString());
    }
  }
  else
  {
        Serial.println("WiFi Status: Not Connected");
  }

  Serial.printf("Display Status: red=%d, green=%d, blue=%d, flastTime left=%d, displayTime left=%d\n", 
    _redVal, _greenVal, _blueVal, _flashTime, _displayTime);
  Serial.println("Current Message: " + _displayMessage);

}


void getUserIdsHandler()
{
  Serial.println("User Ids:");
  //NOTE: Indexes are labeled 1 to 16 because String.ToInt returns 0 for invalid strings
  for (uint8_t i = 0; i < USER_ID_COUNT; i++)
  {
    Serial.printf("%2d: '%s'\n", i + 1, _userIds[i].c_str());
  }
}

void setUserIdsHandler(String input)
{
  String index;
  String id;
  int i;

  //Get the INDEX
  index = getValueFromInputString(input, "INDEX");

  if (index.isEmpty())
  {
      Serial.println("INDEX not found in input. User Id not updated.");
      return;   
  }

  i = index.toInt();

  if (i < 1 || i > USER_ID_COUNT)
  {
      Serial.printf("INDEX was not a number between 1 and %d. User Id not updated.\n", USER_ID_COUNT);
      return;   
  }

  //Get the ID
  id = getValueFromInputString(input, "ID");
  if (id.isEmpty())
  {
      Serial.println("ID not found in input. User Id not updated.");
      return;   
  }  

  if (id.length() > USER_ID_MAX_LEN)
  {
    id = id.substring(0, USER_ID_MAX_LEN - 1);
  }

  Serial.printf("Updateing User Id %d with value '%s'\n", i, id.c_str());
  //Make sure to subtract 1 from i since i should start at 1
  _userIds[i - 1] = id;

  saveUserIds();

}

void parseSerialInput(String input)
{
  String inputUpper = input;
  inputUpper.toUpperCase();

  if (input.isEmpty())
  {
    return;
  }
  else if (inputUpper.startsWith("RESTART"))
  {
    restartHandler();
  }
  else if (inputUpper.startsWith("SETSETTINGS"))
  {
    setSettingsHandler(input);
  }
  else if (inputUpper.startsWith("HELP"))
  {
    helpHandler();
  }
  else if (inputUpper.startsWith("GETSTATUS"))
  {
    getStatusHandler();
  }
  else if (inputUpper.startsWith("GETUSERIDS"))
  {
    getUserIdsHandler();
  }
  else if (inputUpper.startsWith("SETUSERID"))
  {
    setUserIdsHandler(input);
  } 
  else if (inputUpper.startsWith("SETDISPLAY"))
  {
    setDisplayHandler(input);
  }
  else if (inputUpper.startsWith("SETMESSAGE"))
  {
    setMessageHandler(input);
  }             
  else
  {
    Serial.println("Command not recognized.");
  }
}


void handleSerialInput()
{
  static String input;
  char c;
  while (Serial.available() > 0)
  {
    c = Serial.read();

    if (c == '\n')
    {
      Serial.println("Cmd: " + input);
      parseSerialInput(input);
      input.clear();
    }
    else if (c == '\b')
    {
      if (input.length() > 1)
      {
        input = input.substring(0, input.length() - 1);
      }
      else if (input.length() == 1)
      {
        input.clear();
      }
    }
    else
    {
      if (input.length() < MAX_INPUT_LEN)
      {
        input += c;     
      }
    }
  }
}








void clearLCDLine(uint8_t line)
{
  if (line > LCD_ROWS - 1)
  {
    return;
  }

  _lcd.setCursor(0, line);

  for (uint i = 0; i < LCD_COLS; i++)
  {
    _lcd.write(" ");  
  }
  
}

void handleIpDiplayState()
{
  static uint ipDisplayTimer = 0;

  switch (_displayIpState)
  {
    case StartDisplayingIp:
      ipDisplayTimer = 0;
      clearLCDLine(1);
      _lcd.setCursor(0, 1);
      _lcd.write("IP: ");
      _lcd.write(WiFi.localIP().toString().c_str());
      _displayIpState = DisplayingIp;

      break;
    case DisplayingIp:

      if (ipDisplayTimer >= IP_DISPLAY_TIME)
      {
        _displayIpState = StopDisplayingIp;

      }
      ipDisplayTimer++;

      break;
    case StopDisplayingIp:
      clearLCDLine(1);
      _displayIpState = DoNothingIp;
      break;
    case DoNothingIp:
      //do nothing
      break;
  }

}

void handleDisplayState()
{
  static uint flashingTimer = 0;
  static bool flashOn = false;

  switch (_displayState)
  {
    case StartDisplayingColor:

      //The order of ops says we flash first then turn on the display full time, so in this initial state,
      //We check for the flash timer first. If it is not 0, it's intended to turn on infinitely or for a set amount of time.
      //Otherwise, we check the same scenario for the full on display (<0 means always on, 0 means off, >0 countdown to off)
      //Finally, if both "time" vars are 0, just turn off the display.
      if(_flashTime != 0)
      {
        flashingTimer = 0;
        flashOn = true;
        setFullDisplayColor(_redVal, _greenVal, _blueVal);
        _displayState = FlashingColor;
      }
      else if (_displayTime != 0)
      {
        setFullDisplayColor(_redVal, _greenVal, _blueVal);
        _displayState = DisplayingColor;
      }
      else
      {
        _displayState = StopDisplayingColor;
      }


    case FlashingColor:
      /* code */

      //If the _flashTime is 0, we finished flashing. Now we need to check if we move on to just displaying a color or turn off the display.
      //If the _flassTime is > 0, keep flashing
      //The unhandled scenario is if _flashTime < 0, in which case we just keep flashing.
      if (_flashTime == 0)
      {
        //Here, if _displayTime > 0, then we need to switch states to full on display mode.
        if (_displayTime != 0)
        {
          _displayState = DisplayingColor;
        }
        else
        {
          _displayState = StopDisplayingColor;
        }
      }
      else if (_flashTime > 0)
      {
        //We are still in flashing mode here and we are counting down the timer.
        _flashTime--;
      }

      //This statements handles switching the display on and off for flashing mode.
      //Because it only enters the body if the flashingTimer >= FLASH_SPEED, the contents are only executed onces per flash
      if (flashingTimer >= FLASH_SPEED)
      {
        flashingTimer = 0;
        
        if (flashOn)
        {
          clearDisplay();
          flashOn = false;
        }
        else
        {
          setFullDisplayColor(_redVal, _greenVal, _blueVal);
          flashOn = true;
        }
      }

      flashingTimer++;

      break;
    case DisplayingColor:
      
      //In this if statement, if _displayTime > 0, we are counting down to eventually turn off the display.
      //If _displayTime == 0, then the display should be turned off.
      //Else _displayTime must be < 0 so we just leave the display on and move to the do nothing state
      if (_displayTime > 0)
      {
          _displayTime--;  
      }
      else if (_displayTime == 0)
      {
        _displayState = StopDisplayingColor;
      }
      else
      {
        setFullDisplayColor(_redVal, _greenVal, _blueVal);
        _displayState = DoNothing;
      }

      break;
    case StopDisplayingColor:
      clearDisplay();
      _displayState = DoNothing;
      break;
    case DoNothing:
      //do nothing;
      break;                  

  }  
}

void handleMessageScrolling()
{
  static uint scrollTimer = 0;

  if (scrollTimer >= SCROLL_SPEED)
  {

    scrollTimer = 0;
    scrollMessage();
  }
  
  scrollTimer++;
  
}






void setup() {
  
  
  Serial.begin(SERIAL_SPEED);
  Serial.println("\nInitializing. Please wait.");

  //Init LED SPI should come before the LCD or it could interfere with the LDC as some pins are shared
  initLED_SPI();
  //LCD should be initialized early so connection status info can be displayed
  initLCD();



  //The File System needs to be mounted before we can load user ids and settings
  if (!initFS())
  {
    Serial.println("Initialization haulted.");
    return;
  }

  //Loading user ids first so that, if no User Id file exists, the defaults can be loaded
  //If we wait till after getSettings, the Ids will be blank if no network settings can be loaded
  loadUserIds();

  //Need to get the settings before trying to connect to wifi
  if (!loadSettings())
  {
    Serial.println("Without the settings file, the network connection info is unknown.\nInitialization haulted.");
    _lcd.clear();
    _lcd.home();
    _lcd.write("No Network Settings");
    return;
  }

  //Init the timer before wifi so we can use it to temporarily display value.
  initTimer();

  //Need to start WiFi before starting the server
  if (initWifi(_settings))
  {
    //Starting the HTTP server should be the last thing we do for initialization
    initHTTPServer();
  }

  //test
  // _redVal = 64;
  // _greenVal = 0;
  // _blueVal = 32;
  // _flashTime = 7;
  // _displayTime = 0;
  // _displayState = StartDisplayingColor;
  // _startingMessageIndex = 0;
  // _displayMessage = "Hello, World! My name is Branden Boucher!";// And I approve this very long, multiline message.";
  // createDisplayLinesFromMessage();
}

void loop() 
{
  server.handleClient();
  MDNS.update();
  //Try to leave this as is. No other code
}



void timerCallback(void *pArg) 
{
  handleIpDiplayState();
  handleDisplayState();
  //We only want to start displaying the message once we have received one
  if (_messageEnabled) 
  {
    handleMessageScrolling();
  }
  handleSerialInput();
} 

