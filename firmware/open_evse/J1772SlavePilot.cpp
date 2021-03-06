/*
 * This file is part of Open EVSE.

 * Open EVSE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.

 * Open EVSE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with Open EVSE; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#include "open_evse.h"
#include "J1772SlavePilot.h"

//#define SERDBG

#define LIBCALL_ENABLEINTERRUPT
#include <EnableInterrupt.h> 

#define MASTER_PILOT_PIN 2
#define SLAVE_PILOT_PIN 3

//#define DCOM_ENAB_PIN 5

volatile uint32_t t0=0,t1=0,t2=0, tLow=800, tHigh=200;
volatile uint16_t uSecPulsewidth = 200; //init to 12A

void pilotHigh(){
//  pinMode(SLAVE_PILOT_PIN,INPUT_PULLUP);
  digitalWrite(SLAVE_PILOT_PIN,HIGH);
  TCCR2A &= ~(_BV(COM2B0) | _BV(COM2B1));
}

void pilotPWM(){
  TCCR2A |= _BV(COM2B0) | _BV(COM2B1);
}

void pilotLow() {
//  digitalWrite(SLAVE_PILOT_PIN,LOW);
//  pinMode(SLAVE_PILOT_PIN,OUTPUT);
}

// one shot pulse generator from https://wp.josh.com/2015/03/05/the-perfect-pulse-some-tricks-for-generating-precise-one-shots-on-avr8/
#define OSP_SET_WIDTH(cycles) (OCR2B = 0xff-(cycles-1))
//#define OSP_SET_WIDTH(cycles) (OCR2A = 0xff-(cycles-1))

// Setup the one-shot pulse generator and initialize with a pulse width that is (cycles) clock counts long
void osp_setup(uint8_t cycles) {
  TCCR2B =  0;      // Halt counter by setting clock select bits to 0 (No clock source).
//  TCCR2A =  0;      // Halt counter by setting clock select bits to 0 (No clock source).
                    // This keeps anyhting from happeneing while we get set up

  TCNT2 = 0x00;     // Start counting at bottom. 
  OCR2B = 0;      // Set TOP to 0. This effectively keeps us from counting becuase the counter just keeps reseting back to 0.
          // We break out of this by manually setting the TCNT higher than 0, in which case it will count all the way up to MAX and then overflow back to 0 and get locked up again.
  OSP_SET_WIDTH(cycles);    // This also makes new OCR values get loaded frm the buffer on every clock cycle. 
//_BV(COM2B0) |
  TCCR2A =  _BV(COM2B0) | _BV(COM2B1) | _BV(WGM20) | _BV(WGM21); // OC2B=Set on Match, clear on BOTTOM. Mode 7 Fast PWM.
  //TCCR2A = _BV(COM2A0) | _BV(COM2A1) | _BV(WGM20) | _BV(WGM21); // OC2B=Set on Match, clear on BOTTOM. Mode 7 Fast PWM.
  TCCR2B = _BV(WGM22)| _BV(CS22);         // Start counting now. WGM22=1 to select Fast PWM mode 7
}


// Fire a one-shot pulse. Use the most recently set width. 
#define OSP_FIRE() (TCNT2 = OCR2B - 1)
//#define OSP_FIRE() (TCNT2 = OCR2A - 1)

// Test there is currently a pulse still in progress
#define OSP_INPROGRESS() (TCNT2>0)

// Fire a one-shot pulse with the specified width. 
// Order of operations in calculating m must avoid overflow of the unint8_t.
// TCNT2 starts one count lower than the match value because the chip will block any compare on the cycle after setting a TCNT. 
#define OSP_SET_AND_FIRE(cycles) {uint8_t m=0xff-(cycles-1); OCR2B=m; TCNT2=m-1;}
//#define OSP_SET_AND_FIRE(cycles) {uint8_t m=0xff-(cycles-1); OCR2A=m; TCNT2=m-1;}


#define TOP ((F_CPU / 2000000) * 1000) // for 1KHz (=1000us period)
volatile uint8_t cycles = 50, dutyCycleChanged=true;
volatile int16_t tMtoS;

void onMasterPilotChange() {
  int state = digitalRead(MASTER_PILOT_PIN);
  
  if (state == HIGH) { //rising 
    t0 =  micros();
    
    //MEVSE probes at middle of master pulse, adjust slave pulse such that this time is covered
    //25us is the phase delay caused by the ISR and OSP, cant get better than this
    tMtoS = max((tHigh>>1) - (cycles<<1), 0) - 25;    
 	  
    delayMicroseconds(tMtoS); // shift the high pulse to a time when master EVSE is probing for high on pilot, to prevent it detecting a low caused by shortened pulse

    if (dutyCycleChanged){ 
      OSP_SET_AND_FIRE(cycles);
      dutyCycleChanged = false;
    }
    else{ 
      OSP_FIRE();
    }
    
    tLow = t0-t1;
  } 
  else{//falling
    t1 = micros();
    tHigh = t1-t0;
  }

}

#if 0
void onDcomChange() {
  if (digitalRead(DCOM_ENAB_PIN) == LOW){
    uSecPulsewidth = 50;
    cycles = (uSecPulsewidth+5)/4;
    dutyCycleChanged = true;
  }
  else{
    uSecPulsewidth = 50;
    cycles = (uSecPulsewidth+5)/4;
    dutyCycleChanged = true;  
  }
}
#endif
  

void J1772SlavePilot::Init()
{
  pinMode(MASTER_PILOT_PIN, INPUT_PULLUP);
  
  //Enable Digital Communication pin
  //pinMode(DCOM_ENAB_PIN, INPUT_PULLUP);

  pinMode(SLAVE_PILOT_PIN, INPUT_PULLUP);
  osp_setup(cycles); 
  
  digitalWrite(SLAVE_PILOT_PIN, HIGH); // do not disturb until first SetPWM() call digitalRead(MASTER_PILOT_PIN));
  pinMode(SLAVE_PILOT_PIN, OUTPUT);
 
  //master pilot sensing
  enableInterrupt(MASTER_PILOT_PIN, onMasterPilotChange, CHANGE);   
  //digital communication request
  //enableInterrupt(DCOM_ENAB_PIN, onDcomChange, CHANGE);   
}


// no PWM pilot signal - steady state, dont do anything, but follow the master
// PILOT has to be externally NORed with MASTER pilot signal
// two diodes with cathode to signal sources, i.e. MASTER PILOT and PILOT pins, joined anodes and pull-up
// this will ensure that 
// 1. duty cycle stays below MASTER regardless of commanding,
// 2. MASTER failure overwrites any PWM or positive output on pilot

void J1772SlavePilot::SetState(PILOT_STATE state)
{
//  pilotHigh(); // forward master unchanged, high Z
  
  switch (state) {
    case PILOT_STATE_P12:
      pilotHigh();
      m_State = GetState();
    break;
    case PILOT_STATE_N12:
//    pilotLow();
      m_State = GetState();
    break;
    case PILOT_STATE_PWM:
      pilotPWM();  
      m_State = PILOT_STATE_PWM;
    break;
  }  
}


// set EVSE current capacity in Amperes
// duty cycle 
int J1772SlavePilot::SetPWM(int amps)
{
  int16_t limit = SenseMaster(); 
  amps = min( max(0,limit), amps); // limit commanding by amps value sensed on master pilot

  uSecPulsewidth = 0;
  if ((amps >= 6) && (amps <= 51)) {
    uSecPulsewidth = 100 * amps / 6;  // J1772 states "Available current = (duty cycle %) X 0.6"
  } else if ((amps > 51) && (amps <= 80)) {
    uSecPulsewidth = ((amps/2.5)+64)*10;  // J1772 states "Available current = (duty cycle % - 64) X 2.5"
  }
  else {
    return 1; // error
  }
  
#ifdef SERDBG
//  Serial.print(uSecPulsewidth);Serial.print(" usec pulsewidth set, i.e. amps:");Serial.println(amps);
#endif  
  if (uSecPulsewidth) {    
    pinMode(SLAVE_PILOT_PIN, OUTPUT);
    digitalWrite(SLAVE_PILOT_PIN, HIGH);
  
    AutoCriticalSection asc;
    
    // 10% = 24 , 96% = 239
    cycles = (uSecPulsewidth+5)/4;
    dutyCycleChanged = true;
    
    SetState(PILOT_STATE_PWM);
    return 0;
  }
  else { // !duty
    // invalid amps
    return 1;
  }
}


//returns amps and state of incoming master PWM signal 
int J1772SlavePilot::SenseMaster()
{
  uint32_t tPeriod = tLow+tHigh;
  uint16_t duty = (uint16_t)((double)tHigh*100/tPeriod);
  uint16_t amps = 0;

  if (tPeriod == 0){//steady master, nothing measured yet
    return 0;
  }
  
  if(tPeriod<950 || tPeriod>1050){
    //check if the master PWM signal is standard compliant (within measurement tolerances of the Arduino)
  #ifdef SERDBG
    Serial.print("Pilot PWM outside of 5% tolerance of 1kHz. tP,tL,tH[ms]="); Serial.println(tPeriod);Serial.println(tLow);Serial.println(tHigh);
  #endif
    return -1;
  } 
  if (duty <=5){
  #ifdef SERDBG
    Serial.println("Digital communication request sensed on master.");
  #endif
    return -2;
  }
  if (duty <= 85){
       amps = (uint16_t)(duty*0.6);
  }
  else{
     amps = (uint16_t)((duty-64)*2.5);
  }
  #ifdef SERDBG
    Serial.print(tHigh);Serial.print(", ");Serial.print(tLow);Serial.println(", ");
    Serial.print(duty);Serial.print(", ");Serial.print(amps);Serial.println(", ");
  #endif //#ifdef SERDBG
  return amps;  
}

void J1772SlavePilot::ReadPilot(uint16_t *plow, uint16_t *phigh)
{
  uint16_t pl = 1023;
  uint16_t ph = 0;

  // 1x = 114us 20x = 2.3ms 100x = 11.3ms
  for (int i = 0; i < PILOT_LOOP_CNT; i++) {
    uint16_t reading = adcPilot.read();  // measures pilot voltage

    if (reading > ph) {
      ph = reading;
    }
    else if (reading < pl) {
      pl = reading;
    }
  } 
  
  if (plow!=NULL && phigh!=NULL) {
    *plow = pl;
    *phigh = ph;
  }
}


PILOT_STATE J1772SlavePilot::GetState()  { 
  PILOT_STATE state = PILOT_STATE_PWM;
  uint16_t pl = 1023;
  uint16_t ph = 0;
  int16_t deltap = 0;

  ReadPilot(&pl, &ph);

  deltap = ph-pl;
 
  if (deltap<10){ //pilot steady 
    if (ph >= 500){
      m_State = PILOT_STATE_P12;
    }
    if (pl < 500){
      m_State = PILOT_STATE_N12;
    }
  }else{
    m_State = PILOT_STATE_PWM;
  }
 
  return state; 
}
