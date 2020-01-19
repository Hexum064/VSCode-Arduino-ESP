#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <SPI.h>
#include "LiquidCrystal.h"
#include <FS.h>

#define SERIAL_SPEED 115200

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

os_timer_t _displayTimer;

struct Settings
{
  String ssid;
  String pw;
  bool useDHCP;
  String ipAddress;
  String subnet;
  String gateway;
};


LiquidCrystal _lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
ESP8266WebServer server(SERVER_PORT);
Settings _settings;
String _userIds[USER_ID_COUNT];
bool _wasRestartedSinceSettingsUpdate = true;

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

bool initWifi(Settings settings)
{
  int retrySeconds = 0;
  WiFi.mode(WIFI_STA);

  if (!settings.useDHCP)
  {
    Serial.println("IP: '" + settings.ipAddress + "' Gateway: '" + settings.gateway + "' Subnet: '" + settings.subnet + "'");
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


  
    if (retrySeconds++ >= 20)
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
  _lcd.setCursor(0, 1);
  _lcd.write("IP: ");
  _lcd.write(WiFi.localIP().toString().c_str());

  if (settings.useDHCP)
  {
    Serial.println("Using DHCP.");
    _lcd.setCursor(0, 2);
    _lcd.write("Using DHCP.");
  }
 

  return true;

  //TODO: Show connected
  //TODO: Show IP Address if DHCP mode for 3 seconds
}

void initHTTPServer()
{

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

    for (uint8_t i = 0; i < USER_ID_COUNT; i++)
    {
       f.println(_userIds[i]);           
    }

    
    f.close();

    Serial.println("User Ids saved.");  
}

void restartHandler()
{
  _wasRestartedSinceSettingsUpdate = true;
  //TODO: Do restart
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
  //TODO: Implement SETDISPLAY and SETMESSAGE
  Serial.println("SETDISPLAY - not yet implemented.");
  Serial.println("SETMESSAGE - not yet implemented.");
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

  //TODO: Implement Display Status and Message Status
  Serial.println("Display Status: ");
  Serial.println("Message Status: ");

}


void getUserIdsHandler()
{
  Serial.println("User Ids:");
  //NOTE: Indexes are labeled 1 to 16 because String.ToInt returns 0 for invalid strings
  for (uint8_t i = 0; i < USER_ID_COUNT; i++)
  {
    Serial.printf("%2d: %s\n", i + 1, _userIds[i].c_str());
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
  else
  {
    Serial.println("Command not recognized.");
  }
}


void handleSerialInput()
{

  //TODO: Make sure static works here the way we think it will
  static String input;
  char c;
  while (Serial.available() > 0)
  {
    c = Serial.read();

    if (c == '\n')
    {
      parseSerialInput(input);
      input.clear();
    }
    else
    {
      input += c;     
    }
  }
}


void setup() {
  
  
  Serial.begin(SERIAL_SPEED);
  Serial.println("\nInitializing. Please wait.");

  //Init LED SPI should come before the LCD or it could interfere with the LDC as some pins are shared
  initLED_SPI();
  //LCD should be initialized early so connection status info can be displayed
  initLCD();


//test:
  setFullDisplayColor(64, 0, 32);
//end test

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

  //Need to start WiFi before starting the server
  if (initWifi(_settings))
  {
    //Starting the HTTP server should be the last thing we do for initialization
    initHTTPServer();
  }


}

void loop() {
  // put your main code here, to run repeatedly:
  handleSerialInput();
}