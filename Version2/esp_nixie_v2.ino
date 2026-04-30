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
*   - "Sanduhr" bei WLAN-Connect auf Nixies
*   - automatischer Reconnect, wenn WLAN/MQTT weg
* - mit "Internetzeit" (NTP) synchronisieren   
* - Helligkeitssensor (BH1750) eingebunden, um die Nixies dunkel
*   zu schalten, wenn es dunkel ist
* - regelmaessig (...alle 5min...) alle Nixies "scrollen"      
* - Ausgabe Uhrzeit (hh.mm.ss) auf Nixies :-)
* - Steuerung mit MQTT-Nachrichten -> siehe weiter unten!
* - Anzeige Status (binkend; Nummerierung von recht=P0 nach links=P5):
*   -> P0 --> wenn keine Verbindung zum WLAN
*   -> P1 --> wenn time_quality < 1
*   -> P3 --> wenn time_quality < 2
*   -> P5 --> wenn time_quality < 3
* - diverse Werte und Status werden zu einem MQTT-Broker gesendet
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
inspiriert von:
  https://www.werner.rothschopf.net/201802_arduino_esp8266_ntp.htm
  https://randomnerdtutorials.com/esp32-date-time-ntp-client-server-arduino/
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/system_time.html
*/

#include <ESP8266WiFi.h>
#include <Wire.h>
#include <BH1750FVI.h>
#include <PubSubClient.h>
#include <time.h>

// time
time_t now;                         // Unixzeit
tm tm;                              // Structure tm enthaelt die Zeitinformationen

/* Configuration of NTP */
#define MY_NTP_SERVER "10.1.1.1"           
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03" // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

// WiFi
const char *ssid     = "xxxx";
const char *password = "yyyy";
String hostname      = "Nixie";

// MQTT
char mqttServer[30] 	= "nanotuxedo";
int  mqttPort 		    = 1883;
char mqttUser[30] 	    = "";
char mqttPassword[30]   = "";
char mqttClientId[30]   = "nixie";
const char *mqtt_topic  = "nixie/";
const char *mqtt_topic_quality  = "nixie/quality/";

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

boolean wifi_connect = false;

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
#define NIXIE_OFF_LUX               1   // BH1750-Lux-Wert

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


// ein paar Zustandvariablen
uint8_t isNTPSync       = 0;
uint8_t isWLANStatus    = 0;
uint8_t isMQTTStatus    = 0;
uint8_t isNixieScroll   = 0;
float   isLuxValue      = 0;
int     time_quality    = 0;   // 0 = keine Zeit, 1 = schlecht, 2 = ok, 3 = gut


// ********************************************************************************
// erstes Holen ntp-Zeit in...
uint32_t sntp_startup_delay_MS_rfc_not_less_than_60000 ()
{
  return 1000; // 1s
}

// ********************************************************************************
// ntp-Abfrageintervall
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000 ()
{
  return 1 * 60 * 60 * 1000UL; // 1h
}

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
    isNixieScroll = 1;
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
    Serial.println(isLuxValue);
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
    static boolean point_on = true;
    
    int d01, d23, d45;
    if (display_mode == DISPLAY_TIME) {
        // Uhrzeit
        d01 = tm.tm_sec;
        d23 = tm.tm_min;
        d45 = tm.tm_hour;
    } else if (display_mode == DISPLAY_DATE) {
        // Datum
        d01 = tm.tm_year - 100;     // Anz. Jahre seit 1900
        d23 = tm.tm_mon+1;          // Januar = 0
        d45 = tm.tm_mday;        
    }
    
    // wenn time_quality < 3 -> blinken
    if ((time_quality < 3) && point_on) {
        sr_data.sr.p5 = 1;
    } else {
        sr_data.sr.p5 = 0;
    }
    
    // bei date/time immer an
    sr_data.sr.p4 = 1;
    
    // wenn time_quality < 2 -> blinken
    if ((time_quality < 2) && point_on) {
        sr_data.sr.p3 = 1;
    } else {
        sr_data.sr.p3 = 0;
    }
    
    // bei date/time immer an
    sr_data.sr.p2 = 1;
    
    // wenn time_quality < 1 -> blinken
    if ((time_quality < 1) && point_on) {
        sr_data.sr.p1 = 1;
    } else {
        sr_data.sr.p1 = 0;
    }
    
    // wenn keine Verbindung zum WLAN ==>> blinken
    if (!wifi_connect && point_on) {
        sr_data.sr.p0 = 1;
    } else {
        sr_data.sr.p0 = 0;
    }
    
    sr_data.sr.d0 = d01/10;
    sr_data.sr.d1 = d01%10;
    sr_data.sr.d2 = d23/10;
    sr_data.sr.d3 = d23%10;
    sr_data.sr.d4 = d45/10;
    sr_data.sr.d5 = d45%10;
    data2nixie();
    
    if (!wifi_connect || (time_quality < 3)) {
        if (point_on) {
            point_on = false;
        } else {
            point_on = true;
        }
    }
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
    Serial.println("==>> nixie_display_busy() ");
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
    // ...Verbindungsversuch
    if (mqtt_client.connect(mqttClientId)) {
       Serial.println("==>> mqtt_reconnect(): MQTT connected!"); 
       mqtt_client.publish(mqtt_topic,"hello from nixie!");
       mqtt_client.subscribe(mqtt_topic); 
    } else {
        Serial.print("==>> mqtt_reconnect(): MQTT-Connect failed with state ");
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
    Serial.print("==>> mqtt_callback(): MQTT-Message arrived: topic=");
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
// ...ueber sie Sinnhaftigkeit der hier berechneten Werte lässt sich streiten
void update_ntp_quality()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    
    time_t now = tv.tv_sec;
    
    static unsigned long display_counter = 0;
    
    static boolean ntp_initialized      = false;
    static time_t ntp_lastGoodSync      = 0;
    static struct timeval ntp_lastTv    = {0, 0};
    static float  ntp_jitter            = 0;

    // Ist bereits eine realistische Uhrzeit gesetzt? Wenn nein, ist
    // alles egal und raus! 
    if (now < 1700000000) {  
        ntp_initialized = false;
        time_quality = 0;
        return;
    }

    // Es ist eine realistische Uhrzeit gesetzt, also hat der 1.NTP-Sync. 
    // funktioniert. Aber erst im nächsten Durchlauf kann man korrekte
    // Qualitätswerte (1s-Diff, Jitter) berechnen...
    if (!ntp_initialized) {
        isNTPSync = 1;
        ntp_initialized = true;
        ntp_lastGoodSync = now;
        ntp_lastTv = tv;
        time_quality = 3;
        return;
    }

    // vergangene Zeit (in Mikrosekunden) seit letzten Aufruf (...1000ms) 
    long diff = (tv.tv_sec - ntp_lastTv.tv_sec) * 1e6 + (tv.tv_usec - ntp_lastTv.tv_usec); // ...in Mikrosekunden!
    
    // Jitter berechnen (Abweichung vom Ideal 1s; mit Glaettung; exponentieller gleitender Mittelwert)
    ntp_jitter = 0.9 * ntp_jitter + 0.1 * fabs(diff - 1000000); // in Mikrosekunden!
    
    // Wenn die Zeit um mehr als 20000us "springt" und der letzte vermeindliche NTP-Sync. 
    // mehr als 5min her ist, könnte es ein erfolgreicher NTP-Request gewesen sein
    if ((fabs(diff - 1000000) > 20000) && ((now - ntp_lastGoodSync) > 301)) {
        isNTPSync = 1;
        ntp_lastGoodSync = now;
    }
    
    ntp_lastTv = tv;    
    
    // Zeit-Qualität aus Alter des letzten NTP-Sync. ableiten
    unsigned long age = now - ntp_lastGoodSync;

    if (age < 3600) {                         // letzte ntp-sync älter als 1h
        time_quality = 3; // sehr gut
    }
    else if (age < 21600) {                   // letzte ntp-sync älter als 6h
        time_quality = 2; // gut
    }
    else if (age < 43200) {                   // letzte ntp-sync älter als 12h
        time_quality = 1; // ok
    }
    else {
        time_quality = 0; // schlecht
    }
    
    // diverse Werte/Zustände via MQTT senden
    char mqtt_payload[300];
    sprintf(mqtt_payload, 
            "nixie jitter=%f,diff_1s=%d,quality=%d,isNTPSync=%d,isWLANStatus=%d,isMQTTStatus=%d,isNixieScroll=%d,isLuxValue=%f", 
            ntp_jitter,
            (diff-1000000), 
            time_quality,
            isNTPSync,
            isWLANStatus,
            isMQTTStatus,
            isNixieScroll,
            isLuxValue);
    //Serial.println(mqtt_payload);
    mqtt_client.publish(mqtt_topic_quality,mqtt_payload);
    
    // ...gesendete Zustände wieder zurücksetzen
    isNTPSync       = 0;
    isWLANStatus    = 0;
    isMQTTStatus    = 0;
    isNixieScroll   = 0;
    
}

// ********************************************************************************
void setup(void)
{
    
    // ESP auf CPU-Freq. 160MHz stellen, vielleicht nützt es was ;-)
    system_update_cpu_freq(160);
    
    // serielle Schnittstelle initialisieren
    Serial.begin(115200);
    
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
    WiFi.hostname(hostname.c_str());
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    
    // ...WiFi-Events registrieren
    WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& evt) {
        wifi_connect = true;
        Serial.println("WiFi connected");
    });
    WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& evt) {
        wifi_connect = false;
        Serial.println("WiFi lost");
    });
    // ...
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
      nixie_display_busy();
    }
    wifi_connect = true;
    Serial.println("");
    
    // NTP initialisieren/ Uhrzeit sync
    configTime(MY_TZ, MY_NTP_SERVER);
    
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
    
    // ...wifi_connect_counter
    static unsigned long wifi_t = 0;

    // ...mqtt_connect_counter
    static unsigned long mqtt_t = 0;

    // ...lux_counter und Schalter
    static unsigned long lux_t = 0;
    static boolean is_dark = false;

    // ...ntp_quality_counter
    static unsigned long ntp_quality_t = 0;

    time(&now);                       // aktuelle Zeit...
    localtime_r(&now, &tm);           // ...in lokale Zeit
    
    // jede Sekunde Zeit-Qualitaet bestimmen
    if(millis() - ntp_quality_t >= 1000) {
        ntp_quality_t = millis();
        update_ntp_quality();
    }

    // ...jede Sekunde Anzeige entsprechend aktualisieren
    if (tm.tm_sec != old_second) {
        old_second = tm.tm_sec;
                
        // sollen Nixies überhaupt etwas anzeigen (Umgebungshelligkeit ausreichend bzw. via MQTT nicht ausgeschaltet)
        if (is_dark || (display_mode == DISPLAY_OFF)) {
            // ...nein...
            nixie_off();
            // ...Flanke "Lux_off" -> "Lux_on": display_mode = DISPLAY_TIME...???
        } else {
            // ...ja...
            // Nixies zu jeder "definierten" Minute scrollen!?
            if ((tm.tm_sec == 0) && 
                (tm.tm_min % NIXIE_SCROLL_EVERY_MINUTE == 0) && 
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

    // jede 5min prüfen, ob WLAN-Verbindung besteht
    if (millis() - wifi_t > 300000) {
        wifi_t = millis();
        isWLANStatus = 1;
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("==> loop(): WiFi connected.");
            wifi_connect = true; 
        } else {
            wifi_connect = false; 
            Serial.println("==>> loop(): WiFi not connected!");
            WiFi.reconnect();
        }
    }
    
    // jede 5min prüfen, ob mit MQTT alles in Ordnung?
    if (millis() - mqtt_t > 300000) {
        mqtt_t = millis();
        isMQTTStatus = 1;
        if (!mqtt_client.connected()) {
            Serial.println("==>> loop(): MQTT not connected!");
            mqtt_reconnect();
        }
    }

    // jede 1s lux-Sensor auslesen und entspr. Boolean setzen
    if (millis() - lux_t > 1000) {
        lux_t = millis();
        isLuxValue = lux.getLux();
        if (isLuxValue < NIXIE_OFF_LUX) {
            is_dark = true;
        } else {
            is_dark = false;
        }
    }

    // MQTT-loop
    mqtt_client.loop();

}
