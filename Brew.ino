// include the library code:
#include <SPI.h>
#include <LiquidCrystal.h>
#include <OneWire.h>
#include <Ethernet.h>

//define out buttons. change these to suit your setup.
//which analogue input have you connected your button array to?
int buttonPin = A0;
//and where is your temperature sensor connected via 1-wire?
int tempPin = A1;
//the below is the pin that controls your heating element via a relay
int heatPin = 12;
//the below is the pin that controls your cooling element (or fridge compressor) via a relay
int coolPin = 13;

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
float tempReadings[maxReadings];   
int readingIndex = 0;                  
float readingTotal = 0;                 
float readingAverage = 0;                

//used when reading from the 1-wire temperature sensor. stores the address of the first DS18B20 sensor found on the bus.
byte tempAddr[8];

//some sensible starting values. plan to use the SD card to save/restore these.
float targetTemp = 20.5;
float currentTemp;
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

void getCurrentTemp()
{
  int tempByte;
  int tempData[12];
  int16_t tempRaw;
  
  if (sensorFound && (sensorConverting < 10) && (sensorConverting > 0)) {
    Serial.println("Incrementing sensor wait counter.");
    sensorConverting++;
  } else if (sensorFound && (sensorConverting == 0)) {
      Serial.println("Sending a conversion request to sensor.");
      //kick off a temp conversion
      onewire.reset();
      onewire.select(tempAddr);
      onewire.write(0x44);
      //reset quasi-timer
      sensorConverting = 1;
  } else if (sensorFound && (sensorConverting > 9)) {
      Serial.println("Reading sensor.");
      //we've hit one second (10 100 millisecond delays)
      sensorConverting = 0;
      //lets read the value
      onewire.reset();
      onewire.select(tempAddr);
      onewire.write(0xBE);
      //read all 9 bytes
      for (tempByte = 0; tempByte < 9; tempByte++) {
        tempData[tempByte] = onewire.read();
      }
      //convert to something useful!
      tempRaw = (tempData[1] << 8) | tempData[0];
      //read at correct resolution, based on config byte
      if ((tempData[4] & 0x60) == 0x00) {
        Serial.println("Reading 9bit value.");
        tempRaw = tempRaw & ~7;
      } else if ((tempData[4] & 0x60) == 0x20) {
        Serial.println("Reading 10bit value.");
        tempRaw = tempRaw & ~3;
      } else if ((tempData[4] & 0x60) == 0x40) {
        Serial.println("Reading 11bit value.");
        tempRaw = tempRaw & ~1;
      } else {
        Serial.println("Reading 12bit value.");
      }
      Serial.println("Read value.");
      Serial.print(tempRaw / 16);
      currentTemp = (float)tempRaw / 16;
  } else { 
    Serial.println("Sensor not found?");
  }
}

void setup() {
  int addrCount = 0;
  
  //set up serial for debug
  Serial.begin(9600);
  
  //set up TCP server
  Ethernet.begin(mac, ip, gateway, subnet);
  server.begin();

  // set up the LCD's number of columns and rows:
  Serial.println("Setting up LCD"); 
  lcd.begin(16, 2);
  lcd.setCursor(0,0);
  lcd.print("TT: ");
  lcd.setCursor(8,0);
  lcd.print("CT: ");
  lcd.setCursor(0,1);
  lcd.print("Init... ");
  lcd.setCursor(8,1);
  lcd.print("TD: ");
  
  //turn both relays off, set as digital outputs
  Serial.println("Turning relays off");
  pinMode(heatPin, OUTPUT);
  digitalWrite(heatPin, LOW);
  pinMode(coolPin, OUTPUT);
  digitalWrite(coolPin, LOW);
  
  //reset readings array
   for (int thisReading = 0; thisReading < maxReadings; thisReading++)
    tempReadings[thisReading] = 0;  
    
   //search for onewire temp sensor
   delay(5000);
   Serial.println("Searching for temperature sensor");
   while (onewire.search(tempAddr)) {
     Serial.println("Found one-wire device:");
     for (addrCount = 0; addrCount < 8; addrCount++) {
       Serial.print(tempAddr[addrCount], HEX);
     }
     Serial.println();
     if (tempAddr[0] == 0x28) {
       Serial.println("Found DS18B20!");
       sensorFound = true;
     } else {
       Serial.println("Device was not a DS18B20.");
       Serial.print(tempAddr[0],HEX);
     }
   }
}

void switchRelays() {
  //if cooling and temp less than minimum, switch to idle, turn off relays
  if (mode == MODE_COOL && currentTemp < (targetTemp - tempDiff)) {
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if heating and temp higher than maximum, switch to idle, turn off relays
  } else if (mode == MODE_HEAT && currentTemp > (targetTemp + tempDiff)) {
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, LOW);
    mode = MODE_IDLE;
  //if idle and temp higher than maxium, start cooling
  } else if (mode == MODE_IDLE && currentTemp > (targetTemp + tempDiff)) {
    digitalWrite(coolPin, HIGH);
    digitalWrite(heatPin, LOW);
    mode = MODE_COOL;
  //if idle and temp lower than minimum, start heating
  } else if (mode == MODE_IDLE && currentTemp < (targetTemp + tempDiff)) {
    digitalWrite(coolPin, LOW);
    digitalWrite(heatPin, HIGH);
    mode = MODE_HEAT;
  }
}

void updateLCD() {
    //show target temp
    lcd.setCursor(3,0);
    lcd.print(targetTemp, 1);
    //Serial.write(targetTemp);
    //show current temp
    lcd.setCursor(11,0);
    lcd.print(currentTemp, 1);
    //Serial.write(currentTemp);
    //show temp diff
    lcd.setCursor(11,1);
    lcd.print(tempDiff, 1);
    //Serial.write(tempDiff);
   
    //show status
    lcd.setCursor(0,1);
    switch (mode)  {
      case MODE_IDLE :
        lcd.print("Idle    ");
        break;
      case MODE_HEAT :
        lcd.print("Heating ");
        break;
      case MODE_COOL :
        lcd.print("Cooling ");
        break;
    }
}

void loop() {

    //get temperature reading for primary temp
    //Serial.println("Getting temperature");
    getCurrentTemp();

    //handle keypad
    int button = getButton();
    //Serial.println("Geting buttons");
    handleButton(button);
    
    //temperature logic
    
    
    //update the LCD...
    updateLCD();
    
      //handle TCP client
    EthernetClient client = server.available();
    if (client) {
      // read bytes from the incoming client and write them back
      // to any clients connected to the server:
      server.write(client.read());
    }
   
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
          targetTemp += 0.5;
          break;
        case BUTTON_DOWN :
          targetTemp -= 0.5;
          break;
        case BUTTON_LEFT :
          break;
        case BUTTON_RIGHT :
          break;
        case BUTTON_SELECT :
          break;
     }
  } 
}

