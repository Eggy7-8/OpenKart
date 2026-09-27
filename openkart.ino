#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TinyGPSPlus.h>

/* =========================================================================
   1. PIN DEFINITIONS & DISPLAY SETUP (Sunton 5.0" ESP32-8048S050)
   ========================================================================= */
#define GFX_BL 2 // Backlight Pin

// 16-bit Parallel RGB Bus Configuration for ST7262 800x480 Panel
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5  /* G0 */, 6  /* G1 */, 7  /* G2 */, 15 /* G3 */, 16 /* G4 */, 4  /* G5 */,
    8  /* B0 */, 3  /* B1 */, 46 /* B2 */, 9  /* B3 */, 1  /* B4 */,
    0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
    0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
    1 /* pclk_active_neg */, 16000000 /* prefer_speed (16MHz) */
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    800 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */
);

/* =========================================================================
   2. TACHOMETER CONFIGURATION (Predator 212 / Spark Wrap on IO19)
   ========================================================================= */
const int PIN_TACH = 19;
volatile unsigned long lastSparkTime = 0;
volatile unsigned long sparkInterval = 0;

// Interrupt Service Routine: Runs every time spark plug fires
void IRAM_ATTR sparkISR() {
  unsigned long now = micros();
  unsigned long interval = now - lastSparkTime;
  
  // Software filter: Reject spikes faster than 9,500 RPM (~6300 us)
  if (interval > 6300) {
    sparkInterval = interval;
    lastSparkTime = now;
  }
}

/* =========================================================================
   3. GPS CONFIGURATION (Beitian BE-220 on Serial Port)
   ========================================================================= */
// The JST 1.25mm serial port uses GPIO 43 (TX) and GPIO 44 (RX)
static const int RXPin = 44, TXPin = 43;
static const uint32_t GPSBaud = 38400; // BE-220 M10 default baud rate

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

/* =========================================================================
   4. TELEMETRY STATE TRACKING
   ========================================================================= */
float currentRPM = 0;
float currentMPH = 0;
float maxMPH     = 0;
int   satellites = 0;

unsigned long lastScreenUpdate = 0;
const unsigned long SCREEN_REFRESH_MS = 60; // ~16 FPS (Smooth & Responsive)

// Track previous values so we only redraw changed pixels (no flickering)
int lastDrawnRPMWidth = 0;
int lastDrawnSpeed    = -1;
int lastDrawnRPM      = -1;
int lastDrawnSats     = -1;
int lastDrawnMaxSpeed = -1;

/* =========================================================================
   5. STATIC DASHBOARD BACKGROUND (Drawn Once at Boot)
   ========================================================================= */
void drawStaticUI() {
  gfx->fillScreen(BLACK);

  // Tachometer Border Housing (Top 80px)
  gfx->drawRoundRect(30, 20, 740, 45, 8, DARKGREY);
  gfx->drawFastHLine(30, 80, 740, DARKGREY);

  // RPM Tick markers along tachometer bar
  // 1k, 2k, 3k, 4k, 5k RPM markers (Max 6000 RPM scale)
  for (int i = 1; i <= 5; i++) {
    int x = 30 + (i * (740 / 6));
    gfx->drawFastVLine(x, 15, 10, LIGHTGREY);
    gfx->setTextSize(1);
    gfx->setTextColor(LIGHTGREY);
    gfx->setCursor(x - 6, 2);
    gfx->printf("%dK", i);
  }

  // Speed Label Box
  gfx->setTextSize(3);
  gfx->setTextColor(LIGHTGREY);
  gfx->setCursor(440, 250);
  gfx->print("MPH");

  // Bottom Status Bar Dividers
  gfx->drawFastHLine(30, 390, 740, DARKGREY);
  gfx->setTextSize(2);
  gfx->setTextColor(LIGHTGREY);
  gfx->setCursor(50, 410);
  gfx->print("TOP SPEED:");

  gfx->setCursor(550, 410);
  gfx->print("GPS SATS:");
}

/* =========================================================================
   6. DYNAMIC GAUGES UPDATE
   ========================================================================= */
void updateDashboard() {
  // --- A. DRAW TACHOMETER SWEEP BAR ---
  // Scale 0 to 6000 RPM across 736 horizontal pixels
  int barWidth = map(constrain((int)currentRPM, 0, 6000), 0, 6000, 0, 734);
  
  if (barWidth != lastDrawnRPMWidth) {
    if (barWidth > lastDrawnRPMWidth) {
      // Draw new segment
      for (int x = lastDrawnRPMWidth; x < barWidth; x++) {
        uint16_t color = GREEN;
        if (x > 450) color = YELLOW; // Warning range (>3,600 RPM)
        if (x > 610) color = RED;    // Redline range (>5,000 RPM)
        gfx->drawFastVLine(33 + x, 23, 39, color);
      }
    } else {
      // Clear retracted segment
      gfx->fillRect(33 + barWidth, 23, lastDrawnRPMWidth - barWidth, 39, BLACK);
    }
    lastDrawnRPMWidth = barWidth;
  }

  // --- B. DRAW DIGITAL SPEEDOMETER (Centered Large Numbers) ---
  int displaySpeed = (int)currentMPH;
  if (displaySpeed != lastDrawnSpeed) {
    // Blank previous digits
    gfx->fillRect(150, 140, 280, 150, BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(18); // Huge, readable at 40 MPH
    gfx->setCursor(160, 145);
    gfx->printf("%2d", displaySpeed);
    lastDrawnSpeed = displaySpeed;
  }

  // --- C. DRAW DIGITAL RPM TEXT READOUT ---
  int displayRPM = (int)currentRPM;
  if (abs(displayRPM - lastDrawnRPM) > 50) { // Update in 50 RPM increments
    gfx->fillRect(500, 310, 250, 40, BLACK);
    gfx->setTextColor(CYAN);
    gfx->setTextSize(4);
    gfx->setCursor(500, 315);
    gfx->printf("%4d RPM", displayRPM);
    lastDrawnRPM = displayRPM;
  }

  // --- D. DRAW TOP SPEED ---
  int displayMax = (int)maxMPH;
  if (displayMax != lastDrawnMaxSpeed) {
    gfx->fillRect(220, 410, 120, 30, BLACK);
    gfx->setTextColor(GREENYELLOW);
    gfx->setTextSize(3);
    gfx->setCursor(220, 410);
    gfx->printf("%2d MPH", displayMax);
    lastDrawnMaxSpeed = displayMax;
  }

  // --- E. DRAW SATELLITE STATUS ---
  if (satellites != lastDrawnSats) {
    gfx->fillRect(690, 410, 70, 30, BLACK);
    gfx->setTextSize(3);
    if (satellites >= 4) {
      gfx->setTextColor(GREEN); // Solid 3D GPS Lock
    } else {
      gfx->setTextColor(RED);   // Searching for fix
    }
    gfx->setCursor(690, 410);
    gfx->printf("%2d", satellites);
    lastDrawnSats = satellites;
  }
}

/* =========================================================================
   7. ARDUINO SETUP & MAIN LOOP
   ========================================================================= */
void setup() {
  Serial.begin(115200);

  // Initialize GPS UART port
  gpsSerial.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);

  // Initialize Tachometer Pulse Interrupt
  pinMode(PIN_TACH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TACH), sparkISR, FALLING);

  // Backlight & Screen Initialization
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH); // Full Daylight Brightness

  if (!gfx->begin()) {
    Serial.println("Display Init Failed!");
  }

  drawStaticUI();
}

void loop() {
  // Feed GPS parser stream
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Calculate live engine RPM
  unsigned long now = micros();
  if ((now - lastSparkTime) < 400000 && sparkInterval > 0) {
    currentRPM = 60000000.0 / sparkInterval;
  } else {
    currentRPM = 0; // Engine is off or stalled
  }

  // Extract GPS speed (MPH) and satellites
  if (gps.speed.isValid()) {
    currentMPH = gps.speed.mph();
    if (currentMPH > maxMPH) {
      maxMPH = currentMPH;
    }
  }
  if (gps.satellites.isValid()) {
    satellites = gps.satellites.value();
  }

  // Redraw gauges at smooth intervals
  if (millis() - lastScreenUpdate >= SCREEN_REFRESH_MS) {
    lastScreenUpdate = millis();
    updateDashboard();
  }
}