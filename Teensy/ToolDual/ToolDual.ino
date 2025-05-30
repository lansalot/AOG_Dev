// Single antenna, IMU, & dual antenna code for AgOpenGPS
// If dual right antenna is for position (must enter this location in AgOpen), left Antenna is for heading & roll
//
// connection plan:
// Teensy Serial 7 RX (28) to F9P Position receiver TX1 (Position data)
// Teensy Serial 7 TX (29) to F9P Position receiver RX1 (RTCM data for RTK)
// Teensy Serial 2 RX (7) to F9P Heading receiver TX1 (Relative position from left antenna to right antenna)
// Teensy Serial 2 TX (8) to F9P Heading receiver RX1
// F9P Position receiver TX2 to F9P Heading receiver RX2 (RTCM data for Moving Base)
//
// Configuration of receiver
// Position F9P
// CFG-RATE-MEAS - 100 ms -> 10 Hz
// CFG-UART1-BAUDRATE 460800
// Serial 1 In - RTCM (Correction Data from AOG)
// Serial 1 Out - NMEA GGA
// CFG-UART2-BAUDRATE 460800
// Serial 2 Out - RTCM 1074,1084,1094,1230,4072.0 (Correction data for Heading F9P, Moving Base)  
// 1124 is not needed (China’s BeiDou system) - Save F9P brain power 
//
// Heading F9P
// CFG-RATE-MEAS - 100 ms -> 10 Hz
// CFG-UART1-BAUDRATE 460800
// Serial 1 Out - UBX-NAV-RELPOSNED
// CFG-UART2-BAUDRATE 460800
// Serial 2 In RTCM

/************************* User Settings *************************/
// Serial Ports
#define SerialAOG Serial                //AgIO USB conection
#define SerialRTK Serial3               //RTK radio
#define SerialGPS Serial7  //Main postion receiver (GGA) (Serial2 must be used here with T4.0 / Basic Panda boards - Should auto swap)
#define SerialGPS2 Serial2  //Dual heading receiver 

const int32_t baudAOG = 115200;
const int32_t baudGPS = 460800;
const int32_t baudRTK = 9600;     // most are using Xbee radios with default of 115200

#define ImuWire Wire        //SCL=19:A5 SDA=18:A4
#define RAD_TO_DEG_X_10 572.95779513082320876798154814105

#define baseLineLimit 5       //Max CM differance in baseline

#define REPORT_INTERVAL 20    //BNO report time, we want to keep reading it quick & offen. Its not timmed to anything just give constant data.

//Status LED's
#define GGAReceivedLED 13         //Teensy onboard LED
#define Power_on_LED 5            //Red
#define Ethernet_Active_LED 6     //Green
#define GPSRED_LED 9              //Red (Flashing = NO IMU or Dual, ON = GPS fix with IMU)
#define GPSGREEN_LED 10           //Green (Flashing = Dual bad, ON = Dual good)
#define AUTOSTEER_STANDBY_LED 11  //Red
#define AUTOSTEER_ACTIVE_LED 12   //Green
uint32_t gpsReadyTime = 0;        //Used for GGA timeout

/*****************************************************************/

// Ethernet Options
#include "PGNs.h"
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>

struct ConfigIP {
    uint8_t ipOne = 192;
    uint8_t ipTwo = 168;
    uint8_t ipThree = 5;
};  ConfigIP networkAddress;   //3 bytes

// IP & MAC address of this module of this module
byte Eth_myip[4] = { 0, 0, 0, 0}; //This is now set via AgIO
byte mac[] = {0x00, 0x00, 0x56, 0x00, 0x00, 0x78};

unsigned int portMy = 5120;             // port of this module
unsigned int AOGNtripPort = 2233;       // port NTRIP data from AOG comes in
unsigned int AOGAutoSteerPort = 18888;   // port Autosteer data from AOG comes in
unsigned int portDestination = 19999;    // Port of AOG that listens
char Eth_NTRIP_packetBuffer[512];       // buffer for receiving ntrip data

// An EthernetUDP instance to let us send and receive packets over UDP
EthernetUDP Eth_udpPAOGI;     //Out port 5544
EthernetUDP Eth_udpNtrip;     //In port 2233
EthernetUDP Eth_udpToolSteer; //In & Out Port 18888

IPAddress Eth_ipDestination;

byte CK_A = 0;
byte CK_B = 0;
int relposnedByteCount = 0;

#include "zNMEAParser.h"
#include <Wire.h>

bool useDual = false;
bool dualReadyGGA = false;
bool dualReadyRelPos = false;

//Dual
double headingcorr = 900;  //90deg heading correction (90deg*10)
// Heading correction 180 degrees, because normally the heading antenna is in front, but we have it at the back
//double headingcorr = 1800;  // 180deg heading correction (180deg*10)

double baseline = 0;
double rollDual = 0;
double relPosD = 0;
double heading = 0;

byte ackPacket[72] = {0xB5, 0x62, 0x01, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr int serial_buffer_size = 512;
uint8_t GPSrxbuffer[serial_buffer_size];    //Extra serial rx buffer
uint8_t GPStxbuffer[serial_buffer_size];    //Extra serial tx buffer
uint8_t GPS2rxbuffer[serial_buffer_size];   //Extra serial rx buffer
uint8_t GPS2txbuffer[serial_buffer_size];   //Extra serial tx buffer
uint8_t RTKrxbuffer[serial_buffer_size];    //Extra serial rx buffer

/* A parser is declared with 3 handlers at most */
NMEAParser<2> parser;

bool isTriggered = false;
bool blink = false;

bool Ethernet_running = false; //Auto set on in ethernet setup
bool GGA_Available = false;    //Do we have GGA on correct port?

uint32_t PortSwapTime = 0;

// Buffer to read chars from Serial, to check if "!AOG" is found
uint8_t aogSerialCmd[4] = { '!', 'A', 'O', 'G'};
uint8_t aogSerialCmdBuffer[6];
uint8_t aogSerialCmdCounter = 0;

// Booleans to indictate to passthrough GPS or GPS2
bool passThroughGPS = false;
bool passThroughGPS2 = false;

// Setup procedure ------------------------
void setup()
{
    delay(500);                         //Small delay so serial can monitor start up

    pinMode(GGAReceivedLED, OUTPUT);
    pinMode(Power_on_LED, OUTPUT);
    pinMode(Ethernet_Active_LED, OUTPUT);
    pinMode(GPSRED_LED, OUTPUT);
    pinMode(GPSGREEN_LED, OUTPUT);
    pinMode(AUTOSTEER_STANDBY_LED, OUTPUT);
    pinMode(AUTOSTEER_ACTIVE_LED, OUTPUT);

    // the dash means wildcard
    parser.setErrorHandler(errorHandler);
    parser.addHandler("G-GGA", GGA_Handler);
    parser.addHandler("G-VTG", VTG_Handler);

    delay(10);
    Serial.begin(baudAOG);
    delay(10);
    Serial.println("Start setup");

    SerialGPS.begin(baudGPS);
    SerialGPS.addMemoryForRead(GPSrxbuffer, serial_buffer_size);
    SerialGPS.addMemoryForWrite(GPStxbuffer, serial_buffer_size);

    delay(10);
    SerialRTK.begin(baudRTK);
    SerialRTK.addMemoryForRead(RTKrxbuffer, serial_buffer_size);

    delay(10);
    SerialGPS2.begin(baudGPS);
    SerialGPS2.addMemoryForRead(GPS2rxbuffer, serial_buffer_size);
    SerialGPS2.addMemoryForWrite(GPS2txbuffer, serial_buffer_size);

    Serial.println("SerialAOG, SerialRTK, SerialGPS and SerialGPS2 initialized");

    Serial.println("\r\nStarting AutoSteer...");
    ToolsteerSetup();

    Serial.println("\r\nStarting Ethernet...");
    EthernetStart();

    Serial.println("\r\nStarting IMU...");
    //test if CMPS working
    uint8_t error;

    Serial.println("\r\nEnd setup, waiting for GPS...\r\n");
}

void loop()
{
    // Read incoming nmea from GPS
    if (SerialGPS.available())
    {
        parser << SerialGPS.read();
    }

    // If anything comes in SerialGPS2 RelPos data
    if (SerialGPS2.available())
    {
        uint8_t incoming_char = SerialGPS2.read();  //Read RELPOSNED from F9P

        // Just increase the byte counter for the first 3 bytes
        if (relposnedByteCount < 4 && incoming_char == ackPacket[relposnedByteCount])
        {
            relposnedByteCount++;
        }
        else if (relposnedByteCount > 3)
        {
            // Real data, put the received bytes in the buffer
            ackPacket[relposnedByteCount] = incoming_char;
            relposnedByteCount++;
        }
        else
        {
            // Reset the counter, becaues the start sequence was broken
            relposnedByteCount = 0;
        }
    }

    // Check the message when the buffer is full
    if (relposnedByteCount > 71)
    {
        if (calcChecksum())
        {
            //if(deBug) Serial.println("RelPos Message Recived");
            digitalWrite(GPSRED_LED, LOW);   //Turn red GPS LED OFF (we are now in dual mode so green LED)
            useDual = true;
            relPosDecode();
        }
        /*  else {
          if(deBug) Serial.println("ACK Checksum Failure: ");
          }
        */
        relposnedByteCount = 0;
    }

    // If both dual messages are ready, send to AgOpen
    if (dualReadyGGA == true && dualReadyRelPos == true)
    {
        BuildNmea();
        dualReadyGGA = false;
        dualReadyRelPos = false;
    }

    //recv udp
    ReceiveUdp();

    //do the autosteer loop
    toolsteerLoop();

    //send udp ntrip to serialgps
    udpNtrip();

    // Check for RTK Radio
    if (SerialRTK.available())
    {
        SerialGPS.write(SerialRTK.read());
    }

    // Pass NTRIP etc to GPS
    if (SerialAOG.available())
    {
        SerialGPS.write(SerialAOG.read());
    }

    if (Ethernet.linkStatus() == LinkOFF)
    {
        digitalWrite(Power_on_LED, 1);
        digitalWrite(Ethernet_Active_LED, 0);
    }
    if (Ethernet.linkStatus() == LinkON)
    {
        digitalWrite(Power_on_LED, 0);
        digitalWrite(Ethernet_Active_LED, 1);
    }
    //GGA timeout, turn off GPS LED's etc
    if ((systick_millis_count - gpsReadyTime) > 10000) //GGA age over 10sec
    {
        digitalWrite(GPSRED_LED, LOW);
        digitalWrite(GPSGREEN_LED, LOW);
        useDual = false;
    }
}
//End Loop
//**************************************************************************

bool calcChecksum()
{
    CK_A = 0;
    CK_B = 0;

    for (int i = 2; i < 70; i++)
    {
        CK_A = CK_A + ackPacket[i];
        CK_B = CK_B + CK_A;
    }

    return (CK_A == ackPacket[70] && CK_B == ackPacket[71]);
}

