/**************************************************************
                      Nixie-Uhr
               =======================
               Uwe Berger; 2011, 2012
               bergeruw(at)gmx(dot)net   

Hardwarekonfiguration
=====================
 
MCU
---
Type..........: ATMega8

Fuse (avrdude): -U lfuse:w:0xe4:m -U hfuse:w:0xc9:m 
                (intern 8MHz; CKOPT fuer Uhrenquarz) 

TWI-Bus.......: optional bestueckbar mit:
                 LM75 (Temperatursensor)
                 TWI-DCF77 (http://bralug.de/wiki/BLIT2008-Board-DCF77)
	
INT0 (PD2)....: externer Eingang fuer Sekundentaktnormal, mit 
                dem stuendlich synchronisiert wird

4 Taster
--------
- MCU-Pins siehe Defines...
	
- Tastenfunktionen:
	
-->>--
|    |
| -------
| |Key 4|
| |-----|-------------------------------
| |Time |      |           |           |
| -------      |           |           |
|    |      -------     -------     -------
|    |      |Key 1|     |Key 2|     |Key 3|
|    v      |-----|     |-----|     |-----|
|    |      |Std++|     |Min++|     |Sek=0|
|    |      -------     -------     -------
| -------
| |Key 4|
| |-----|-------------------------------
| |Date |      |           |           |
| -------      |           |           |
|	 |      -------     -------     -------
|	 |      |Key 1|     |Key 2|     |Key 3|
|    v      |-----|     |-----|     |-----|
|    |      |Day++|     |Mon++|     |YY++ |
|    |      -------     -------     -------
| -------
| |Key 4|
| |-----|
| |Temp.|
| -------
|    |
|    |
|    v
|    |
|    |
| -----------
| |  Key 4  |
| |---------|
| |DCF77_NOK|
| -----------
|    |
--<<--
  

Schiebergister (SR); Typ: 74xx595x
----------------------------------
- vier SR in Reihe geschaltet mit folgenden Ausgaengen von der MCU:
	PC4 -> SER \
	PC5 -> SCK   \ 8Bit-Schieberegister 
	PC6 -> RCK   / (74xx595x; G auf L)
	PC7 -> SCL / 
 	- Verteilung
 		- SR 0...2 --> 6x BCD (Stelle 0...6) --> 10aus4-Dekoder (z.B.74141) --> Nixie
 		- SR 3 --> Anzeigepunkte, wenn vorhanden... (maximal 8 Punkte)


Fotowiderstand
--------------
-Typ: Reichelt.de LDR07
-verschaltet als Spannungsteiler an ADC3 (Pin 26)
	Vcc --> LDR --> ADC3-Input --> 10kOhm --> GND
	
	
IR-Empfaenger fuer Fernbedienung
--------------------------------
- Hardware ein TSOP1738 (mit entsprechender Standardbeschaltung) 
  an PC2
- dekodiert wird RC5 (entsprechende Programmabschnitte von Peter 
  Dannegger; http://www.mikrocontroller.net/topic/12216)
- RC5-Tastencode 1-3 entsprechen Taster 3-1; RC5 4 Taster 4  


---------
Have fun!
 
***************************************************************/ 

#ifndef F_CPU
#define F_CPU 8000000UL     				
#endif


#include <avr/interrupt.h>
#include <util/delay.h>
#include "i2cmaster.h" 


#define KEY1_DDR		DDRB
#define KEY1_PORT		PORTB
#define KEY1_PINX		PINB
#define KEY1_PIN		PB0
#define KEY1			(1<<KEY1_PIN)

#define KEY2_DDR		DDRB
#define KEY2_PORT		PORTB
#define KEY2_PINX		PINB
#define KEY2_PIN		PB1
#define KEY2			(1<<KEY2_PIN)

#define KEY3_DDR		DDRB
#define KEY3_PORT		PORTB
#define KEY3_PINX		PINB
#define KEY3_PIN		PB2
#define KEY3			(1<<KEY3_PIN)

#define KEY4_DDR		DDRD
#define KEY4_PORT		PORTD
#define KEY4_PINX		PIND
#define KEY4_PIN		PD7
#define KEY4			(1<<KEY4_PIN)

#define SR_DDR			DDRD
#define SR_PORT			PORTD
#define SR_SER			(1<<PD3)
#define SR_SCK			(1<<PD4)
#define SR_RCK			(1<<PD6)
#define SR_SCL			(1<<PD5)
#define SR_SER_HIGHT	SR_PORT |= SR_SER
#define SR_SER_LOW		SR_PORT &= ~SR_SER
#define SR_SER_TOGGLE	SR_PORT ^= SR_SER
#define SR_SCK_HIGHT	SR_PORT |= SR_SCK
#define SR_SCK_LOW		SR_PORT &= ~SR_SCK
#define SR_SCK_TOGGLE	SR_PORT ^= SR_SCK
#define SR_RCK_HIGHT	SR_PORT |= SR_RCK
#define SR_RCK_LOW		SR_PORT &= ~SR_RCK
#define SR_RCK_TOGGLE	SR_PORT ^= SR_RCK
#define SR_SCL_HIGHT	SR_PORT |= SR_SCL
#define SR_SCL_LOW		SR_PORT &= ~SR_SCL
#define SR_SCL_TOGGLE	SR_PORT ^= SR_SCL

#define ADDR_TINY 	0x20
#define ADDR_LM75 	0x92

#define DEBOUNCE_MS	100

#define PRELOAD_MANUELL	DEBOUNCE_MS/4	// 1000ms/256 = 4ms

#define KEY_CODE_1	1
#define KEY_CODE_2	2
#define KEY_CODE_3	3
#define KEY_CODE_4	4

#define DISPLAY_TIME			1
#define DISPLAY_DATE			2
#define DISPLAY_TEMP			3
#define DISPLAY_DCF77_NOK		4
#define DISPLAY_SCROLL			5
#define DEFAULT_MODE			DISPLAY_TIME

#define TIMEOUT_DEFAULT_MODE	5

#define ADC_DARK_VALUE			50

#define INT0_ON		GICR  |= (1<<INT0)
#define INT0_OFF	GICR  &= ~(1<<INT0)
#define INT0_CLEAR	GIFR  = (1<<INTF0)

#define	xRC5_IN		PINC
#define	xRC5		PC2							// IR input low active
#define RC5TIME 	1.778e-3					// 1.778msec
#define PULSE_MIN	(uint8_t)(F_CPU / 512 * RC5TIME * 0.4 + 0.5)
#define PULSE_1_2	(uint8_t)(F_CPU / 512 * RC5TIME * 0.8 + 0.5)
#define PULSE_MAX	(uint8_t)(F_CPU / 512 * RC5TIME * 1.2 + 0.5)

volatile uint8_t	rc5_bit;						// bit value
volatile uint8_t	rc5_time;						// count bit time
volatile uint16_t	rc5_tmp;						// shift bits in
volatile uint16_t	rc5_data;						// store result

uint8_t digit = 0;
volatile uint8_t click = 0;
typedef struct {uint8_t hs, ss, mm, hh, dd, mt, yy, wd, mez, mesz;} date_time_t;
volatile date_time_t dcf77, dt;

volatile union sr_data_t {
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

uint8_t display_mode, display_mode_temp;
uint8_t default_mode_counter;
static const uint8_t dd2mt[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
uint8_t h_temp, l_temp;
uint8_t dcf77_not_ok;
uint8_t sync_dcf77_sec;

uint32_t dcf77_nok_counter = 0;
uint8_t scroll_counter = 0;

uint8_t	display_blank = 0;


//**********************************************************************
void my_delay_ms(uint16_t ms)
{
	uint16_t i;
	for (i=0; i<ms; i++) _delay_ms(1);
}

//**********************************************************************
ISR(TIMER0_OVF_vect)
{
	uint16_t tmp = rc5_tmp;						// for faster access
	TCNT0 = -2;									// 2 * 256 = 512 cycle
	if( ++rc5_time > PULSE_MAX ){				// count pulse time
		if( !(tmp & 0x4000) && tmp & 0x2000 )	// only if 14 bits received
			rc5_data = tmp;
		tmp = 0;
	}
	if( (rc5_bit ^ xRC5_IN) & 1<<xRC5 ){		// change detect
		rc5_bit = ~rc5_bit;						// 0x00 -> 0xFF -> 0x00
		if( rc5_time < PULSE_MIN )				// to short
			tmp = 0;
		if( !tmp || rc5_time > PULSE_1_2 ){		// start or long pulse time
			if( !(tmp & 0x4000) )				// not to many bits
				tmp <<= 1;						// shift
			if( !(rc5_bit & 1<<xRC5) )			// inverted bit
				tmp |= 1;						// insert new bit
			rc5_time = 0;						// count next pulse time
		}
	}
	rc5_tmp = tmp;
}


//**********************************************************************
ISR(TIMER2_OVF_vect)
{
	click = 1;
}

//***********************************************************	
ISR(INT0_vect)
{
	if (sync_dcf77_sec) {
		sync_dcf77_sec = 0;
		TCNT2 = 255;
		while((ASSR & (1<< TCN2UB)));
	}
}	

//**********************************************************************
uint8_t lm75_read(void)
{
	// Anstoss senden
	if (i2c_start(ADDR_LM75+I2C_WRITE)) return 1;
	i2c_write(0);							
	i2c_stop();
	my_delay_ms(30);
	// Werte auslesen
	i2c_start(ADDR_LM75+I2C_READ);
	h_temp = i2c_readAck();
	l_temp = i2c_readNak();
	i2c_stop();
	return 0;
}

//***********************************************************	
uint8_t dcf77_read(void)
{
	// Anstoss senden
	if (i2c_start(ADDR_TINY+I2C_WRITE)) return 1;
	i2c_write(0);
	i2c_stop();
	my_delay_ms(30);
	// Werte lesen
	if (i2c_start(ADDR_TINY+I2C_READ)) return 1;
	dcf77.ss   = i2c_readAck();
	dcf77.mm   = i2c_readAck();
	dcf77.hh   = i2c_readAck();
	dcf77.dd   = i2c_readAck();
	dcf77.mt   = i2c_readAck();
	dcf77.yy   = i2c_readAck();
	dcf77.wd   = i2c_readAck();
	dcf77.mez  = i2c_readAck();
	dcf77.mesz = i2c_readNak();
	i2c_stop();
	// wenn Tag (oder Monat) Null, dann kein valides Datum
	if (!dcf77.dd) return 1;
	return 0;
}

//**********************************************************************
uint8_t sync_with_dcf77(void) {
	if (!dcf77_read()) {
		dt.ss = dcf77.ss;
		dt.mm = dcf77.mm;
		dt.hh = dcf77.hh;
		dt.dd = dcf77.dd;
		dt.mt = dcf77.mt;
		dt.yy = dcf77.yy;
		dcf77_not_ok=0;
		sync_dcf77_sec = 1;
		dcf77_nok_counter = 0;
	} else {
		dcf77_not_ok=1;
		return 1;
	}
	return 0;
}

//**********************************************************************
void add_time_date(void)
{
	dt.ss++;
	if (dt.ss > 59) {
		dt.ss = 0;
		dt.mm++;
		if (dt.mm > 59) {
			dt.mm = 0;
			dt.hh++;
			if (dt.hh > 23) {
				dt.hh = 0;
				dt.dd++;
				if (dt.dd > dd2mt[dt.mt-1]) {
					dt.dd = 1;
					dt.mt++;
					if (dt.mt > 12) {
						dt.mt = 1;
						dt.yy++;
						if (dt.yy > 99) {
							dt.yy = 0;
						}	
					}	
				}	
			}
		}	
	}
}

//**********************************************************************
void data2display(void)
{
	uint8_t i, j, mask;
	uint32_t t, c;
	
	
	if (display_blank) {
		// Anzeige dunkelschalten
		sr_data.sr.p5 = 0;
		sr_data.sr.p4 = 0;
		sr_data.sr.p3 = 0;
		sr_data.sr.p2 = 0;
		sr_data.sr.p1 = 0;
		sr_data.sr.p0 = 0;
		sr_data.sr.d0 = 15;
		sr_data.sr.d1 = 15;
		sr_data.sr.d2 = 15;
		sr_data.sr.d3 = 15;
		sr_data.sr.d4 = 15;
		sr_data.sr.d5 = 15;
	} else {
		// je nach Anzeige-Modus Daten in Anzeige-Puffer
		switch (display_mode) {
			case DISPLAY_TIME: 
					{
						sr_data.sr.p5 = 0;
						sr_data.sr.p4 = 1;
						sr_data.sr.p3 = 0;
						sr_data.sr.p2 = 1;
						sr_data.sr.p1 = 0;
						if (dcf77_not_ok) {
						sr_data.sr.p0=1;
						} else {
							sr_data.sr.p0 = 0;
						}
						sr_data.sr.d0 = dt.ss/10;
						sr_data.sr.d1 = dt.ss%10;
						sr_data.sr.d2 = dt.mm/10;
						sr_data.sr.d3 = dt.mm%10;
						sr_data.sr.d4 = dt.hh/10;
						sr_data.sr.d5 = dt.hh%10;
						break;
					}
			case DISPLAY_DATE: 
					{
						sr_data.sr.p5 = 0;
						sr_data.sr.p4 = 1;
						sr_data.sr.p3 = 0;
						sr_data.sr.p2 = 1;
						sr_data.sr.p1 = 0;
						if (dcf77_not_ok) {
							sr_data.sr.p0=1;
						} else {
							sr_data.sr.p0 = 0;
						}
						sr_data.sr.d0 = dt.yy/10;
						sr_data.sr.d1 = dt.yy%10;
						sr_data.sr.d2 = dt.mt/10;
						sr_data.sr.d3 = dt.mt%10;
						sr_data.sr.d4 = dt.dd/10;
						sr_data.sr.d5 = dt.dd%10;
						break;
					}
			case DISPLAY_DCF77_NOK: 
					{
						sr_data.sr.p5 = 0;
						sr_data.sr.p4 = 0;
						sr_data.sr.p3 = 0;
						sr_data.sr.p2 = 0;
						sr_data.sr.p1 = 0;
						sr_data.sr.p0 = 1;
						c = dcf77_nok_counter;
						t = c/100000;
						sr_data.sr.d4 = t;
						if (t) c = c - 100000*t;
						t = c/10000;
						sr_data.sr.d5 = t;
						if (t) c = c - 10000*t;
						t = c/1000;
						sr_data.sr.d2 = t;
						if (t) c = c - 1000*t;
						t = c/100;
						sr_data.sr.d3 = t;
						if (t) c = c - 100*t;
						sr_data.sr.d0 = c/10;
						sr_data.sr.d1 = c%10;
						break;
					}
			case DISPLAY_TEMP: 
					{
						if (lm75_read()) {
							// Temperaturlesen fehlerhaft
							sr_data.sr.p5 = 1;
							sr_data.sr.p4 = 1;
							sr_data.sr.p3 = 1;
							sr_data.sr.p2 = 1;
							sr_data.sr.p1 = 1;
							sr_data.sr.p0 = 1;
							sr_data.sr.d0 = 0;
							sr_data.sr.d1 = 0;
							sr_data.sr.d2 = 0;
							sr_data.sr.d3 = 0;
							sr_data.sr.d4 = 0;
							sr_data.sr.d5 = 0;
						} else {
							// Temperaturlesen ok
							sr_data.sr.p5 = 0;
							sr_data.sr.p4 = 0;
							sr_data.sr.p3 = 0;
							sr_data.sr.p2 = 0;
							sr_data.sr.p1 = 1;
							sr_data.sr.p0 = 0;
							// Vorzeichen (neg- --> ersten beiden Punkte links)
							if (h_temp >= 128) {	
								h_temp = ~(h_temp);	
								h_temp++;			
								sr_data.sr.p5 = 1;
								sr_data.sr.p4 = 1;
							}
							// Stelle 4 und 5 sind Null
							sr_data.sr.d4 = 0;
							sr_data.sr.d5 = 0;
							// 100ter-Stelle --> d2
							if (h_temp/100) {
								sr_data.sr.d2 = h_temp/100;
							} else {
								sr_data.sr.d2 = 0;
							}
							// 10er-Stelle
							if (h_temp/10) {
								sr_data.sr.d3 = h_temp/10;
							} else {
								sr_data.sr.d3 = 0;
							}
							// 1er-Stelle
							sr_data.sr.d0 = h_temp-h_temp/10*10;
							// Nachkomma
							if (l_temp >= 128) {
								sr_data.sr.d1 = 5;
							} else {
								sr_data.sr.d1 = 0;
							}
						}
						break;
					}
			case DISPLAY_SCROLL: 
					{
						sr_data.sr.p5 = 1;
						sr_data.sr.p4 = 1;
						sr_data.sr.p3 = 1;
						sr_data.sr.p2 = 1;
						sr_data.sr.p1 = 1;
						sr_data.sr.p0 = 1;
						sr_data.sr.d0 = scroll_counter;
						sr_data.sr.d1 = scroll_counter;
						sr_data.sr.d2 = scroll_counter;
						sr_data.sr.d3 = scroll_counter;
						sr_data.sr.d4 = scroll_counter;
						sr_data.sr.d5 = scroll_counter;
						break;
					}
		}
	}
	
	// Schieberegister zuruecksetzen
	SR_SCL_LOW;
	SR_SCL_HIGHT;
	// Ausgabe ins Schiebergister schieben
	for (j=0; j<4; j++) {
		mask = 0b10000000;
		for (i=0; i<8; i++) {
			if (sr_data.sr_array.d[j] & mask)
				// eine 1 reinschieben
				SR_SER_HIGHT;
			else
				// eine 0 reinschieben
				SR_SER_LOW;
			/* 0-1-Flanke an SCK schiebt Daten um eine Stelle weiter */
			SR_SCK_LOW;
			SR_SCK_HIGHT;
			mask /=2;
		}	
	}
	/* 0-1-Flanke an RCK bringt die Daten aus den Registern in die Latches */
	SR_RCK_LOW;
	SR_RCK_HIGHT;
}

//**********************************************************************
uint8_t scankey(volatile uint8_t *pinx, uint8_t pin) 
{
	if (bit_is_clear(*pinx, pin)) {
		my_delay_ms(DEBOUNCE_MS);
		if (bit_is_clear(*pinx, pin)) return 1;
	}
	return 0;
}

//**********************************************************************
uint8_t getkey(void) 
{
	uint16_t temp_key;
	uint16_t rc5_key_code;
	// Fernbedienung via RC5
	cli();               
	temp_key = rc5_data;			
	rc5_data = 0;
	sei();
	if (temp_key) {
		rc5_key_code   = ((temp_key & 0x3F) | (~temp_key >> 7 & 0x40));	
		if (rc5_key_code == 1) return KEY_CODE_3;
		if (rc5_key_code == 2) return KEY_CODE_2;
		if (rc5_key_code == 3) return KEY_CODE_1;
		if (rc5_key_code == 4) return KEY_CODE_4;
		return KEY_CODE_4;
	}
	// Hardware-Taster
	if (scankey(&KEY1_PINX, KEY1_PIN))	return KEY_CODE_1;
	if (scankey(&KEY2_PINX, KEY2_PIN))	return KEY_CODE_2;
	if (scankey(&KEY3_PINX, KEY3_PIN))	return KEY_CODE_3;
	if (scankey(&KEY4_PINX, KEY4_PIN))	return KEY_CODE_4;
	return 0;
}

//**********************************************************************
//**********************************************************************
//**********************************************************************
int main(void)
{
		
	// Eingaenge...
   	KEY1_PORT &= ~KEY1;
   	KEY2_PORT &= ~KEY2;
   	KEY3_PORT &= ~KEY3;
   	KEY4_PORT &= ~KEY4;
    
    // Ausgaenge...
   	SR_DDR |= SR_SER;
	SR_DDR |= SR_SCK;
	SR_DDR |= SR_RCK;
	SR_DDR |= SR_SCL;

    // Timerinterrupt konfigurieren
    ASSR   = (1<< AS2);					// Timer2 asynchron takten
    TCNT2  = 0;
    TCCR0  = 1<<CS02;					// 256
	TCCR2 |= (1 << CS22) | (1 << CS20);	// 128
	TIMSK |= (1 << TOIE2) | (1<<TOIE0);
	
	MCUCR |= (1<<ISC00) | (1<<ISC01);	// steigende Flanke (L->H) fuer INT0
	GICR  |= (1<<INT0);
	
	// ADC konfigurieren
	ADMUX = (1<<ADLAR) | (1<<REFS0) | (1<<MUX1) | (1<<MUX0);
	ADCSRA = (1<<ADEN) | (1<<ADPS0) | (1<<ADPS1);
    
	// Interrupts einschalten
	sei();
	
	// TWI-Zeugs initialisieren
	i2c_init();

	// u.U. koennte DCF77 bereits valid Zeit haben...
	sync_dcf77_sec = 0;
	if (sync_with_dcf77()) {
		dt.ss = 0;
		dt.mm = 0;
		dt.hh = 0;
		dt.dd = 1;
		dt.mt = 1;
		dt.yy = 0;
	}
	
	// initialer Anzeige-Modus
	display_mode = DISPLAY_TIME;
	default_mode_counter = 0;

	// Endlosschleife
	while(1){
		// ... es gibt was zu tun...
		if (click) {
			click = 0;
			
			// erst mal Zeit selbst hochzaehlen
			add_time_date();
			
			// jede volle Stunde oder wenn Problem mit DCF77-Modul, 
			// dann versuchen Zeit/Datum zu synchronisieren
			if ((!dt.mm && !dt.ss) || dcf77_not_ok) {
				// Minuten ohne gueltige DCF77-Daten hochzaehlen
				if (!dt.ss) {
					dcf77_nok_counter++;	
				}
				sync_with_dcf77();
			}  
			
			// Fotowiderstand abfragen
			ADCSRA |= (1<<ADSC);              
			while (ADCSRA & (1<<ADSC));
			if (ADCH < ADC_DARK_VALUE) {
				display_blank = 1;
			} else {
				display_blank = 0;				
			}
			
			// jede jede 10 min ein "Scrollen" der Anzeige einfuegen
			if (!dt.ss && !(dt.mm%10)) {
				display_mode_temp = display_mode;
				display_mode = DISPLAY_SCROLL;
				for (scroll_counter=0; scroll_counter < 10; scroll_counter++) {
					data2display();
					my_delay_ms(20);
					
				}
				display_mode = display_mode_temp;
			}
			
			// Display-Timeout aktualisieren
			if (default_mode_counter) {
				default_mode_counter--;	
			} else {
				display_mode = DEFAULT_MODE;
			}

			// jeweilige Daten anzeigen
			data2display();
		}
		
		// Tastaturabfrage
		switch (getkey()) {
			case KEY_CODE_1:
					{	
						switch (display_mode) {
							case DISPLAY_TIME:
									{
										dt.ss=0;
										// Timerinterrupt Preloader
										TCNT2 = PRELOAD_MANUELL;
										while((ASSR & (1<< TCN2UB)));
										break;
									}
							case DISPLAY_DATE:
									{
										dt.yy++;
										if (dt.yy > 99) dt.yy=0;
										default_mode_counter = TIMEOUT_DEFAULT_MODE;
										break;
									}
						}
						data2display();
						break;
					}
						
			case KEY_CODE_2:	
					{
						switch (display_mode) {
							case DISPLAY_TIME:
									{
										dt.mm++;
										if (dt.mm > 59) dt.mm=0;
										break;
									}
							case DISPLAY_DATE:
									{
										dt.mt++;
										if (dt.mt > 12) dt.mt=1;
										default_mode_counter = TIMEOUT_DEFAULT_MODE;
										break;
									}
						}
						data2display();
						break;
					}
						
			case KEY_CODE_3:
					{
						switch (display_mode) {
							case DISPLAY_TIME:
									{
										dt.hh++;
										if (dt.hh > 23) dt.hh=0;
										break;
									}
							case DISPLAY_DATE:
									{
										dt.dd++;
										if (dt.dd > dd2mt[dt.mt-1]) dt.dd=1;
										default_mode_counter = TIMEOUT_DEFAULT_MODE;
										break;
									}
						}
						data2display();
						break;
					}
						
			case KEY_CODE_4:
					{	
						switch (display_mode) {
							case DISPLAY_TIME:
									{
										display_mode = DISPLAY_DATE;
										default_mode_counter = TIMEOUT_DEFAULT_MODE;
										break;	
									}
							case DISPLAY_DATE:
									{
										display_mode = DISPLAY_TEMP;
										default_mode_counter = TIMEOUT_DEFAULT_MODE;
										break;	
									}
							case DISPLAY_TEMP:
									{
										display_mode = DISPLAY_DCF77_NOK;
										break;	
									}
							case DISPLAY_DCF77_NOK:
									{
										display_mode = DISPLAY_TIME;
										break;	
									}
						}
						data2display();
						break;
					}
		}
	}
}
