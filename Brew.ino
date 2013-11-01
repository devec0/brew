// include the library code:
#include <SPI.h>
#include <LiquidCrystal.h>
#include <OneWire.h>
#include <Ethernet.h>
#include <string.h>

//define out buttons. change these to suit your setup.
//which analogue input have you connected your button array to?
int buttonPin = A0;
//and where is your temperature sensor connected via 1-wire?
int tempPin = A1;
//the below is the pin that controls your heating element via a relay
int heatPin = A5;
//the below is the pin that controls your cooling element (or fridge compressor) via a relay
int coolPin = A4;

//tracks the operation mode
//0 = idle
//1 = cooling
//2 = heating
int mode = 0;

//initialise the OneWire library on your temperature pin
OneWire onewire(tempPin);

//all used for smoothing readings as they come in, to prevent spikes. 
//will implement this soon.
const int maxReadings = 10;
int numReadings = 0;
float internaltempReadings[maxReadings];   
float externaltempReadings[maxReadings];   
int readingIndex = 0;                  
float readingTotal = 0;                 
float readingAverage = 0;                

//used when reading from the 1-wire temperature sensor. stores the address of the first two DS18B20 sensors found on the bus.
byte internaltempAddr[8];
byte externaltempAddr[8];

//some sensible starting values. plan to use the SD card to save/restore these.
float targetTemp = 20.5;
float currentinternalTemp;
float currentexternalTemp;
float tempDiff = 2.5;

//did we find a sensor? used during detection logic and loop to 
//prevent us from making any temperature based decisions if the 
//sensor was not detected.
boolean sensorFound = false;
//are we mid-conversion? needed because we only update temperature
//once per second, however i use a 100 msec delay in the loop to keep the LCD
//and keypad responsive
int sensorConverting = false;

//change the below if you would like a less stupid MAC on your ethernet connection
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
//the ip you would like to reach the controller on
byte ip[] = {192,168,1,6};
//your network's internet gateway
byte gateway[] = {192,168,1,1};
//your network's subnet
byte subnet[] = {255,255,255,0};
//set up a server on port 80
EthernetServer server(80);

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


// initialize the library with the numbers of the interface pins you are using
// see the LiquidCrystal documentation for a more detailed explanationeeeeeeeeee
LiquidCrystal lcd( 8, 9, 4, 5, 6, 7 );

void findTempSensors()
{
  int sensorCount = 0;
  int addrCount = 0;
  byte tempAddr[8];
  
  Serial.println("Searching for temperature sensor");
  while (onewire.search(tempAddr)) {
     Serial.println("Found one-wire device:");
     for (addrCount = 0; addrCount < 8; addrCount++) {
       Serial.print(tempAddr[addrCount], HEX);
     }
     Serial.println();
     if (tempAddr[0] == 0x28) {
       Serial.println("Found DS18B20!");
       sensorCount++;
       if (sensorCount > 1) {
         memcpy(externaltempAddr,tempAddr,8);
         Serial.print("Found external sensor: ");
         for (addrCount = 0; addrCount < 8; addrCount++) {
           Serial.print(externaltempAddr[addrCount], HEX);
         }
         Serial.println();
         Serial.println("We've found both sensors.");
         sensorFound = true;
       } else if (sensorCount > 0) {
         memcpy(internaltempAddr,tempAddr,8);
         Serial.print("Found internal sensor: ");
         for (addrCount = 0; addrCount < 8; addrCount++) {
           Serial.print(internaltempAddr[addrCount], HEX);
         }
         Serial.println();
       }
     } else {
       Serial.println("Device was not a DS18B20.");
       Serial.print(tempAddr[addrCount],HEX);
     }
   }
}

void getCurrentTemps()
{
  int tempByte;
  int tempData[12];
  int16_t tempRaw;
  
  if (sensorFound && (sensorConverting < 40) && (sensorConverting > 0)) {
    //Serial.println("Incrementing sensor wait counter.");
    sensorConverting++;
  } else if (sensorFound && (sensorConverting == 0)) {
      Serial.println("Sending a conversion request to sensors.");
      //kick off a temp conversion
      onewire.reset();
      onewire.select(externaltempAddr);
      onewire.write(0x44);
      onewire.reset();
      onewire.select(internaltempAddr);
      onewire.write(0x44);
      //reset quasi-timer
      sensorConverting = 1;
  } else if (sensorFound && (sensorConverting > 9)) {
      //lets read internal then external sensors
      Serial.println("Reading sensors.");
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
      Serial.print("Read external value: ");
      Serial.print(tempRaw / 16);
      Serial.println();
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
      Serial.print("Read internal value: ");
      Serial.print(tempRaw / 16);
      Serial.println();
      currentinternalTemp = (float)tempRaw / 16;
      
  } else { 
    Serial.println("Sensor not found? Searching again...");
    findTempSensors();
  }
}

void setup() {
  
  //set up serial for debug
  Serial.begin(9600);
  
  //set up TCP server
  Ethernet.begin(mac, ip, gateway, subnet);
  server.begin();

  // set up the LCD's number of columns and rows:
  Serial.println("Setting up LCD"); 
  lcd.begin(16, 2);
  lcd.setCursor(2,0);
  lcd.print("I:");
  lcd.setCursor(9,0);
  lcd.print("E:");
  lcd.setCursor(0,1);
  lcd.print("[I]");
  lcd.setCursor(3,1);
  lcd.print("T:");
  lcd.setCursor(10,1);
  lcd.print("D:");
  
  //turn both relays off, set as digital outputs
  Serial.println("Turning relays off");
  pinMode(heatPin, OUTPUT);
  digitalWrite(heatPin, LOW);
  pinMode(coolPin, OUTPUT);
  digitalWrite(coolPin, LOW);
  
  //reset readings array
   for (int thisReading = 0; thisReading < maxReadings; thisReading++) {
    internaltempReadings[thisReading] = 0;  
    externaltempReadings[thisReading] = 0;  
   }
    
   //search for onewire temp sensor
   delay(5000);
   findTempSensors();
}

void switchRelays() {
  //if cooling and temp less than minimum, switch to idle, turn off relays
  if (mode == MODE_COOL && currentinternalTemp < (targetTemp - tempDiff)) {
    Serial.println("Idling because we've been cooling and we've reached target temperature.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if heating and temp higher than maximum, switch to idle, turn off relays
  } else if (mode == MODE_HEAT && currentinternalTemp > (targetTemp + tempDiff)) {
    Serial.println("Idling because we've been heating and we've reached target temperature.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if idle and temp inside is higher than outside, and temperature has not reached minimum, keep idling
  } else if (mode == MODE_IDLE && currentinternalTemp > (targetTemp - tempDiff) && currentexternalTemp < currentinternalTemp) {
    Serial.println("Keep idling because it's cooler outside and we're above minimum temp.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if idle and temp inside is lower than outside, and temperature has not reached maximum, keep idling
  } else if (mode == MODE_IDLE && currentinternalTemp < (targetTemp + tempDiff) && currentexternalTemp > currentinternalTemp) {
    Serial.println("Keep idling because it's hotter outside and we're below maximum temp.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if idle and temp inside is lower than temp outside, and temperature has reached maximum, time to cool
  } else if (mode == MODE_IDLE && currentinternalTemp > (targetTemp + tempDiff) && currentexternalTemp > (targetTemp + tempDiff)) {
    Serial.println("Start cooling because it's hotter outside and we're above maximum temp.");
    digitalWrite(coolPin, HIGH);
    digitalWrite(heatPin, LOW);
    mode = MODE_COOL;
  //if idle and temp inside is higher than temp outsid, and temperature has reached minimum, time to heat
  } else if (mode == MODE_IDLE && currentinternalTemp < (targetTemp - tempDiff) && currentexternalTemp < (targetTemp - tempDiff)) {
    Serial.println("Start heating because it's colder outside and we're below minimum temp.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, HIGH);
    mode = MODE_HEAT;
  } else if (mode == MODE_COOL && currentinternalTemp >= (targetTemp - tempDiff)) {
    Serial.println("Keep cooling, we haven't reached our goal.");
    digitalWrite(coolPin, HIGH);
    digitalWrite(heatPin, LOW);
    mode = MODE_COOL;
  } else if (mode == MODE_HEAT && currentinternalTemp <= (targetTemp + tempDiff)) {
    Serial.println("Keep heating, we haven't reached our goal.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, HIGH);
    mode = MODE_HEAT;
  } else {
    //default position is to idle
    Serial.println("No matched condition, keep idling.");
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  }
}

void updateLCD() {
    //show current temp
    lcd.setCursor(4,0);
    lcd.print(currentinternalTemp, 1);
    lcd.setCursor(11,0);
    lcd.print(currentexternalTemp, 1);
    //Serial.write(currentTemp);
    //show target temp
    lcd.setCursor(5,1);
    lcd.print(targetTemp, 1);
    //Serial.write(targetTemp);
    //show temp diff
    lcd.setCursor(12,1);
    lcd.print(tempDiff, 1);
    //Serial.write(tempDiff);
   
    //show status
    lcd.setCursor(0,1);
    switch (mode)  {
      case MODE_IDLE :
        lcd.print("[I]");
        break;
      case MODE_HEAT :
        lcd.print("H>>");
        break;
      case MODE_COOL :
        lcd.print("<<C");
        break;
    }
}

void checkClients() {
    //handle TCP client
    EthernetClient client = server.available();
    if (client) {
      // read bytes from the incoming client and write them back
      // to any clients connected to the server:
      boolean currentLineIsBlank = true;
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          Serial.write(c);
          // if you've gotten to the end of the line (received a newline
          // character) and the line is blank, the http request has ended,
          // so you can send a reply
          if (c == '\n' && currentLineIsBlank) {
            // send a standard http response header
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: application/json");
            client.println("Connection: close");  // the connection will be closed after completion of the response
            client.println();
            client.println("{");
            // output the current temp
            client.print("currentinternaltemp: ");
            client.print(currentinternalTemp);
            client.println(",");
            // output the current external temp
            client.print("currentexternaltemp: ");
            client.print(currentexternalTemp);
            client.println(",");
            // output the target temp
            client.print("targettemp: ");
            client.print(targetTemp);
            client.println(",");
            // output the temp difference
            client.print("tempdiff: ");
            client.print(tempDiff);
            client.println(",");
            // output the temp difference
            client.print("mode: ");
            client.print(mode);
            client.println("");
            //end json file
            client.println("}");
            break;
          }
          if (c == '\n') {
            // you're starting a new line
            currentLineIsBlank = true;
          } else if (c != '\r') {
            // you've gotten a character on the current line
            currentLineIsBlank = false;
        }
      }
    }
    // give the web browser time to receive the data
    delay(1);
    // close the connection:
    client.stop();
    }
}

void loop() {

    //get temperature reading for primary temp
    //Serial.println("Getting temperature");
    getCurrentTemps();

    //handle keypad
    int button = getButton();
    //Serial.println("Geting buttons");
    handleButton(button);
    
    //temperature logic
    switchRelays();
    
    //update the LCD...
    updateLCD();
    
    //handle tcp ip client
    checkClients();
   
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
          break;
     }
  } 
}

