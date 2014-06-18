
// include the library code:
#include <LiquidCrystal.h>
#include <OneWire.h>
#include <EEPROM.h>
#include <EtherCard.h>
#include <Serial.h>

//define out buttons. change these to suit your setup.
//which analogue input have you connected your button array to?
int buttonPin = A0;
//and where is your temperature sensor connected via 1-wire?
int tempPin = A1;
//the below is the pin that controls your heating element via a relay
int heatPin = A2;
//the below is the pin that controls your cooling element (or fridge compressor) via a relay
int coolPin = A3;

//tracks the operation mode
//0 = idle
//1 = cooling
//2 = heating
//3 = disabled (fridge on! useful for cooling hot wort)
int mode = 0;

//overshoot percentages
#define COLD_OVERSHOOT 0.1

//tracks time spent cooling
unsigned long timeCooling = 0;
unsigned long lastCool = 0;
unsigned long lastIdle = 0;
unsigned long lastHeat = 0;

//initialise the OneWire library on your temperature pin
OneWire onewire(tempPin);               

//used when reading from the 1-wire temperature sensor. stores the address of the first two DS18B20 sensors found on the bus.
byte internaltempAddr[8];
byte externaltempAddr[8];

//some sensible starting values. these are saved in the EEPROM.
float targetTemp = 21.0;
float currentinternalTemp;
float currentexternalTemp;
float tempDiff = 1.0;

//did we find a sensor? used during detection logic and loop to 
//prevent us from making any temperature based decisions if the 
//sensor was not detected.
boolean sensorsFound = false;
//are we mid-conversion? needed because we only update temperature
//once per second, however i use a 100 msec delay in the loop to keep the LCD
//and keypad responsive
int sensorConverting = false;

//change the below if you would like a less stupid MAC on your ethernet connection
static byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
//the ip you would like to reach the controller on
static byte ip[] = {192,168,1,6};
//your network's internet gateway
static byte gateway[] = {192,168,1,1};  

//a buffer for received packets
byte Ethernet::buffer[500];
BufferFiller bfill;

//this is set when we've correctly identified that a button is being pressed,
//so we can fire the button handler. this is reset once the button is handled and released
//to avoid super-rapid-repeated-button-presses whilst the button is being pressed.
int buttonPressed = 0;

//some constants to pretty up the code.
//magic numbers are the devil.
#define BUTTON_NONE               0 
#define BUTTON_RIGHT              1 
#define BUTTON_UP                 2 
#define BUTTON_DOWN               3 
#define BUTTON_LEFT               4 
#define BUTTON_SELECT             5 

//more constants just to help readability
#define MODE_IDLE                 0
#define MODE_COOL                 1
#define MODE_HEAT                 2
#define MODE_DISABLED             3

//if you need to change these, they're in milliseconds.
#define REST_COMPRESSOR 1780000
#define STARTUP_DELAY 30000
#define COMPRESSOR_MIN_RUN 30000

// initialize the library with the numbers of the interface pins you are using
// see the LiquidCrystal documentation for a more detailed explanation
// the below is a fairly standard wiring for an LCD shield, change these 
// if you've wired up a character LCD directly.
LiquidCrystal lcd( 8, 9, 4, 5, 6, 7 );

//pinched from uberfridge (www.elcojacobs.com)
float eepromReadFloat(int address){
   union u_tag {
     byte b[4];
     float fval;
   } u;   
   u.b[0] = EEPROM.read(address);
   u.b[1] = EEPROM.read(address+1);
   u.b[2] = EEPROM.read(address+2);
   u.b[3] = EEPROM.read(address+3);
   return u.fval;
}
 
void eepromWriteFloat(int address, float value){
   union u_tag {
     byte b[4];
     float fval;
   } u;
   u.fval=value;
 
   EEPROM.write(address  , u.b[0]);
   EEPROM.write(address+1, u.b[1]);
   EEPROM.write(address+2, u.b[2]);
   EEPROM.write(address+3, u.b[3]);
}

void saveSettings(void){
  if(targetTemp != eepromReadFloat(0)){
     eepromWriteFloat(0, targetTemp);
  }
  if(tempDiff != eepromReadFloat(4)){
     eepromWriteFloat(4, tempDiff);
  }
}
 
void loadSettings(void){
  //used to store EEPROM values for checking before loading
  float readTargetTemp;
  float readTempDiff;
  
  readTargetTemp = eepromReadFloat(0);
  if(readTargetTemp > 0 && readTargetTemp < 100) {
     targetTemp = readTargetTemp;
  }  
  
  readTempDiff = eepromReadFloat(4);
  if(readTempDiff > 0 && readTempDiff <= 10) {
     tempDiff = readTempDiff;
  }  
  
}

//this attempts to find a pair of temperature sensors on the 1wire bus.
//this checks for DS18B20 sensors. If your internal (in the wort or taped to
//the side of the vessel) sensor and external sensor (outside the fridge) are
//are being detected in the wrong order, modify the below to assign the first
//and second detected sensors to internaltempAddr and externaltempAddr
//in the order they are being detected.
void findTempSensors()
{
  int sensorCount = 0;
  int addrCount = 0;
  byte tempAddr[8];
  
  while (onewire.search(tempAddr)) {
     if (tempAddr[0] == 0x28) {
       sensorCount++;
       if (sensorCount > 1) {
         memcpy(internaltempAddr,tempAddr,8);
         sensorsFound = true;
       } else if (sensorCount > 0) {
         memcpy(externaltempAddr,tempAddr,8);
       }
     } 
   }
}

//read the temperatures!
void getCurrentTemps()
{
  int tempByte;
  int tempData[12];
  int16_t tempRaw;
  
  if (sensorsFound && (sensorConverting < 10) && (sensorConverting > 0)) {
    sensorConverting++;
  } else if (sensorsFound && (sensorConverting == 0)) {
      //kick off a temp conversion
      onewire.reset();
      onewire.select(externaltempAddr);
      onewire.write(0x44);
      onewire.reset();
      onewire.select(internaltempAddr);
      onewire.write(0x44);
      //reset quasi-timer
      sensorConverting = 1;
  } else if (sensorsFound && (sensorConverting > 9)) {
      //lets read internal then external sensors
      //we've hit one second (10 100 millisecond delays)
      sensorConverting = 0;
      //reset the bus
      onewire.reset();
      
      //read the external (first) sensor
      onewire.select(externaltempAddr);
      onewire.write(0xBE);
      //read all 9 bytes
      for (tempByte = 0; tempByte < 9; tempByte++) {
        tempData[tempByte] = onewire.read();
      }
      //convert to something useful!
      tempRaw = (tempData[1] << 8) | tempData[0];
      //read at correct resolution, based on config byte
      if ((tempData[4] & 0x60) == 0x00) {
        tempRaw = tempRaw & ~7;
      } else if ((tempData[4] & 0x60) == 0x20) {
        tempRaw = tempRaw & ~3;
      } else if ((tempData[4] & 0x60) == 0x40) {
        tempRaw = tempRaw & ~1;
      } 
      currentexternalTemp = (float)tempRaw / 16;
      
      //lets read external value
      onewire.reset();
      onewire.select(internaltempAddr);
      onewire.write(0xBE);
      //read all 9 bytes
      for (tempByte = 0; tempByte < 9; tempByte++) {
        tempData[tempByte] = onewire.read();
      }
      //convert to something useful!
      tempRaw = (tempData[1] << 8) | tempData[0];
      //read at correct resolution, based on config byte
      if ((tempData[4] & 0x60) == 0x00) {
        tempRaw = tempRaw & ~7;
      } else if ((tempData[4] & 0x60) == 0x20) {
        tempRaw = tempRaw & ~3;
      } else if ((tempData[4] & 0x60) == 0x40) {
        tempRaw = tempRaw & ~1;
      }
      currentinternalTemp = (float)tempRaw / 16;
      
  } else { 
    findTempSensors();
  }
}

//setup the various moving parts
void setup() {
  
  Serial.begin(9600);
  delay(5000);
  
  //read settings
  loadSettings();

  Serial.println("Starting Ethernet");
  //set up ethercard
  if (ether.begin(sizeof Ethernet::buffer, mac, 10) == 0)
    Serial.println( "Failed to access Ethernet controller");
  ether.staticSetup(ip);
  
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);
  lcd.setCursor(3,0);
  lcd.print("I:");
  lcd.setCursor(10,0);
  lcd.print("E:");
  lcd.setCursor(0,1);
  lcd.print("[I]");
  lcd.setCursor(3,1);
  lcd.print("T:");
  lcd.setCursor(10,1);
  lcd.print("D:");
  
  //turn both relays off, set as digital outputs
  pinMode(heatPin, OUTPUT);
  digitalWrite(heatPin, LOW);
  pinMode(coolPin, OUTPUT);
  digitalWrite(coolPin, LOW);
    
   //search for onewire temp sensor
   delay(5000);
   findTempSensors();
}

//this function controls the relays based on temperature and operation mode
void switchRelays() {
  if (mode == MODE_DISABLED) {
    //disable mode - 
    digitalWrite(coolPin, HIGH);
    digitalWrite(heatPin, LOW);
    mode = MODE_DISABLED;
  }
  else if (!sensorsFound) {
    //no sensors, no relay action. 
    digitalWrite(coolPin, LOW);
    digitalWrite(coolPin, LOW);
    mode = MODE_IDLE;
  } else if (mode == MODE_COOL) {
    //this is where we need to account for overshoot! your fridge will have
    //a certain lag period between the temperature of the refridgeration 
    //loop reaching target temperature, and the temperature of the wort 
    //reaching target temperature. We use the overshoot delay to cut off
    //the compressor before the wort cools to target temp, as the temperature
    //will continue to drop once the compressor is shut off. 
    //if the temperature is still higher than our target temp + estimated idle cooling (overshoot), 
    //or if the compressor has been running for less than 10 seconds, keep cooling
    if (currentinternalTemp - (currentinternalTemp * COLD_OVERSHOOT) >= (targetTemp - tempDiff) || (millis() - timeCooling < COMPRESSOR_MIN_RUN)) {
      digitalWrite(coolPin, HIGH);
      digitalWrite(heatPin, LOW);
      mode = MODE_COOL;
    } else {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, LOW);
      mode = MODE_IDLE;
      //set last cool counter
      lastCool = millis();
    }
  //if heating and temp higher than maximum, switch to idle, turn off relays
  //I just use a small lightbulb in the fridge for this, it works well!
  } else if (mode == MODE_HEAT) {
    if (currentinternalTemp <= (targetTemp + tempDiff)) {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, HIGH);
      mode = MODE_HEAT;
    } else {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, LOW);
      mode = MODE_IDLE;
      //set the last heat counter
      lastHeat = millis();
    }
  //if idle and temp inside is higher than outside, and temperature has not reached minimum, keep idling
  } else if (mode == MODE_IDLE) {
    if (currentinternalTemp >= (targetTemp - tempDiff) && currentexternalTemp <= currentinternalTemp && currentinternalTemp <= (targetTemp + tempDiff)) {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, LOW);
      mode = MODE_IDLE;
    //if idle and temp inside is lower than outside, and temperature has not reached maximum, keep idling
    } else if (currentinternalTemp <= (targetTemp + tempDiff) && currentexternalTemp >= currentinternalTemp) {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, LOW);
      mode = MODE_IDLE;
     //if idle and temp inside is lower than temp outside, but we are within startup delay period, or resting the compressor, idle
    } else if (currentinternalTemp >= (targetTemp + tempDiff) && currentexternalTemp >= currentinternalTemp && ((lastCool > 0 && (millis() - lastCool) < REST_COMPRESSOR) || millis() < STARTUP_DELAY)) {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, LOW);
      mode = MODE_IDLE;    
      //if idle and temp inside is lower than temp outside, and temperature has reached maximum, time to cool
    } else if (currentinternalTemp >= (targetTemp + tempDiff) && currentexternalTemp >= currentinternalTemp) {
      digitalWrite(coolPin, HIGH);
      digitalWrite(heatPin, LOW);
      mode = MODE_COOL;
      //set last idle counter
      lastIdle = millis();
      timeCooling = millis();
      //if idle and temp inside is higher than temp outsid, and temperature has reached minimum, time to heat
    } else if (mode == MODE_IDLE && currentinternalTemp <= (targetTemp - tempDiff) && currentexternalTemp <= currentinternalTemp) {
      digitalWrite(coolPin, LOW);
      digitalWrite(heatPin, HIGH);
      mode = MODE_HEAT;
      //set last idle counter
      lastIdle = millis();
    }
  } else {
    //default position is to idle, catch all in case of a logic error
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  }
}

//updates values on the character LCD
void updateLCD() {
    //show current temp
    lcd.setCursor(5,0);
    lcd.print(currentinternalTemp, 1);
    lcd.setCursor(12,0);
    lcd.print(currentexternalTemp, 1);
    //show target temp
    lcd.setCursor(5,1);
    lcd.print(targetTemp, 1);
    //show temp diff
    lcd.setCursor(12,1);
    lcd.print(tempDiff, 1);
   
    //show status
    lcd.setCursor(0,1);
    switch (mode)  {
      case MODE_IDLE :
        lcd.print("[I]");
        break;
      case MODE_HEAT :
        lcd.print(" H>");
        break;
      case MODE_COOL :
        lcd.print("<C ");
        break;
      case MODE_DISABLED :
        lcd.print("-D-");
        break;
    }
}

//outputs JSON to the network in response to a request
static word outputJSON() {
    //handle TCP client

    //convert internal temperature to string
    static char itempbuffer[6];
    dtostrf(currentinternalTemp, 5, 2, itempbuffer);
    //convert external temperature to string
    static char etempbuffer[6];
    dtostrf(currentexternalTemp, 5, 2, etempbuffer);
    //convert target temperature to string
    static char ttempbuffer[6];
    dtostrf(targetTemp, 5, 2, ttempbuffer);
    //convert temperature difference to string
    static char dtempbuffer[6];
    dtostrf(tempDiff, 5, 2, dtempbuffer);

    bfill = ether.tcpOffset();
    bfill.emit_p(PSTR(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Connection: close\r\n"
      "{\n"
      "  \"currentinternaltemp\": $S,\r\n"
      "  \"currentexternaltemp\": $S,\r\n"
      "  \"targettemp\": $S,\r\n"
      "  \"tempdiff\": $S,\r\n"
      "  \"mode\": $D,\r\n"
      "  \"lastcool\": $D,\r\n"
      "  \"lastidle\": $D,\r\n"
      "  \"lastheat\": $D,\r\n"
      "  \"timecooling\": $D\r\n"
      "}"),itempbuffer,etempbuffer,ttempbuffer,dtempbuffer,mode,lastCool,lastIdle,lastHeat,timeCooling);
    return bfill.position();
}

void loop() {

    //get temperature reading for primary temp
    getCurrentTemps();
    
    //handle keypad
    int button = getButton();
    handleButton(button);
    
    //temperature logic
    switchRelays();
    
    //update the LCD...
    updateLCD();
   
   //check for data on the Ethernet port 
   word len = ether.packetReceive();
   word pos = ether.packetLoop(len);
  
   if (pos)  // check if valid tcp data is received
     ether.httpServerReply(outputJSON()); // send web page data
   
   //small delay
   delay(100); 
}

int getButton() {
  int buttonValue = analogRead(buttonPin);
  
  if (buttonValue < 10) {
    buttonPressed++;
    return BUTTON_RIGHT;
  } else if (buttonValue < 155 && buttonValue > 135) {
    buttonPressed++;
    return BUTTON_UP;
  } else if (buttonValue < 339 && buttonValue > 319) {
    buttonPressed++;
    return BUTTON_DOWN;
  } else if (buttonValue < 515 && buttonValue > 495) {
    buttonPressed++;
    return BUTTON_LEFT;
  } else if (buttonValue < 751 && buttonValue > 731) {
    buttonPressed++;
    return BUTTON_SELECT;
  } else {
    buttonPressed = 0;
    return BUTTON_NONE;
  }
}

void handleButton(int button) {
  if (buttonPressed == 1) {
    switch (button)
     {
        case BUTTON_UP :
          targetTemp += 0.1;
          break;
        case BUTTON_DOWN :
          targetTemp -= 0.1;
          break;
        case BUTTON_LEFT :
          tempDiff -= 0.1;
          break;
        case BUTTON_RIGHT :
          tempDiff += 0.1;
          break;
        case BUTTON_SELECT :
          if (mode == MODE_DISABLED) {
            mode = MODE_IDLE;
          } else {
            mode = MODE_DISABLED;
          }
          break;
     }
    saveSettings();
  } 
}

