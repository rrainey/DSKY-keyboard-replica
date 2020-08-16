/*
Copyright (c) 2020, Riley Rainey

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <Arduino.h>
#include <Wire.h>

/*
 * This is the driver for the DSKY-matic keyboard.  It provides software debouncing of the
 * keyboard and sends key press/release events via the I2C interface when polled from the
 * Raspberry Pi 3/4
 */

#define PIN_BACKLIGHT 7
#define PIN_RED_LED 13

// consult variant.cpp for the dskymatic_m0 board definition
// to see how digital pins were added

#define PA_PIN(x) (23+(x))

/*
#define PIN_S1_STBY     PA_PIN(28)
#define PIN_S2_KEY_REL  PA_PIN(27)
#define PIN_S3_ENTR     PA_PIN(7)
#define PIN_S4_VERB     PA_PIN(6)
#define PIN_S5_NOUN     PA_PIN(1)
#define PIN_S6_CLR      PA_PIN(18)
#define PIN_S7_MINUS    PA_PIN(3)
#define PIN_S8_0        PA_PIN(4)
#define PIN_S9_PLUS     PA_PIN(2)
#define PIN_S10_1     PA_PIN(15)
#define PIN_S11_2     PA_PIN(18)
#define PIN_S12_4     PA_PIN(14)
#define PIN_S13_3     PA_PIN(23)
#define PIN_S14_5     PA_PIN(17)
#define PIN_S15_8     PA_PIN(16)
#define PIN_S16_6     PA_PIN(22)
#define PIN_S17_7     PA_PIN(5)
#define PIN_S18_9     PA_PIN(19)
#define PIN_S19_RSET  PA_PIN(11)
*/

#define SMAP_IGNORE 255
#define SMAP_SIZE 19

// (CLR) currently must be ignored in V2 board
const uint8_t s_map[SMAP_SIZE] = {
  PA_PIN(28),
  PA_PIN(27),
  PA_PIN(7),
  PA_PIN(6),
  PA_PIN(1),
  SMAP_IGNORE,
  PA_PIN(3),
  PA_PIN(4),
  PA_PIN(2),
  PA_PIN(15),
  PA_PIN(18),
  PA_PIN(14),
  PA_PIN(23),
  PA_PIN(17),
  PA_PIN(16),
  PA_PIN(22),
  PA_PIN(5),
  PA_PIN(19),
  PA_PIN(11)
};

enum KeyState {
  NORMAL = 0,
  PRESSED = 1
};

KeyState g_keystate[SMAP_SIZE];

uint16_t g_bounce[SMAP_SIZE];

struct _key_event {
  unsigned long time;
  uint8_t key;
  uint8_t  new_state;
};

/*
 * All key state changes are logged in a queue.
 * Items are removed from the queue by being 
 * polled via the I2C interface
 */
#define QUEUE_MAX 16

unsigned int qhead = 0;
unsigned int qtail = 0;
struct _key_event event_queue[QUEUE_MAX];

#define DEBOUNCE_IGNORE_MASK 0xe000
#define DEBOUNCE_TRIGGER     0xffff

void pushEvent(unsigned long time, uint8_t key, KeyState new_state) {
  unsigned int entry = (qtail + 1) % QUEUE_MAX;
  // overflowing? ignore this push request
  if (entry == qhead) {
    return;
  }
  struct _key_event *p = &event_queue[entry];

  p->time = time;
  p->key = key;
  p->new_state = new_state;
  qtail = entry;
}

struct _key_event *popEvent() {
  struct _key_event *p = NULL;
  if (qtail != qhead ) {
    p = &event_queue[qhead];
    qhead = (qhead+1) % QUEUE_MAX;
  }
  return p;
}

KeyState stateCheck(int key)
{
  int pin = s_map[key];
  if (pin == SMAP_IGNORE) {
    return NORMAL;
  }

  /*
   * g_bounce is basically a string of poll result history for a given key. 
   * This allows for a window of time for a key to settle into a new state.
   * Polling is currently performed on a 4ms interval and 13 bit history is maintained, so
   * the current configuration allows for a 4x13 = 52ms settle time.
   */
  
  if (g_keystate[key] == NORMAL) {
    g_bounce[key] = DEBOUNCE_IGNORE_MASK | (g_bounce[key] << 1) | (digitalRead(pin) == LOW) ? 1 : 0;

    if (g_bounce[key] == 0xffff) {
      g_keystate[key] = PRESSED;
      pushEvent(millis(), key, PRESSED);
      g_bounce[key] = DEBOUNCE_IGNORE_MASK;
    }
  }
  else {
    g_bounce[key] = DEBOUNCE_IGNORE_MASK | (g_bounce[key] << 1) | (digitalRead(pin) == HIGH) ? 1 : 0;

    if (g_bounce[key] == 0xffff) {
      g_keystate[key] = NORMAL;
      pushEvent(millis(), key, NORMAL);
      g_bounce[key] = DEBOUNCE_IGNORE_MASK;
    }
  }
}

void requestEvent() {
  struct _key_event *p = popEvent();
  if (p == NULL) {
    Wire.write("\[\]");
  }
  else {
    char buffer[64];
    sprintf(buffer, "\[%ul,%d,%d\]", p->time, p->key+1, p->new_state);
    Wire.write(buffer);
  }
}

void setup() 
{
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);

  pinMode(PIN_RED_LED, OUTPUT);
  digitalWrite(PIN_RED_LED, HIGH);

  for(int i=0; i<SMAP_SIZE; ++i) {
    if (s_map[i] != SMAP_IGNORE) {
      pinMode(s_map[i], INPUT_PULLUP);
      g_keystate[i] = NORMAL;
      g_bounce[i] = DEBOUNCE_IGNORE_MASK;
    }
  }

  /* 
   *  Connect to I2C Bus on address 0x10
   */
  Wire.begin( 0x10 );
  Wire.onRequest( requestEvent );
}

boolean first_time = false;
unsigned int tlast = 0;

void loop() {
  unsigned int delta_t;
  unsigned int t;
  
  if (first_time) {
    tlast = millis();
    first_time = false;
  }
  else {
    t = millis();
    delta_t = t - tlast;

    // poll key state every 4 milliseconds
    if (delta_t >= 4) {
      for(int i=0; i<SMAP_SIZE; ++i) {
        if (s_map[i] != SMAP_IGNORE) {
          stateCheck(i);
        }
      }
      tlast = t;
    }
  }
}
