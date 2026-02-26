/*
************************************************************************
*
*          esp_nixie
*   ======================
*   Uwe Berger; 2023, 2026
*
* 
* https://github.com/boerge42/nixie
* http://bralug.de/wiki/Nixie
*
* 
* Was ist das hier?
* -----------------
* Ansteuerung meiner alten Nixie (siehe obige Links) mit einem ESP8266. 
* Idee ist es die alte "Platine" ohne AVR-MCU zu benutzen. Der ESP8266 
* ist dazu, via Pegelwandler(!), mit der Schieberegisterkette (4x 74HC595)
* verbunden.
* 
* Warum der Aufwand?
* ------------------
* Das DCF77-Zeitsignal ist (bei mir) zu instabil. Deshalb soll sich die 
* Uhr mit meinem WLAN verbinden und die Zeit aus dem Internet holen!
*
*  
* Funktionen:
* -----------
* - Anmeldung im WLAN                    
*   - Sanduhr bei WLAN-Connect auf Nixies
*   - automatischer Reconnect, wenn WLAN weg
*   - Anzeige, wenn keine Verbindung zum WLAN
* - mit "Internetzeit" synchronisieren   
* - Helligkeitssensor (BH1750) eingebunden, um die Nixies dunkel
*   zu schalten, wenn es dunkel ist
* - regelmaessig alle Nixies "scrollen"      
* - Ausgabe Uhrzeit (hh.mm.ss) auf Nixies :-)
* - Steuerung mit MQTT-Nachrichten
* 
*
* Steuerung durch MQTT-Nachrichten (Topic siehe Quelltext):
* ---------------------------------------------------------
* Payload | Aktion
* ========+=======================================================
* time    | Anzeige Uhrzeit
* --------+-------------------------------------------------------
* date    | Anzeige Datum
* --------+-------------------------------------------------------
* on      | Nixies an
* --------+-------------------------------------------------------
* off     | Nixies aus
* --------+-------------------------------------------------------
* ...     | wenn Zahlen/Punkte; Anzeige der letzten sechs Stellen
* --------+-------------------------------------------------------
*
*
* Da erstmal keine Taster mit dem ESP verbunden sind, gibt es kein
* Umschalten zwischen Datum/Uhrzeit, kein manuelles Einstellen der 
* Uhrzeit o.ä.. Vielleicht kommt das irgendwann nochmal...
*
* Ideen?
* ------
* - Setzen Datum/Uhrzeit via MQTT
*
*  
* ---------
* Have fun!
*
************************************************************************
*/

#include <ESP8266WiFi.h>
#include <ezTime.h>
#include <Wire.h>
#include <BH1750FVI.h>
#include <PubSubClient.h>


// WiFi
const char *ssid     = "xxxx";
const char *password = "yyyy";

// MQTT
//~ #define MQTT_CLIENT_ID "nixie"
char mqttServer[30] 	= "nanotuxedo";
int  mqttPort 		    = 1883;
char mqttUser[30] 	    = "";
char mqttPassword[30]   = "";
char mqttClientId[30]   = "nixie";
const char *mqtt_topic  = "nixie/";

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// ezTime & Zeitzone
#define MY_TIME_ZONE "Europe/Berlin"
Timezone myTZ;
char my_time_zone[50] = MY_TIME_ZONE;

// Helligkeitssensor
BH1750FVI lux(0x23);

// Daten, die in die 4x 74HC595-Kette geschoben werden
union sr_data_t {
	struct	{uint8_t d[4];} sr_array;
	struct	{
				unsigned p0:1;		// (rechte) Punkte
				unsigned p1:1;
				unsigned p2:1;
				unsigned p3:1;
				unsigned p4:1;
				unsigned p5:1;
				unsigned p6:1;
				unsigned p7:1;
				unsigned d5:4;		// Stellen
				unsigned d4:4;
				unsigned d3:4;
				unsigned d2:4;
				unsigned d1:4;
				unsigned d0:4;
			} sr;
	} sr_data;

// display modes
#define DISPLAY_TIME        0
#define DISPLAY_DATE        1
#define DISPLAY_OFF         2
#define DISPLAY_MQTT        3
int display_mode = DISPLAY_TIME;

// ein paar Parameter
#define NIXIE_SCROLL_EVERY_MINUTE   5   // Minute%... == 0
#define NIXIE_OFF_LUX               5   // BH1750-Lux-Wert

#define SWITCH_BACK_TO_TIME         5   // Sekunden
int switch_back_to_time_counter = 0; 


//~ Wemos D1 mini:
//~ +-----------+--------------+---------------------------+
//~ | Board-Pin | ESP8266 GPIO | Typische Funktion         |
//~ +-----------+--------------+---------------------------+
//~ | D0        | GPIO16       | kein PWM, kein I2C        |
//~ | D1        | GPIO5        | SCL (I2C)                 |
//~ | D2        | GPIO4        | SDA (I2C)                 |
//~ | D3        | GPIO0        | Boot-Modus                |
//~ | D4        | GPIO2        | Onboard-LED               |
//~ | D5        | GPIO14       | SPI SCK                   |
//~ | D6        | GPIO12       | SPI MISO                  |
//~ | D7        | GPIO13       | SPI MOSI                  |
//~ | D8        | GPIO15       | Boot-Modus                |
//~ +-----------+--------------+---------------------------+

// Definition GPIOs
// ...74HC595...
#define SR_SER  14  // GPIO14 -> D5
#define SR_SCK  12  // GPIO12 -> D6
#define SR_SCL  13  // GPIO13 -> D7
#define SR_RCK  16  // GPIO16 -> D0

// ...I2C...
#define SDA     4   // GPIO4 -> D2
#define SCL     5   // GPIO5 -> D1


// ********************************************************************************
void data2nixie()
{
    uint8_t i, j, mask;
	// Schieberegister zuruecksetzen
    digitalWrite(SR_SCL, LOW);
    digitalWrite(SR_SCL, HIGH);
	// Ausgabe ins Schiebergister schieben
	for (j=0; j<4; j++) {
        for (int i = 7; i >= 0; i--) { 
            digitalWrite(SR_SCK, LOW); 
            digitalWrite(SR_SER, (sr_data.sr_array.d[j] >> i) & 0x01); 
            digitalWrite(SR_SCK, HIGH); 
            } 		
	}
    // 0-1-Flanke an RCK bringt die Daten aus den Registern in die Latches 
    digitalWrite(SR_RCK, LOW);
    digitalWrite(SR_RCK, HIGH);
}

// ********************************************************************************
void nixie_scroll()
{
    int scroll_counter;
    Serial.println("==> nixie_scroll()");
    //
    sr_data.sr_array.d[0] = 0b00111111;
    for (scroll_counter=0; scroll_counter < 10; scroll_counter++) {
        sr_data.sr.d0 = scroll_counter;
        sr_data.sr.d1 = scroll_counter;
        sr_data.sr.d2 = scroll_counter;
        sr_data.sr.d3 = scroll_counter;
        sr_data.sr.d4 = scroll_counter;
        sr_data.sr.d5 = scroll_counter;
        data2nixie();
        delay(20);
    }
}

// ********************************************************************************
void nixie_off()
{
    Serial.print("==> nixie_off(): ");
    Serial.println(lux.getLux());
    // 
    sr_data.sr_array.d[0] = 0b00000000;
    sr_data.sr_array.d[1] = 0b11111111;
    sr_data.sr_array.d[2] = 0b11111111;
    sr_data.sr_array.d[3] = 0b11111111;
    data2nixie();
}

// ********************************************************************************
void nixie_display_date_time()
{
    int d01, d23, d45;
    if (display_mode == DISPLAY_TIME) {
        // Uhrzeit
        d01 = myTZ.second();
        d23 = myTZ.minute();
        d45 = myTZ.hour();
    } else if (display_mode == DISPLAY_DATE) {
        // Datum
        d01 = myTZ.year() - 2000;
        d23 = myTZ.month();
        d45 = myTZ.day();        
    }
    char str[50];
    sprintf(str, "==> nixie_display_date_time(): %02d.%02d.%02d", d45, d23, d01);
    Serial.println(str);
    //
    sr_data.sr.p5 = 0;
    sr_data.sr.p4 = 1;
    sr_data.sr.p3 = 0;
    sr_data.sr.p2 = 1;
    sr_data.sr.p1 = 0;
    // wenn keine Verbindung zum WLAN, dann letzten Punkt leuchten lassen
    if (WiFi.status() == WL_CONNECTED) {
        sr_data.sr.p0 = 0;
    } else {
        sr_data.sr.p0 = 1;
    }
    sr_data.sr.d0 = d01/10;
    sr_data.sr.d1 = d01%10;
    sr_data.sr.d2 = d23/10;
    sr_data.sr.d3 = d23%10;
    sr_data.sr.d4 = d45/10;
    sr_data.sr.d5 = d45%10;
    data2nixie();
}

// ********************************************************************************
void nixie_display_mqtt_payload(byte* payload, unsigned int length)
{
    int idx = 0;
    boolean dp_pre = false;
    char c;
    Serial.println("==>> nixie_display_mqtt_payload()");
    // nixie clear
    sr_data.sr_array.d[0] = 0b00000000;
    sr_data.sr_array.d[1] = 0b11111111;
    sr_data.sr_array.d[2] = 0b11111111;
    sr_data.sr_array.d[3] = 0b11111111;
    // payload parsen
    for (int i = 0; i < length; i++) { 
        c = (char)payload[length - i - 1];
        if (c == '.') {
           if (dp_pre) {
               idx++;
           }
           dp_pre = true;
           switch (idx) {
               case 0:
                    sr_data.sr.p0 = 1;
                    break;
               case 1:
                    sr_data.sr.p1 = 1;
                    break;
               case 2:
                    sr_data.sr.p2 = 1;
                    break;
               case 3:
                    sr_data.sr.p3 = 1;
                    break;
               case 4:
                    sr_data.sr.p4 = 1;
                    break;
               case 5:
                    sr_data.sr.p5 = 1;
                    break;
           }
        } else if (c >= '0' && c <= '9') {
           switch (idx) {
               case 0:
                    sr_data.sr.d1 = c - '0';
                    break;
               case 1:
                    sr_data.sr.d0 = c - '0';
                    break;
               case 2:
                    sr_data.sr.d3 = c - '0';
                    break;
               case 3:
                    sr_data.sr.d2 = c - '0';
                    break;
               case 4:
                    sr_data.sr.d5 = c - '0';
                    break;
               case 5:
                    sr_data.sr.d4 = c - '0';
                    break;
           }
           idx++; 
           dp_pre = false;
        } else {
            idx++;
           dp_pre = false;
        }
        if (idx > 5) {
            break;
        }
    }
    data2nixie();
}


// ********************************************************************************
void nixie_display_busy()
{
    static int counter = 0;
    static int add = 1;
    Serial.println("==> nixie_display_busy() ");
    //
    sr_data.sr_array.d[0] = 0b00000000;
    sr_data.sr_array.d[1] = 0b11111111;
    sr_data.sr_array.d[2] = 0b11111111;
    sr_data.sr_array.d[3] = 0b11111111;
    // ...2 hoch counter
    sr_data.sr_array.d[0] = 1 << counter;
    data2nixie();
    if ((add == 1) && (counter == 5)) {add = 0;}
    if ((add == 0) && (counter == 0)) {add = 1;}
    if (add == 1) {counter += 1;} else {counter -= 1;}
}

// **************************************************************
void mqtt_reconnect ()
{
    // ...Verbindungsvesuch
    if (mqtt_client.connect(mqttClientId)) {
       Serial.println("MQTT connected!"); 
       mqtt_client.publish(mqtt_topic,"hello from nixie!");
       mqtt_client.subscribe(mqtt_topic); 
    } else {
        Serial.print("MQTT-Connect failed with state ");
        Serial.println(mqtt_client.state());
    }
}

// **************************************************************
void mqtt_callback (char* topic, byte* payload, unsigned int length)
{

    // Payload in String umwandeln 
    String message; 
    for (int i = 0; i < length; i++) { 
        message += (char)payload[i]; 
        }

    // Ausgabe topic/payload
    Serial.print("MQTT-Message arrived: topic=");
    Serial.print(topic);
    Serial.print("; payload=");
    Serial.print(message);
    Serial.println();
    
    // Payload (message) auswerten
    if (message.equalsIgnoreCase("time")) {
        display_mode = DISPLAY_TIME;                        // Anzeige Uhrzeit
    } else if (message.equalsIgnoreCase("date")) {
        display_mode = DISPLAY_DATE;                        // Anzeige Datum
        switch_back_to_time_counter = SWITCH_BACK_TO_TIME;
    } else if (message.equalsIgnoreCase("on")) {
        display_mode = DISPLAY_TIME;                        // Nixies an -> Anzeige Uhrzeit
    } else if (message.equalsIgnoreCase("off")) {
        display_mode = DISPLAY_OFF;                         // Nixies aus
    } else {
       display_mode = DISPLAY_MQTT;                         // MQTT-Payload anzeigen
       nixie_display_mqtt_payload(payload, length); 
       switch_back_to_time_counter = SWITCH_BACK_TO_TIME;
    }
}

// ********************************************************************************
void setup(void)
{
    // serielle Schnittstelle initialisieren
    Serial.begin(115200);
    Serial.println("");
    
    // GPIOs
    pinMode(SR_SER, OUTPUT);
    pinMode(SR_SCK, OUTPUT);
    pinMode(SR_SCL, OUTPUT);
    pinMode(SR_RCK, OUTPUT);

    // BH1750
    Wire.begin();
    lux.powerOn();
    lux.setContHighRes();

    // Verbindung mit WLAN herstellen
    Serial.println("connecting to wifi:");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
      nixie_display_busy();
    }
    Serial.println("");
    
    // Datum/Uhrzeit holen
    waitForSync();
    Serial.println("UTC: " + UTC.dateTime());
    myTZ.setLocation(my_time_zone);
    Serial.println("MyTZ time: " + myTZ.dateTime("d-M-y H:i:s"));    
    
    // MQTT initialisieren/verbinden
    mqtt_client.setServer(mqttServer, mqttPort);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_reconnect();
    
}

// ********************************************************************************
// ********************************************************************************
// ********************************************************************************
void loop()
{

    // ...vorhergehende Sekunde
    static uint8_t old_second = -1;

    // ...jede Sekunde schauen, was zu tun ist...
    if (myTZ.second() != old_second) {
        old_second = myTZ.second();
                
       // wenn keine Verbindung zum WLAN besteht, jede (volle) Minute einen Reconnect versuchen
        if ((WiFi.status() != WL_CONNECTED) && (old_second%60 == 0)) {
           Serial.println("reconnecting to wifi...");
           WiFi.disconnect();
           WiFi.reconnect();
        }                
        
        // mit MQTT alles in Ordnung?
        if (!mqtt_client.connected()) {
            mqtt_reconnect();
        }
                
        // sollen Nixies ueberhaupt etwas anzeigen (Umgebungshelligkeit ausreichend bzw. via MQTT ausgeschaltet)
        if ((lux.getLux() < NIXIE_OFF_LUX) || (display_mode == DISPLAY_OFF)) {
            // ...nein...
            nixie_off();
            // ...Flanke "Lux_off" -> "Lux_on": display_mode = DISPLAY_TIME...???
        } else {
            // ...ja...
            // Nixies zu jeder "definierten" Minute scrollen!?
            if ((myTZ.second() == 0) && 
                (myTZ.minute() % NIXIE_SCROLL_EVERY_MINUTE == 0) && 
                ((display_mode == DISPLAY_DATE) || (display_mode == DISPLAY_TIME))) {
                nixie_scroll();
            }
            // switch_back_to_time...
            if (switch_back_to_time_counter > 0) {
                switch_back_to_time_counter--;
                if (switch_back_to_time_counter == 0) {
                    display_mode = DISPLAY_TIME;
                }
            }
            // Ausgabe Datum oder Uhrzeit
            if ((display_mode == DISPLAY_DATE) || (display_mode == DISPLAY_TIME)) {
                nixie_display_date_time();
            }
        }
        
    }

    // MQTT-loop
    mqtt_client.loop();

    // ezTime-loop
    events();    

    // Verschnaufspause ;-)
    delay(5);

}
