    //Spray module

#include <EEPROM.h> 

//all the program variables
#include "zVar.h"

// if not in eeprom, overwrite 
#define EEP_Ident 5108

//PWM for flow motor control
#define Motor1 9
#define Motor2 10 

#define FLOWPIN 2    //This is the input pin on the Arduino for flow meter
#define PRESSURE A0

    void setup() 
    {
        //set the baud rate
        Serial.begin(115200);

        //D2 is interrupt 0 for flowmeter
        pinMode(FLOWPIN, INPUT_PULLUP);           //Sets the pin as an input, pullup
        pinMode (A0, INPUT);

        //pin D4,D5 is direction for PWM, 
        pinMode(Motor1, OUTPUT); //pwm     
        pinMode(Motor2, OUTPUT); //pwm   

        pinMode(5, OUTPUT);
        pinMode(6, OUTPUT); //flow valve closed
        pinMode(7, OUTPUT);
        pinMode(8, OUTPUT);

        attachInterrupt(digitalPinToInterrupt(FLOWPIN), isr_Flow, FALLING);  //Configures interrupt 0 (pin 2 on the Arduino Uno) to run the isr

        EEPROM.get(0, EEread);              // read identifier

        if (EEread != EEP_Ident)   // check on first start and write EEPROM
        {
            EEPROM.put(0, EEP_Ident);
            EEPROM.put(4, settings);
        }
        else
        {
            EEPROM.get(4, settings);     // read the Settings
        }

        gainPWM = pow(1.09, ((105 - settings.Kp) * -1));
    }

    void loop()
    {
        //Loop triggers every 200 msec
        currentTime = millis();

        //200 msec
        if (currentTime - lastTime >= LOOP_TIME)
        {
            lastTime = currentTime;

            //If connection lost to AgOpenGPS, the watchdog will count up 
            if (watchdogTimer++ > 250) watchdogTimer = 30;

            //read pressure and be above the min
            pressureActual = (float)analogRead(A0);
            pressureActual -= 102;
            pressureActual /= settings.pressureCalFactor;

            if (isr_isFlowing)
            {
                //1 million divided by flow time in microseconds times 60 times 100 all divided by flowmeter Cal number
                actualGPM = ((1000000.0 / (float)isr_flowTime) * 6000) / settings.flowCalFactor;
            }
            else
            {
                actualGPM = 0;
            }

            //turn sections on/off          
            DoRelays();

            //Volume x 10
            totalVolume = (isr_flowCount * 10) / settings.flowCalFactor;

            //if (isPressureLow || watchdogTimer > 29)
            if ((watchdogTimer > 29 || setGPM < 50 || speed < 1.0) && autoMode)
            {
                //make sure flow control doesn't move
                relayLo = 0;
                relayHi = 0;

                flowError = 0;
                pwmDrive = 0;
                DoManualPID();
            }
            else
            {
                //Two modes, Manual and Auto
                if (autoMode)
                {
                    if (relayLo == 0)
                    {
                        flowError = 0;
                        pwmDrive = 0;
                        DoManualPID();
                    }
                    else
                    {
                        DoPID();
                    }
                }
                else //is manual control mode
                {
                    if (manualCounter > 0)
                    {
                        manualCounter--;
                    }
                    else
                    {
                        pwmDrive = 0;
                    }
                    flowError = (setGPM - actualGPM); //for dsplay only   
                    DoManualPID();
                }
            }

            flipFlopPWM++;
            if (flipFlopPWM > 5) flipFlopPWM = 0;
            
            flipFlop++;
            
            if (flipFlop > 1)
            {
                //Volume x 10
                totalVolume = (uint16_t)((((float)(isr_flowCount)) / settings.flowCalFactor) * 10);

                flipFlop = 0;

                AOG[5] = (byte)(totalVolume);
                AOG[6] = (byte)(totalVolume >> 8);

                if (speed < 1.0) 
                {
                  actualGPM = 0;
                  isr_isFlowing = 0;
                }
                uint16_t aGPM = actualGPM;
                AOG[7] = (uint8_t)aGPM;
                AOG[8] = (uint8_t)(aGPM >> 8);

                AOG[9] = (uint8_t)(pressureActual);
                AOG[10] = (int8_t)(isr_isFlowing);

                //1 is positive pwm drive, 0 is negative
                uint8_t syne = 1;
                if (pwmDrive < 0) syne = 0;
                AOG[11] = (int8_t)(abs(pwmDrive));
                AOG[12] = syne;

                float hz = (float)(isr_flowTime) / 1000.0;
                hz = 1 / hz;
                hz = hz * 1000;

                AOG[13] = (byte)(hz);
                AOG[14] = (byte)(((uint16_t)hz >> 8));

                //add the checksum
                int16_t CK_A = 0;
                for (uint8_t i = 2; i < sizeof(AOG) - 1; i++)
                {
                    CK_A = (CK_A + AOG[i]);
                }
                AOG[sizeof(AOG) - 1] = CK_A;

                Serial.write(AOG, sizeof(AOG));
                Serial.flush();   // flush out buffer
            }

            if (isr_isFlowing == 1) isr_isFlowing = 0;

        }//end of timed loop

        // UDP from AgIO
        DoSerialReceive();
    }

    void DoRelays()
    {
        digitalWrite(5, (bitRead(relayLo, 0)));
        digitalWrite(6, (bitRead(relayLo, 1)));
        digitalWrite(7, (bitRead(relayLo, 2)));
        digitalWrite(8, (bitRead(relayLo, 3)));
    }
