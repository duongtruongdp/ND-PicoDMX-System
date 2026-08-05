/*
  ND DMX NODE 4U v2.0.1 TEMP FIX
  RP2040 + W5500 + four RS485 transmitters

  Recommended Arduino-Pico settings for a standard Raspberry Pi Pico:
    Board:       Raspberry Pi Pico
    CPU Speed:   133 MHz
    Flash Size:  2MB (no filesystem required)
    USB Stack:   Pico SDK
    Optimize:    Small (-Os)

  Architecture:
    Core 0: W5500, Art-Net, sACN, REST API and dashboard transport
    Core 1: deterministic four-port DMX frame engine
    PIO:    250 kbit/s, 8N2 UART payload

  The dashboard is a single flash-resident document. Large Ethernet writes are
  retried in 512-byte blocks and UDP is serviced between blocks, preventing the
  partial-page fault seen with a single large EthernetClient::print() call.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <EEPROM.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h" // Spinlock RP2040
#include "hardware/adc.h"  // Temp RP2040

#define HARDWARE_LED_PIN 23 

// Manage the PIO program loading offset to avoid duplicate loading that could cause the Core to freeze
static int pio0_offset = -1;
static int pio1_offset = -1;

// The flag indicates that the system has completed booting (Core cross-conflict error prevention)
volatile bool systemReady = false;
volatile uint32_t core1_heartbeat = 0; // Heart rate counter for safe watchdog loading between 2 cores
spin_lock_t* dmx_lock = nullptr;

// ================= PIO DMX DRIVER =================
// PIO UART TX 250 kbit/s, 8 data bits, no parity, 2 stop bits (DMX512 8N2).
// State-machine clock = 1 MHz, therefore one PIO cycle = 1 us.
// Each serial bit lasts 4 PIO cycles = 4 us.
//
// Equivalent PIO source:
//   .program dmx_tx
//   .side_set 1 opt
//   .wrap_target
//       pull block      side 1
//       set x, 7        side 0 [3]
//   bitloop:
//       out pins, 1            [2]
//       jmp x-- bitloop
//       nop             side 1 [6]
//   .wrap
static const uint16_t dmx_program_instructions[] = {
    0x98a0, // pull block side 1
    0xf327, // set x, 7 side 0 [3]
    0x6201, // out pins, 1 [2]
    0x0042, // jmp x--, 2
    0xbe42  // nop side 1 [6]
};
static const struct pio_program dmx_program = { dmx_program_instructions, 5, -1 };

class PIODMX {
private:
    PIO pio; uint sm; uint offset; uint pin; pio_sm_config c;
public:
    void begin(PIO pio_hw, uint sm_num, uint tx_pin) {
        pio = pio_hw; sm = sm_num; pin = tx_pin;
        
        if (pio == pio0) {
            if (pio0_offset == -1) {
                pio0_offset = pio_add_program(pio, &dmx_program);
            }
            offset = pio0_offset;
        } else {
            if (pio1_offset == -1) {
                pio1_offset = pio_add_program(pio, &dmx_program);
            }
            offset = pio1_offset;
        }

        c = pio_get_default_sm_config();
        sm_config_set_wrap(&c, offset + 0, offset + 4);
        sm_config_set_out_pins(&c, pin, 1);
        sm_config_set_sideset_pins(&c, pin);
        sm_config_set_sideset(&c, 2, true, false); // .side_set 1 opt
        sm_config_set_out_shift(&c, true, false, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (250000.0f * 4.0f));

        // Establish MARK/idle HIGH before handing the pin to PIO.
        pio_sm_set_pins_with_mask(pio, sm, 1u << pin, 1u << pin);
        pio_sm_set_pindirs_with_mask(pio, sm, 1u << pin, 1u << pin);
        pio_gpio_init(pio, pin);

        pio_sm_init(pio, sm, offset, &c);
        pio_sm_set_enabled(pio, sm, true);
    }
    
    // Push 1 byte into FIFO (Do not block the thread)
    inline bool put_byte(uint8_t b) {
        if (pio_sm_is_tx_fifo_full(pio, sm)) return false;
        pio_sm_put(pio, sm, b);
        return true;
    }

    // Get program offset
    uint getOffset() const { return offset; }
    
    // Arm the UART state machine immediately after the CPU-generated MAB.
    void reset_sm() {
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_restart(pio, sm);
        pio_sm_clear_fifos(pio, sm);
        pio_sm_exec(pio, sm, pio_encode_jmp(offset));
        pio_sm_clkdiv_restart(pio, 1u << sm);

        // Return the GPIO to this PIO block before enabling the SM.
        pio_gpio_init(pio, pin);
        pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
        pio_sm_set_enabled(pio, sm, true);
    }
};

PIODMX dmxOutputs[4];

// ================= HARDWARE ASSIGNMENT =================
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#define PIN_RST  20
const uint8_t RS485_PINS[4] = {8, 9, 14, 15};

byte mac[] = { 0x02, 0x4E, 0x44, 0x34, 0x55, 0x01 };
EthernetServer server(80);
EthernetUDP artnetUdp;
EthernetUDP sacnDdp;             // Unicast sACN Socket
EthernetUDP sacnMulticastUdp[4]; // Multicast sACN Sockets per Universe/Port

#define ARTNET_PORT 6454
#define SACN_PORT   5568

#define DEVICE_MODEL       "ND DMX NODE 4U"
#define FIRMWARE_VERSION   "2.0.1-temp-fix"
#define HARDWARE_REVISION  "RP2040 + W5500 + 4x RS485"
#define DASHBOARD_REFRESH_MS 1000
#define EVENT_LOG_SIZE 10

enum OutputTestMode : uint8_t {
  TEST_NETWORK = 0,
  TEST_BLACKOUT = 1,
  TEST_HALF = 2,
  TEST_FULL = 3,
  TEST_CHASE = 4
};

struct EventLogEntry {
  uint32_t timestampMs;
  char message[58];
};

// CONFIGURATION STRUCTURE V2
struct Config {
  uint16_t startUniverse; 
  uint8_t manualMode; 
  uint16_t portUniverses[4]; 
  uint8_t portEnabled[4];
  uint8_t portProtocol[4]; // 0: AUTO, 1: ARTNET, 2: SACN
  uint8_t mergeMode;       // 0: LTP, 1: HTP, 2: BLOCK
} cfg;

struct SystemDiagnostics {
  uint32_t core0Loops;
  uint32_t core1Loops;
  uint32_t maxNetTimeUs;
  uint32_t totalPacketsReceived;
  float mcuTemperatureC;
} sysLog;

enum DmxTxState {
  STATE_IDLE = 0,
  STATE_BREAK,
  STATE_MAB,
  STATE_SENDING
};

struct PortStatus {
  uint16_t universe; bool active; uint32_t lastPacketMs;
  uint32_t frameCount; uint32_t lastFpsMs; float fps; uint16_t usedChannels;
  volatile uint32_t dmxFrameCount; uint32_t lastDmxFpsMs; float dmxFps;
  uint8_t currentPriority; IPAddress sender; char protocol[8];
  
  // Conflict Detection
  bool conflict;
  IPAddress conflictIP;
  uint32_t lastConflictMs;

  // DOUBLE BUFFERING ARCHITECTURE
  uint8_t dmxBuffer[513];       // Back-buffer (Core 0 ghi)
  uint8_t activeDmxBuffer[513]; // Front-buffer (Core 1 đọc)
  volatile bool hasNewData; 
  uint32_t nextFrameTimeUs;     // The microsecond interval at which the next frame is allowed to be sent.
  
  // Unblocked transmission state
  DmxTxState state;
  uint32_t stateStartTimeUs;
  uint16_t txIndex;

  // Output test override. Network data remains in dmxBuffer and resumes immediately when stopped.
  volatile uint8_t testMode;
  uint16_t chaseChannel;
  uint32_t lastChaseStepMs;
};

PortStatus ports[4]; 
EventLogEntry eventLog[EVENT_LOG_SIZE];
uint8_t eventLogHead = 0;
uint8_t eventLogCount = 0;
uint8_t packetBuffer[768]; 
auto lastLinkStatus = LinkON;

volatile uint32_t lCount0 = 0, lCount1 = 0, maxNetTime = 0;

// ================= EEPROM ENGINE =================
uint8_t calcCRC(const Config& target) {
  uint8_t crc = lowByte(target.startUniverse) + highByte(target.startUniverse) + target.manualMode + target.mergeMode;
  for(int i=0; i<4; i++) {
    crc += lowByte(target.portUniverses[i]) + highByte(target.portUniverses[i]) + target.portEnabled[i] + target.portProtocol[i];
  }
  return crc;
}

void saveConfig() {
  EEPROM.write(0, 0x51); // Magic header for v2
  EEPROM.write(1, lowByte(cfg.startUniverse)); EEPROM.write(2, highByte(cfg.startUniverse)); EEPROM.write(3, cfg.manualMode);
  for(int i=0; i<4; i++) {
    EEPROM.write(4+(i*2), lowByte(cfg.portUniverses[i])); EEPROM.write(5+(i*2), highByte(cfg.portUniverses[i]));
    EEPROM.write(12+i, cfg.portEnabled[i]);
    EEPROM.write(16+i, cfg.portProtocol[i]);
  }
  EEPROM.write(20, cfg.mergeMode);
  EEPROM.write(21, calcCRC(cfg)); EEPROM.commit();
}

void setupSacnSockets(); // Forward declaration

void loadConfig() {
  EEPROM.begin(64);
  Config temp;
  temp.startUniverse = EEPROM.read(1) | (EEPROM.read(2) << 8);
  temp.manualMode = EEPROM.read(3);
  for(int i=0; i<4; i++) {
    temp.portUniverses[i] = EEPROM.read(4+(i*2)) | (EEPROM.read(5+(i*2)) << 8);
    temp.portEnabled[i] = EEPROM.read(12+i);
    temp.portProtocol[i] = EEPROM.read(16+i);
  }
  temp.mergeMode = EEPROM.read(20);
  
  uint8_t crc = calcCRC(temp);
  
  if (EEPROM.read(0) == 0x51 && EEPROM.read(21) == crc) {
    cfg = temp;
  } else {
    // Default settings
    cfg.startUniverse = 1; cfg.manualMode = 0; cfg.mergeMode = 0; // 0 = LTP
    for(int i=0; i<4; i++) { 
      cfg.portUniverses[i] = i + 1; 
      cfg.portEnabled[i] = 1; 
      cfg.portProtocol[i] = 0; // 0 = AUTO
    }
    saveConfig();
  }
}

void updateUniverseMap() {
  for (int i = 0; i < 4; i++) {
    ports[i].universe = (cfg.manualMode == 1) ? cfg.portUniverses[i] : (cfg.startUniverse + i);
    ports[i].active = false; ports[i].fps = 0; ports[i].usedChannels = 0; strcpy(ports[i].protocol, "-");
    ports[i].conflict = false; ports[i].hasNewData = false; ports[i].lastFpsMs = millis(); ports[i].nextFrameTimeUs = 0;
    ports[i].dmxFrameCount = 0; ports[i].lastDmxFpsMs = millis(); ports[i].dmxFps = 0;
    ports[i].state = STATE_IDLE; ports[i].txIndex = 0;
    ports[i].testMode = TEST_NETWORK; ports[i].chaseChannel = 1; ports[i].lastChaseStepMs = 0;
    memset((void*)ports[i].dmxBuffer, 0, 513);
    memset((void*)ports[i].activeDmxBuffer, 0, 513);
  }
  setupSacnSockets(); // Update the sACN Multicast Sockets
}

// Initialize sACN Multicast sockets according to the universe of each port
void setupSacnSockets() {
  sacnDdp.stop();
  sacnDdp.begin(SACN_PORT); // Unicast sACN
  
  for (int i = 0; i < 4; i++) {
    sacnMulticastUdp[i].stop();
    uint16_t u = ports[i].universe;
    if (cfg.portEnabled[i] && u > 0) {
      bool alreadyJoined = false;
      for (int j = 0; j < i; j++) {
        if (cfg.portEnabled[j] && ports[j].universe == u) {
          alreadyJoined = true;
          break;
        }
      }
      if (!alreadyJoined) {
        // ANSI E1.31 standard sACN Multicast address: 239.255.A.B (where Universe = A*256 + B)
        IPAddress mIP(239, 255, (u >> 8) & 0xFF, u & 0xFF);
        sacnMulticastUdp[i].beginMulticast(mIP, SACN_PORT);
      }
    }
  }
}

// ================= CORE 0: NETWORK & WEB SERVER =================
uint16_t getUsedChannels(uint8_t *data, uint16_t len) {
  for (int i = len - 1; i >= 0; i--) if (data[i] != 0) return i + 1;
  return 0;
}


float readMcuTemperatureC() {
  // Arduino-Pico reads the RP2040 internal temperature sensor through ADC4.
  // Discard the first conversion, then average only physically plausible samples.
  // A cached last-good value prevents one transient ADC fault from corrupting JSON.
  static float lastGoodTemperatureC = NAN;

  (void)analogReadTemp(3.3f);  // Throw away the first ADC conversion.
  delayMicroseconds(100);

  float sum = 0.0f;
  uint8_t validSamples = 0;

  for (uint8_t i = 0; i < 8; ++i) {
    const float sampleC = analogReadTemp(3.3f);

    if (isfinite(sampleC) && sampleC >= -40.0f && sampleC <= 125.0f) {
      sum += sampleC;
      ++validSamples;
    }

    delayMicroseconds(100);
  }

  // Require several valid samples before accepting a new reading.
  if (validSamples >= 4) {
    lastGoodTemperatureC = sum / validSamples;
  }

  return lastGoodTemperatureC;
}

void addEvent(const char* message) {
  EventLogEntry &entry = eventLog[eventLogHead];
  entry.timestampMs = millis();
  strncpy(entry.message, message, sizeof(entry.message) - 1);
  entry.message[sizeof(entry.message) - 1] = '\0';
  eventLogHead = (eventLogHead + 1) % EVENT_LOG_SIZE;
  if (eventLogCount < EVENT_LOG_SIZE) eventLogCount++;
}

void addPortEvent(int portIndex, const char* detail) {
  char message[58];
  snprintf(message, sizeof(message), "Port %d %s", portIndex + 1, detail);
  addEvent(message);
}

const char* testModeName(uint8_t mode) {
  switch (mode) {
    case TEST_BLACKOUT: return "BLACKOUT";
    case TEST_HALF: return "50 PERCENT";
    case TEST_FULL: return "FULL";
    case TEST_CHASE: return "CHASE";
    default: return "NETWORK";
  }
}

void printMac(EthernetClient& c) {
  for (int i = 0; i < 6; i++) {
    if (i) c.print(':');
    if (mac[i] < 16) c.print('0');
    c.print(mac[i], HEX);
  }
}

// Generate an ArtPollReply packet (240 bytes) and send it to the automatic detection software
void sendArtPollReply(IPAddress remoteIP)
{
    uint8_t reply[240];
    memset(reply, 0, sizeof(reply));

    // Art-Net Header
    memcpy(reply, "Art-Net\0", 8);

    // OpPollReply (0x2100)
    reply[8] = 0x00;
    reply[9] = 0x21;

    // IP Address
    IPAddress ip = Ethernet.localIP();
    reply[10] = ip[0];
    reply[11] = ip[1];
    reply[12] = ip[2];
    reply[13] = ip[3];

    // Port 0x1936 = 6454
    reply[14] = 0x36;
    reply[15] = 0x19;

    // Firmware version v1.0
    reply[16] = 0x00;
    reply[17] = 0x01;

    // NetSwitch / SubSwitch
    uint16_t u = ports[0].universe - 1;
    reply[18] = (u >> 8) & 0x7F;
    reply[19] = (u >> 4) & 0x0F;

    // OEM Code
    reply[20] = 0xFF;
    reply[21] = 0xFF;

    reply[22] = 0x00; // UBEA Version
    reply[23] = 0x08; // Status1: Normal
    reply[24] = 0x00; // ESTA Manufacturer
    reply[25] = 0x00;

    // Short Name (18 bytes)
    strncpy((char*)&reply[26], "ND DMX System", 17);

    // Long Name (64 bytes)
    strncpy((char*)&reply[44], "ND DMX System 4-Port Node", 63);

    // Node Report
    snprintf((char*)&reply[108], 63, "#0001 [0000] ND DMX System OK");

    // NumPorts = 4
    reply[172] = 0x00;
    reply[173] = 0x04;

    // Port Types (DMX Output = 0x80)
    for(int i = 0; i < 4; i++) reply[174 + i] = 0x80;

    // GoodInput
    memset(reply + 178, 0, 4);

    // GoodOutput
    for(int i = 0; i < 4; i++) reply[182 + i] = ports[i].active ? 0x80 : 0x00;

    // SwIn
    memset(reply + 186, 0, 4);

    // SwOut
    for(int i = 0; i < 4; i++) reply[190 + i] = (ports[i].universe - 1) & 0x0F;

    // Video/Macro/Spare
    memset(reply + 194, 0, 7);

    reply[201] = 0x00; // Style = StNode

    // MAC Address
    memcpy(reply + 202, mac, 6);

    // Bind IP
    reply[208] = ip[0]; reply[209] = ip[1]; reply[210] = ip[2]; reply[211] = ip[3];

    // Bind Index & Status2
    reply[212] = 1;
    reply[213] = 0x08; // Status2: Art-Net 4 support

    memset(reply + 214, 0, 26); // GoodOutputB

    // Send Unicast responses to the detection computer
    artnetUdp.beginPacket(remoteIP, ARTNET_PORT);
    artnetUdp.write(reply, 240);
    artnetUdp.endPacket();
}

void processDmxData(int i, uint8_t* d, uint16_t len, uint8_t prio, IPAddress ip, const char* name) {
  uint32_t now = millis();
  bool wasActive = ports[i].active;
  
  // 1. Check the Protocol per Port configuration (0: AUTO, 1: ARTNET, 2: SACN)
  if (cfg.portProtocol[i] == 1 && strcmp(name, "Art-Net") != 0) return;
  if (cfg.portProtocol[i] == 2 && strcmp(name, "sACN") != 0) return;

  // 2. Source IP address conflict detection (Conflict Detection)
  if (ports[i].active && (now - ports[i].lastPacketMs < 1000) && ip != ports[i].sender) {
    bool isNewConflict = !ports[i].conflict;
    ports[i].conflict = true;
    ports[i].conflictIP = ip;
    ports[i].lastConflictMs = now;
    if (isNewConflict) addPortEvent(i, "source conflict detected");
  }

  // 3. Process Merge Policy Mode = 2 (BLOCK)
  if (cfg.mergeMode == 2) {
    if (ports[i].active && (now - ports[i].lastPacketMs < 1000) && ip != ports[i].sender) {
      return; // Chặn nguồn IP thứ 2
    }
  }

  // 4. Check sACN Priority
  if (ports[i].active && strcmp(ports[i].protocol, "sACN") == 0 && prio < ports[i].currentPriority && (now - ports[i].lastPacketMs < 1000)) return; 
  
  ports[i].active = true; ports[i].lastPacketMs = now; ports[i].currentPriority = prio;
  ports[i].sender = ip;
  
  strncpy(ports[i].protocol, name, sizeof(ports[i].protocol) - 1);
  ports[i].protocol[sizeof(ports[i].protocol) - 1] = '\0';
  
  if (len > 512) len = 512;
  ports[i].usedChannels = getUsedChannels(d, len);
  
  uint32_t irq_status = spin_lock_blocking(dmx_lock);
  
  // 5. Merge Policy Mode = 1 (HTP - Highest Takes Precedence)
  if (cfg.mergeMode == 1 && ports[i].active) {
    for (uint16_t k = 0; k < len; k++) {
      if (d[k] > ports[i].dmxBuffer[1 + k]) {
        ports[i].dmxBuffer[1 + k] = d[k];
      }
    }
  } else { 
    // Merge Policy Mode = 0 (LTP - Latest)
    memcpy(ports[i].dmxBuffer + 1, d, len);
    if (len < 512) memset(ports[i].dmxBuffer + 1 + len, 0, 512 - len);
  }
  
  ports[i].hasNewData = true; 
  spin_unlock(dmx_lock, irq_status);
  
  if (!wasActive) {
    char detail[46];
    snprintf(detail, sizeof(detail), "%s source active", name);
    addPortEvent(i, detail);
  }

  ports[i].frameCount++; sysLog.totalPacketsReceived++;
}

// sACN Packet Extraction Function (ANSI E1.31 Standard Packet)
void parseSacnPacket(EthernetUDP& udp) {
  int sacnSize = udp.parsePacket();
  if (sacnSize >= 126 && sacnSize <= (int)sizeof(packetBuffer)) {
    udp.read(packetBuffer, sacnSize);
    
    // Check the ACN Packet Identifier ("ASC-E1.17" at byte 4-12)
    if (memcmp(packetBuffer + 4, "ASC-E1.17", 9) == 0) {
      // Root Vector (0x04) & Framing Vector (0x02)
      if (packetBuffer[21] == 0x04 && packetBuffer[43] == 0x02) {
        uint16_t u = (packetBuffer[113] << 8) | packetBuffer[114];
        uint16_t rawLen = (packetBuffer[123] << 8) | packetBuffer[124];
        uint16_t len = (rawLen > 0) ? rawLen - 1 : 0;
        uint8_t prio = packetBuffer[108];
        
        for (int i = 0; i < 4; i++) {
          if (u == ports[i].universe && cfg.portEnabled[i]) {
            processDmxData(i, packetBuffer + 126, len, prio, udp.remoteIP(), "sACN");
            break;
          }
        }
      }
    }
  }
}

void handleNetworkPackets() {
  // 1. ART-NET PACKET PROCESSING
  int artSize = artnetUdp.parsePacket();
  if (artSize >= 10 && artSize <= (int)sizeof(packetBuffer)) {
    artnetUdp.read(packetBuffer, artSize);
    if (memcmp(packetBuffer, "Art-Net", 8) == 0) {
      uint16_t opcode = packetBuffer[8] | (packetBuffer[9] << 8);
      if (opcode == 0x5000) { // ArtDmx
        if (artSize >= 18) {
          uint16_t len = (packetBuffer[16] << 8) | packetBuffer[17];
          if (18 + len <= artSize) {
            uint16_t u = packetBuffer[14] | (packetBuffer[15] << 8);
            for (int i = 0; i < 4; i++) {
              if (u == ports[i].universe && cfg.portEnabled[i]) {
                processDmxData(i, packetBuffer + 18, len, 100, artnetUdp.remoteIP(), "Art-Net");
                break;
              }
            }
          }
        }
      } else if (opcode == 0x2000) { // ArtPoll
        sendArtPollReply(artnetUdp.remoteIP());
      }
    }
  }

  // 2. PROCESSING sACN PACKETS (BOTH UNICAST AND MULTICAST FROM LIGHTKEY)
  parseSacnPacket(sacnDdp); // Unicast sACN
  for (int i = 0; i < 4; i++) {
    parseSacnPacket(sacnMulticastUdp[i]); // Multicast sACN per Port
  }
}

int getParamValue(const char* req, const char* key) {
  char searchKey[16]; snprintf(searchKey, sizeof(searchKey), "%s=", key);
  char* pos = strstr((char*)req, searchKey); if (!pos) return -1;
  return atoi(pos + strlen(searchKey));
}

void printIP(EthernetClient& c, IPAddress ip) {
  c.print(ip[0]); c.print('.');
  c.print(ip[1]); c.print('.');
  c.print(ip[2]); c.print('.');
  c.print(ip[3]);
}

static const char INDEX_HTML[] = R"NDWEB(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="color-scheme" content="dark">
  <title>ND DMX Node 4U</title>
  <style>
    :root{
      --bg:#05070b;--panel:#0d131d;--panel-2:#090e16;--line:#202b39;
      --text:#f5f7fb;--muted:#8996a8;--blue:#3487ff;--cyan:#26d5e8;
      --green:#36d486;--amber:#ffbf47;--red:#ff627c;
      --headline:Impact,Haettenschweiler,"Arial Narrow Bold",sans-serif;
      --body:"Helvetica Neue",Helvetica,Arial,sans-serif;
      --mono:Consolas,"Liberation Mono",Menlo,monospace;
    }
    *{box-sizing:border-box}
    html{background:var(--bg)}
    body{margin:0;min-height:100vh;color:var(--text);font-family:var(--body);background:
      radial-gradient(circle at 10% -10%,rgba(52,135,255,.20),transparent 31%),
      radial-gradient(circle at 100% 15%,rgba(38,213,232,.09),transparent 27%),var(--bg)}
    button,input,select{font:inherit}
    button{touch-action:manipulation}
    .shell{width:min(1220px,calc(100% - 36px));margin:auto;padding:28px 0 42px}
    .topbar{display:flex;align-items:flex-start;justify-content:space-between;gap:20px;margin-bottom:24px}
    .brand{display:flex;align-items:flex-start;gap:16px}
    .brand-mark{display:grid;grid-template-columns:repeat(4,6px);align-items:end;gap:4px;width:46px;height:48px;padding:7px;border:1px solid rgba(52,135,255,.38);border-radius:12px;background:rgba(52,135,255,.08);box-shadow:0 0 30px rgba(52,135,255,.12)}
    .brand-mark i{display:block;border-radius:3px 3px 1px 1px;background:linear-gradient(#55a2ff,#2563eb);box-shadow:0 0 9px rgba(52,135,255,.45)}
    .brand-mark i:nth-child(1){height:18px}.brand-mark i:nth-child(2){height:30px}.brand-mark i:nth-child(3){height:24px}.brand-mark i:nth-child(4){height:34px}
    .eyebrow{color:var(--blue);font-family:var(--headline);font-size:11px;letter-spacing:.20em}
    h1{margin:3px 0 0;font-family:var(--headline);font-size:clamp(40px,7vw,72px);font-weight:900;line-height:.9;letter-spacing:.025em}
    .subtitle{margin:11px 0 0;color:var(--muted);font-size:12px;letter-spacing:.02em}
    .online-pill{display:flex;align-items:center;gap:9px;padding:10px 14px;border:1px solid rgba(54,212,134,.28);border-radius:999px;color:#64eea2;background:rgba(54,212,134,.08);font-family:var(--headline);font-size:11px;letter-spacing:.10em;white-space:nowrap}
    .online-pill.offline{color:#ff879a;border-color:rgba(255,98,124,.32);background:rgba(255,98,124,.09)}
    .led{width:8px;height:8px;border-radius:50%;background:currentColor;box-shadow:0 0 13px currentColor}
    .panel{position:relative;overflow:hidden;margin-bottom:18px;padding:22px;border:1px solid var(--line);border-radius:17px;background:linear-gradient(145deg,rgba(255,255,255,.018),transparent 44%),var(--panel);box-shadow:0 18px 50px rgba(0,0,0,.22),inset 0 1px rgba(255,255,255,.025)}
    .panel:before{content:"";position:absolute;top:0;left:24px;right:24px;height:1px;background:linear-gradient(90deg,transparent,rgba(52,135,255,.72),transparent)}
    .section-head{display:flex;align-items:flex-end;justify-content:space-between;gap:14px;margin-bottom:18px}
    h2,h3,.metric-value,.universe,.status-badge,.primary-btn{font-family:var(--headline);font-weight:900;letter-spacing:.04em}
    h2{margin:0;font-size:21px;text-transform:uppercase}.section-note{margin:4px 0 0;color:var(--muted);font-size:11px}.last-update{color:var(--muted);font-family:var(--mono);font-size:10px}
    .metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
    .metric{min-height:104px;padding:15px;border:1px solid var(--line);border-radius:13px;background:var(--panel-2)}
    .metric-label{color:var(--muted);font-size:9px;font-weight:700;letter-spacing:.14em;text-transform:uppercase}
    .metric-value{margin-top:12px;font-family:var(--mono);font-size:20px;line-height:1.15;overflow-wrap:anywhere}
    .metric-note{margin-top:7px;color:#667487;font-size:9px}
    .ports{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
    .port-card{position:relative;padding:19px;border:1px solid var(--line);border-radius:15px;background:linear-gradient(145deg,rgba(255,255,255,.022),transparent 48%),var(--panel-2);transition:transform .16s,border-color .16s}
    .port-card:hover{transform:translateY(-2px);border-color:rgba(52,135,255,.43)}
    .port-card.override{border-color:rgba(38,213,232,.32)}.port-card.conflict{border-color:rgba(255,191,71,.42)}
    .port-card h3{margin:0;font-size:16px}.status-badge{position:absolute;top:17px;right:17px;display:flex;align-items:center;gap:7px;padding:6px 9px;border:1px solid;border-radius:999px;font-size:9px;letter-spacing:.10em}
    .status-badge:before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor;box-shadow:0 0 9px currentColor}
    .s-active{color:#61eca1;border-color:rgba(54,212,134,.28);background:rgba(54,212,134,.08)}
    .s-standby{color:#93a1b4;border-color:rgba(147,161,180,.20);background:rgba(147,161,180,.06)}
    .s-degraded,.s-conflict{color:#ffd16d;border-color:rgba(255,191,71,.31);background:rgba(255,191,71,.08)}
    .s-disabled{color:#ff879a;border-color:rgba(255,98,124,.28);background:rgba(255,98,124,.08)}
    .s-test{color:#6be8f4;border-color:rgba(38,213,232,.31);background:rgba(38,213,232,.08)}
    .universe-block{margin:24px 0 13px}.micro-label{color:var(--muted);font-size:9px;font-weight:700;letter-spacing:.15em;text-transform:uppercase}
    .universe{margin-top:3px;font-family:var(--mono);font-size:47px;line-height:1}.quality{margin-left:10px;color:var(--cyan);font-family:var(--mono);font-size:10px;vertical-align:middle}
    .channel-head{display:flex;justify-content:space-between;margin-top:12px;color:var(--muted);font-size:9px}.channel-head b{color:#cbd5e1;font-family:var(--mono);font-weight:400}
    .channel-track{height:6px;margin:7px 0 12px;overflow:hidden;border-radius:99px;background:#1a2532}.channel-fill{height:100%;width:0;border-radius:inherit;background:linear-gradient(90deg,var(--blue),var(--cyan));box-shadow:0 0 11px rgba(38,213,232,.32);transition:width .25s}
    .data-row{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:35px;padding:6px 0;border-bottom:1px solid rgba(137,150,168,.11);color:var(--muted);font-size:11px}.data-row:last-child{border-bottom:0}.data-row b{color:var(--text);font-family:var(--mono);font-size:11px;font-weight:400;text-align:right;overflow-wrap:anywhere}
    .conflict-box{display:none;margin-top:11px;padding:9px 10px;border:1px solid rgba(255,191,71,.28);border-radius:8px;color:#ffe09a;background:rgba(255,191,71,.07);font-size:10px;font-weight:700}.conflict-box.show{display:block}
    .test-zone{margin-top:14px;padding-top:13px;border-top:1px solid rgba(137,150,168,.13)}.test-zone-head{display:flex;justify-content:space-between;margin-bottom:8px;color:var(--muted);font-size:9px;font-weight:700;letter-spacing:.12em}.test-mode-name{color:var(--cyan);font-family:var(--mono);letter-spacing:0}
    .test-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:6px}.test-btn{padding:8px 3px;border:1px solid var(--line);border-radius:7px;color:#aeb9c8;background:#101925;font-family:var(--mono);font-size:8px;cursor:pointer}.test-btn:hover,.test-btn.selected{color:#fff;border-color:rgba(38,213,232,.48);background:rgba(38,213,232,.11)}.test-btn:disabled{opacity:.35;cursor:not-allowed}
    details.panel{padding:0}summary{padding:22px;cursor:pointer;list-style:none;font-family:var(--headline);font-size:20px;letter-spacing:.04em;text-transform:uppercase}summary::-webkit-details-marker{display:none}summary:after{content:"+";float:right;color:var(--blue)}details[open] summary:after{content:"−"}.config-content{padding:0 22px 22px;border-top:1px solid rgba(137,150,168,.10)}
    .mode-row{display:flex;flex-wrap:wrap;gap:19px;padding:18px 0}.mode-row label{display:flex;align-items:center;gap:7px;color:#d5dde8;font-size:11px}.mode-row input{accent-color:var(--blue)}
    .config-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:11px}.config-line{display:grid;grid-template-columns:58px 1fr 1fr 1.35fr;align-items:center;gap:8px;padding:10px;border:1px solid var(--line);border-radius:10px;background:var(--panel-2)}.config-line strong{font-family:var(--headline);font-size:12px;letter-spacing:.05em}
    .field-label{display:block;margin:0 0 6px;color:var(--muted);font-size:9px;font-weight:700;letter-spacing:.11em;text-transform:uppercase}.config-row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:13px}.field{padding:12px;border:1px solid var(--line);border-radius:10px;background:var(--panel-2)}
    input[type=number],select{width:100%;min-height:36px;padding:7px 9px;border:1px solid var(--line);border-radius:7px;color:#fff;background:#060b12;font-family:var(--mono);font-size:10px;outline:none}input:focus,select:focus{border-color:rgba(52,135,255,.7);box-shadow:0 0 0 2px rgba(52,135,255,.10)}
    .actions{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:15px}.config-hint{color:#68768a;font-size:9px}.primary-btn{padding:10px 17px;border:0;border-radius:8px;color:#fff;background:linear-gradient(135deg,#2563eb,#3b8cff);font-size:12px;letter-spacing:.06em;cursor:pointer}.primary-btn:disabled{opacity:.5;cursor:wait}
    .diag-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:11px}.diag-box{padding:14px;border:1px solid var(--line);border-radius:11px;background:var(--panel-2)}.diag-box span{display:block;color:var(--muted);font-size:9px;letter-spacing:.11em}.diag-box b{display:block;margin-top:8px;font-family:var(--mono);font-size:17px;font-weight:400}.diag-bar{height:4px;margin-top:10px;overflow:hidden;border-radius:99px;background:#182330}.diag-bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--blue),var(--cyan));transition:width .25s}
    .event-list{display:grid;gap:7px}.event-item{display:grid;grid-template-columns:78px 1fr;gap:12px;padding:9px 11px;border:1px solid rgba(137,150,168,.11);border-radius:9px;background:var(--panel-2);font-size:11px}.event-time{color:var(--cyan);font-family:var(--mono)}.event-message{color:#c8d1de}.empty{color:var(--muted);text-align:center;padding:18px;font-size:11px}
    .device-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 28px}.footer{padding:5px 0 0;color:#5f6c7e;text-align:center;font-family:var(--mono);font-size:9px;letter-spacing:.08em}
    .toast{position:fixed;right:20px;bottom:20px;z-index:10;max-width:min(380px,calc(100% - 40px));padding:11px 14px;border:1px solid rgba(54,212,134,.3);border-radius:9px;color:#dffbed;background:#0d2119;box-shadow:0 14px 40px rgba(0,0,0,.35);font-size:11px;opacity:0;transform:translateY(10px);pointer-events:none;transition:.2s}.toast.show{opacity:1;transform:none}.toast.error{color:#ffe1e6;border-color:rgba(255,98,124,.32);background:#281016}
    .offline-banner{display:none;margin-bottom:15px;padding:10px 13px;border:1px solid rgba(255,98,124,.28);border-radius:10px;color:#ffc2cc;background:rgba(255,98,124,.08);font-size:11px}.offline-banner.show{display:block}
    noscript{display:block;padding:14px;color:#ffe09a;background:#2b210c;font-family:var(--mono)}
    @media(max-width:930px){.metrics{grid-template-columns:repeat(2,1fr)}.ports{grid-template-columns:1fr}.config-grid{grid-template-columns:1fr}}
    @media(max-width:650px){.shell{width:min(100% - 24px,1220px);padding-top:17px}.topbar{flex-direction:column}.brand-mark{display:none}.panel{padding:17px}.metrics{grid-template-columns:1fr 1fr}.section-head{align-items:flex-start;flex-direction:column}.test-grid{grid-template-columns:repeat(3,1fr)}.config-line{grid-template-columns:1fr 1fr}.config-line strong{grid-column:1/-1}.config-row,.diag-grid,.device-grid{grid-template-columns:1fr}.event-item{grid-template-columns:66px 1fr}summary{padding:18px}.config-content{padding:0 17px 17px}}
    @media(max-width:410px){.metrics{grid-template-columns:1fr}.universe{font-size:42px}}
  </style>
</head>
<body>
<noscript>This dashboard requires JavaScript. DMX output continues to run normally.</noscript>
<div class="shell">
  <header class="topbar">
    <div class="brand">
      <div class="brand-mark" aria-hidden="true"><i></i><i></i><i></i><i></i></div>
      <div><div class="eyebrow">NETWORK DMX CONTROLLER</div><h1>ND DMX NODE 4U</h1><p class="subtitle">Art-Net / sACN to four-port DMX512 gateway</p></div>
    </div>
    <div class="online-pill" id="online-pill"><span class="led"></span><span id="online-text">CONNECTING</span></div>
  </header>

  <div class="offline-banner" id="offline-banner">Dashboard communication is unavailable. Physical DMX output continues using the last valid buffers.</div>

  <section class="panel">
    <div class="section-head"><div><h2>System Overview</h2><p class="section-note">Live telemetry. The DMX engine remains isolated on Core 1.</p></div><div class="last-update" id="last-update">WAITING FOR NODE</div></div>
    <div class="metrics">
      <div class="metric"><div class="metric-label">Device IP</div><div class="metric-value" id="m-ip">--</div><div class="metric-note">Static Ethernet address</div></div>
      <div class="metric"><div class="metric-label">Uptime</div><div class="metric-value" id="m-uptime">--</div><div class="metric-note">Since last restart</div></div>
      <div class="metric"><div class="metric-label">Packets Received</div><div class="metric-value" id="m-packets">--</div><div class="metric-note"><span id="m-pps">0.0</span> accepted packets/s</div></div>
      <div class="metric"><div class="metric-label">Ethernet Link</div><div class="metric-value" id="m-link">--</div><div class="metric-note">W5500 physical connection</div></div>
      <div class="metric"><div class="metric-label">MCU Temperature</div><div class="metric-value" id="m-temp">--</div><div class="metric-note">RP2040 internal sensor</div></div>
      <div class="metric"><div class="metric-label">Active Sources</div><div class="metric-value" id="m-active">--</div><div class="metric-note">Network-fed output ports</div></div>
      <div class="metric"><div class="metric-label">Live Protocol</div><div class="metric-value" id="m-protocol">--</div><div class="metric-note">Art-Net / sACN activity</div></div>
      <div class="metric"><div class="metric-label">Merge Policy</div><div class="metric-value" id="m-merge">--</div><div class="metric-note">Conflict handling mode</div></div>
    </div>
  </section>

  <section class="panel">
    <div class="section-head"><div><h2>DMX Output Ports</h2><p class="section-note">Input telemetry, output refresh and local test override.</p></div></div>
    <div class="ports">
      <article class="port-card" id="port-1">
        <h3>OUTPUT PORT 1</h3><div class="status-badge s-standby" id="p1-badge">STANDBY</div>
        <div class="universe-block"><div class="micro-label">UNIVERSE <span class="quality" id="p1-quality">NO SOURCE</span></div><div class="universe" id="p1-universe">---</div></div>
        <div class="channel-head"><span>CHANNEL ACTIVITY</span><b><span id="p1-used">0</span> / 512</b></div><div class="channel-track"><div class="channel-fill" id="p1-fill"></div></div>
        <div class="data-row"><span>Input FPS</span><b id="p1-ifps">0.0 Hz</b></div>
        <div class="data-row"><span>DMX Output</span><b id="p1-ofps">0.0 Hz</b></div>
        <div class="data-row"><span>Protocol</span><b id="p1-proto">-</b></div>
        <div class="data-row"><span>Sender IP</span><b id="p1-sender">-</b></div>
        <div class="data-row"><span>Last Packet</span><b id="p1-age">-</b></div>
        <div class="data-row"><span>sACN Priority</span><b id="p1-prio">-</b></div>
        <div class="conflict-box" id="p1-conflict">SOURCE CONFLICT</div>
        <div class="test-zone"><div class="test-zone-head"><span>OUTPUT TEST</span><span class="test-mode-name" id="p1-test-name">NETWORK</span></div>
          <div class="test-grid">
            <button type="button" class="test-btn" data-port="0" data-mode="0">STOP</button>
            <button type="button" class="test-btn" data-port="0" data-mode="1">0%</button>
            <button type="button" class="test-btn" data-port="0" data-mode="2">50%</button>
            <button type="button" class="test-btn" data-port="0" data-mode="3">100%</button>
            <button type="button" class="test-btn" data-port="0" data-mode="4">CHASE</button>
          </div>
        </div>
      </article>
      <article class="port-card" id="port-2">
        <h3>OUTPUT PORT 2</h3><div class="status-badge s-standby" id="p2-badge">STANDBY</div>
        <div class="universe-block"><div class="micro-label">UNIVERSE <span class="quality" id="p2-quality">NO SOURCE</span></div><div class="universe" id="p2-universe">---</div></div>
        <div class="channel-head"><span>CHANNEL ACTIVITY</span><b><span id="p2-used">0</span> / 512</b></div><div class="channel-track"><div class="channel-fill" id="p2-fill"></div></div>
        <div class="data-row"><span>Input FPS</span><b id="p2-ifps">0.0 Hz</b></div>
        <div class="data-row"><span>DMX Output</span><b id="p2-ofps">0.0 Hz</b></div>
        <div class="data-row"><span>Protocol</span><b id="p2-proto">-</b></div>
        <div class="data-row"><span>Sender IP</span><b id="p2-sender">-</b></div>
        <div class="data-row"><span>Last Packet</span><b id="p2-age">-</b></div>
        <div class="data-row"><span>sACN Priority</span><b id="p2-prio">-</b></div>
        <div class="conflict-box" id="p2-conflict">SOURCE CONFLICT</div>
        <div class="test-zone"><div class="test-zone-head"><span>OUTPUT TEST</span><span class="test-mode-name" id="p2-test-name">NETWORK</span></div>
          <div class="test-grid">
            <button type="button" class="test-btn" data-port="1" data-mode="0">STOP</button>
            <button type="button" class="test-btn" data-port="1" data-mode="1">0%</button>
            <button type="button" class="test-btn" data-port="1" data-mode="2">50%</button>
            <button type="button" class="test-btn" data-port="1" data-mode="3">100%</button>
            <button type="button" class="test-btn" data-port="1" data-mode="4">CHASE</button>
          </div>
        </div>
      </article>
      <article class="port-card" id="port-3">
        <h3>OUTPUT PORT 3</h3><div class="status-badge s-standby" id="p3-badge">STANDBY</div>
        <div class="universe-block"><div class="micro-label">UNIVERSE <span class="quality" id="p3-quality">NO SOURCE</span></div><div class="universe" id="p3-universe">---</div></div>
        <div class="channel-head"><span>CHANNEL ACTIVITY</span><b><span id="p3-used">0</span> / 512</b></div><div class="channel-track"><div class="channel-fill" id="p3-fill"></div></div>
        <div class="data-row"><span>Input FPS</span><b id="p3-ifps">0.0 Hz</b></div>
        <div class="data-row"><span>DMX Output</span><b id="p3-ofps">0.0 Hz</b></div>
        <div class="data-row"><span>Protocol</span><b id="p3-proto">-</b></div>
        <div class="data-row"><span>Sender IP</span><b id="p3-sender">-</b></div>
        <div class="data-row"><span>Last Packet</span><b id="p3-age">-</b></div>
        <div class="data-row"><span>sACN Priority</span><b id="p3-prio">-</b></div>
        <div class="conflict-box" id="p3-conflict">SOURCE CONFLICT</div>
        <div class="test-zone"><div class="test-zone-head"><span>OUTPUT TEST</span><span class="test-mode-name" id="p3-test-name">NETWORK</span></div>
          <div class="test-grid">
            <button type="button" class="test-btn" data-port="2" data-mode="0">STOP</button>
            <button type="button" class="test-btn" data-port="2" data-mode="1">0%</button>
            <button type="button" class="test-btn" data-port="2" data-mode="2">50%</button>
            <button type="button" class="test-btn" data-port="2" data-mode="3">100%</button>
            <button type="button" class="test-btn" data-port="2" data-mode="4">CHASE</button>
          </div>
        </div>
      </article>
      <article class="port-card" id="port-4">
        <h3>OUTPUT PORT 4</h3><div class="status-badge s-standby" id="p4-badge">STANDBY</div>
        <div class="universe-block"><div class="micro-label">UNIVERSE <span class="quality" id="p4-quality">NO SOURCE</span></div><div class="universe" id="p4-universe">---</div></div>
        <div class="channel-head"><span>CHANNEL ACTIVITY</span><b><span id="p4-used">0</span> / 512</b></div><div class="channel-track"><div class="channel-fill" id="p4-fill"></div></div>
        <div class="data-row"><span>Input FPS</span><b id="p4-ifps">0.0 Hz</b></div>
        <div class="data-row"><span>DMX Output</span><b id="p4-ofps">0.0 Hz</b></div>
        <div class="data-row"><span>Protocol</span><b id="p4-proto">-</b></div>
        <div class="data-row"><span>Sender IP</span><b id="p4-sender">-</b></div>
        <div class="data-row"><span>Last Packet</span><b id="p4-age">-</b></div>
        <div class="data-row"><span>sACN Priority</span><b id="p4-prio">-</b></div>
        <div class="conflict-box" id="p4-conflict">SOURCE CONFLICT</div>
        <div class="test-zone"><div class="test-zone-head"><span>OUTPUT TEST</span><span class="test-mode-name" id="p4-test-name">NETWORK</span></div>
          <div class="test-grid">
            <button type="button" class="test-btn" data-port="3" data-mode="0">STOP</button>
            <button type="button" class="test-btn" data-port="3" data-mode="1">0%</button>
            <button type="button" class="test-btn" data-port="3" data-mode="2">50%</button>
            <button type="button" class="test-btn" data-port="3" data-mode="3">100%</button>
            <button type="button" class="test-btn" data-port="3" data-mode="4">CHASE</button>
          </div>
        </div>
      </article>
    </div>
  </section>

  <details class="panel" id="config-panel">
    <summary>Node Routing &amp; Configuration</summary>
    <div class="config-content">
      <form id="config-form">
        <div class="mode-row">
          <label><input type="radio" name="routing" id="route-auto" value="0"> Auto · Sequential universes</label>
          <label><input type="radio" name="routing" id="route-manual" value="1"> Manual · Per-port universes</label>
        </div>
        <div class="config-row" id="auto-fields">
          <div class="field"><label class="field-label" for="start-universe">Start Universe</label><input id="start-universe" type="number" min="1" max="63996" value="1"></div>
          <div class="field"><label class="field-label" for="merge-mode">Conflict Merge Policy</label><select id="merge-mode"><option value="0">LTP · Latest source</option><option value="1">HTP · Highest value</option><option value="2">BLOCK · First source</option></select></div>
        </div>
        <div class="config-grid" id="manual-fields">
          <div class="config-line"><strong>PORT 1</strong><input id="cfg-u0" aria-label="Port 1 universe" type="number" min="1" max="63999"><select id="cfg-e0" aria-label="Port 1 enable"><option value="1">ON</option><option value="0">OFF</option></select><select id="cfg-p0" aria-label="Port 1 protocol"><option value="0">AUTO</option><option value="1">ART-NET</option><option value="2">sACN</option></select></div>
          <div class="config-line"><strong>PORT 2</strong><input id="cfg-u1" aria-label="Port 2 universe" type="number" min="1" max="63999"><select id="cfg-e1" aria-label="Port 2 enable"><option value="1">ON</option><option value="0">OFF</option></select><select id="cfg-p1" aria-label="Port 2 protocol"><option value="0">AUTO</option><option value="1">ART-NET</option><option value="2">sACN</option></select></div>
          <div class="config-line"><strong>PORT 3</strong><input id="cfg-u2" aria-label="Port 3 universe" type="number" min="1" max="63999"><select id="cfg-e2" aria-label="Port 3 enable"><option value="1">ON</option><option value="0">OFF</option></select><select id="cfg-p2" aria-label="Port 3 protocol"><option value="0">AUTO</option><option value="1">ART-NET</option><option value="2">sACN</option></select></div>
          <div class="config-line"><strong>PORT 4</strong><input id="cfg-u3" aria-label="Port 4 universe" type="number" min="1" max="63999"><select id="cfg-e3" aria-label="Port 4 enable"><option value="1">ON</option><option value="0">OFF</option></select><select id="cfg-p3" aria-label="Port 4 protocol"><option value="0">AUTO</option><option value="1">ART-NET</option><option value="2">sACN</option></select></div>
        </div>
        <div class="actions"><span class="config-hint">Saving routing briefly resets network source state; DMX output resumes immediately.</span><button class="primary-btn" id="save-config" type="submit">SAVE CONFIGURATION</button></div>
      </form>
    </div>
  </details>

  <section class="panel">
    <div class="section-head"><div><h2>Core Diagnostics</h2><p class="section-note">Operational counters for network and DMX tasks.</p></div></div>
    <div class="diag-grid">
      <div class="diag-box"><span>CORE 0 · NETWORK / WEB</span><b id="d-core0">-- loops/s</b><div class="diag-bar"><i id="d-core0-bar"></i></div></div>
      <div class="diag-box"><span>CORE 1 · DMX ENGINE</span><b id="d-core1">-- loops/s</b><div class="diag-bar"><i id="d-core1-bar"></i></div></div>
      <div class="diag-box"><span>MAX NETWORK SERVICE TIME</span><b id="d-net">-- µs</b><div class="diag-bar"><i id="d-net-bar"></i></div></div>
    </div>
  </section>

  <section class="panel">
    <div class="section-head"><div><h2>Recent Events</h2><p class="section-note">Fixed-size ten-entry log stored in RAM.</p></div></div>
    <div class="event-list" id="event-list"><div class="empty">No events received yet.</div></div>
  </section>

  <section class="panel">
    <div class="section-head"><div><h2>Device Information</h2><p class="section-note">Firmware and hardware identity.</p></div></div>
    <div class="device-grid">
      <div class="data-row"><span>Model</span><b id="i-model">ND DMX NODE 4U</b></div>
      <div class="data-row"><span>Firmware</span><b id="i-firmware">--</b></div>
      <div class="data-row"><span>Hardware</span><b id="i-hardware">--</b></div>
      <div class="data-row"><span>MAC Address</span><b id="i-mac">--</b></div>
      <div class="data-row"><span>Build</span><b id="i-build">--</b></div>
      <div class="data-row"><span>Dashboard</span><b>LOCAL · NO EXTERNAL ASSETS</b></div>
    </div>
  </section>
  <div class="footer">ND DMX SYSTEM · LOCAL CONTROL INTERFACE</div>
</div>
<div class="toast" id="toast"></div>
<script>
'use strict';
const $=id=>document.getElementById(id);
const mergeNames=['LTP','HTP','BLOCK'];
const testNames=['NETWORK','BLACKOUT','50%','FULL','CHASE'];
let configLoaded=false,lastPackets=null,lastPollTime=0,pollBusy=false,toastTimer=0;

function n(v,f=0){const x=Number(v);return Number.isFinite(x)?x:f}
function text(id,value){const e=$(id);if(e)e.textContent=value}
function fmtUptime(seconds){seconds=Math.max(0,Math.floor(n(seconds)));const d=Math.floor(seconds/86400);const h=Math.floor(seconds%86400/3600);const m=Math.floor(seconds%3600/60);const s=seconds%60;return (d?d+'d ':'')+String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0')}
function fmtAge(ms){ms=n(ms);if(ms<1)return '<1 ms';if(ms<1000)return Math.round(ms)+' ms';return (ms/1000).toFixed(1)+' s'}
function fmtEvent(ms){let s=Math.floor(n(ms)/1000),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s%60).padStart(2,'0')}
function showToast(message,isError=false){const t=$('toast');t.textContent=message;t.className='toast show'+(isError?' error':'');clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.className='toast',2600)}
function setConnection(ok){$('online-pill').classList.toggle('offline',!ok);text('online-text',ok?'SYSTEM ONLINE':'DASHBOARD OFFLINE');$('offline-banner').classList.toggle('show',!ok)}
function qualityFor(p){if(!p.en)return {name:'DISABLED',badge:'DISABLED',cls:'s-disabled'};if(n(p.test)>0)return {name:'LOCAL OVERRIDE',badge:'TEST MODE',cls:'s-test'};if(p.conflict)return {name:'SOURCE CONFLICT',badge:'CONFLICT',cls:'s-conflict'};if(!p.active)return {name:'NO SOURCE',badge:'STANDBY',cls:'s-standby'};if(n(p.age)>250||n(p.fps)<20)return {name:'DEGRADED',badge:'DEGRADED',cls:'s-degraded'};return {name:'STABLE',badge:'ACTIVE',cls:'s-active'}}

function renderPort(p){
  const id=n(p.port),card=$('port-'+id),q=qualityFor(p),test=n(p.test),enabled=!!p.en;
  if(!card)return;
  card.classList.toggle('override',test>0);card.classList.toggle('conflict',!!p.conflict);
  const badge=$('p'+id+'-badge');badge.className='status-badge '+q.cls;badge.textContent=q.badge;
  text('p'+id+'-quality',q.name);text('p'+id+'-universe',String(n(p.universe)).padStart(3,'0'));
  text('p'+id+'-used',n(p.used));$('p'+id+'-fill').style.width=Math.min(100,n(p.used)/512*100).toFixed(1)+'%';
  text('p'+id+'-ifps',n(p.fps).toFixed(1)+' Hz');text('p'+id+'-ofps',n(p.dmxFps).toFixed(1)+' Hz');
  text('p'+id+'-proto',p.proto||'-');text('p'+id+'-sender',p.active?(p.sender||'-'):'-');
  text('p'+id+'-age',p.active?fmtAge(p.age):'-');text('p'+id+'-prio',p.proto==='sACN'?n(p.prio):'-');
  const conflict=$('p'+id+'-conflict');conflict.classList.toggle('show',!!p.conflict);conflict.textContent='IP CONFLICT · '+(p.c_ip||'UNKNOWN');
  text('p'+id+'-test-name',testNames[test]||'NETWORK');
  card.querySelectorAll('.test-btn').forEach(btn=>{btn.classList.toggle('selected',n(btn.dataset.mode)===test);btn.disabled=!enabled&&n(btn.dataset.mode)!==0});
}

function renderEvents(events){const list=$('event-list');list.textContent='';if(!Array.isArray(events)||events.length===0){const e=document.createElement('div');e.className='empty';e.textContent='No events recorded.';list.appendChild(e);return}events.slice(0,10).forEach(ev=>{const row=document.createElement('div'),time=document.createElement('div'),msg=document.createElement('div');row.className='event-item';time.className='event-time';msg.className='event-message';time.textContent=fmtEvent(ev.t);msg.textContent=ev.m||'';row.append(time,msg);list.appendChild(row)})}

function updateRoutingVisibility(){const manual=$('route-manual').checked;$('manual-fields').style.display=manual?'grid':'none';$('start-universe').disabled=manual}
function populateConfig(j,force=false){if(configLoaded&&!force)return;$('route-auto').checked=n(j.mode)===0;$('route-manual').checked=n(j.mode)===1;$('start-universe').value=n(j.start,1);$('merge-mode').value=n(j.merge);if(Array.isArray(j.ports))j.ports.forEach((p,i)=>{$('cfg-u'+i).value=n(p.universe,i+1);$('cfg-e'+i).value=p.en?1:0;$('cfg-p'+i).value=n(p.pmode)});updateRoutingVisibility();configLoaded=true}

function renderStatus(j){
  const now=Date.now(),dt=lastPollTime?(now-lastPollTime)/1000:0,packets=n(j.packets),pps=lastPackets===null||dt<=0?0:Math.max(0,(packets-lastPackets)/dt);lastPackets=packets;lastPollTime=now;
  text('m-ip',j.ip||'--');text('m-uptime',fmtUptime(j.uptime));text('m-packets',packets.toLocaleString());text('m-pps',pps.toFixed(1));text('m-link',j.link?'CONNECTED':'DISCONNECTED');
  const tempText=(typeof j.temp==='number'&&Number.isFinite(j.temp))?j.temp.toFixed(1)+' °C':'SENSOR ERROR';
  text('m-temp',tempText);
  const ports=Array.isArray(j.ports)?j.ports:[],active=ports.filter(p=>p.active).length,protocols=[...new Set(ports.filter(p=>p.active).map(p=>p.proto).filter(v=>v&&v!=='-'))];
  text('m-active',active+' / 4');text('m-protocol',protocols.length?protocols.join(' + '):'STANDBY');text('m-merge',mergeNames[n(j.merge)]||'LTP');ports.forEach(renderPort);
  text('d-core0',n(j.c0).toLocaleString()+' loops/s');text('d-core1',n(j.c1).toLocaleString()+' loops/s');text('d-net',n(j.net).toLocaleString()+' µs');
  $('d-core0-bar').style.width=Math.min(100,n(j.c0)/6000*100)+'%';$('d-core1-bar').style.width=Math.min(100,n(j.c1)/150000*100)+'%';$('d-net-bar').style.width=Math.min(100,n(j.net)/3000*100)+'%';
  text('i-model',j.model||'ND DMX NODE 4U');text('i-firmware',j.firmware||'--');text('i-hardware',j.hardware||'--');text('i-mac',j.mac||'--');text('i-build',j.build||'--');
  renderEvents(j.events);populateConfig(j);text('last-update','UPDATED '+new Date().toLocaleTimeString());setConnection(true)
}

async function refreshStatus(){if(pollBusy)return;pollBusy=true;try{const r=await fetch('/api/status?_='+Date.now(),{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);renderStatus(await r.json())}catch(err){setConnection(false);text('last-update','NO RESPONSE');console.error('Dashboard status error:',err)}finally{pollBusy=false}}

async function setTest(port,mode){if(mode!==0&&!confirm('Local test will override network DMX on Port '+(port+1)+'. Continue?'))return;try{const r=await fetch('/api/test?p='+port+'&m='+mode,{cache:'no-store'}),j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error||'Test command failed');showToast('Port '+(port+1)+' · '+testNames[mode]);await refreshStatus()}catch(err){showToast(err.message,true)}}

document.querySelectorAll('.test-btn').forEach(btn=>btn.addEventListener('click',()=>setTest(n(btn.dataset.port),n(btn.dataset.mode))));
$('route-auto').addEventListener('change',updateRoutingVisibility);$('route-manual').addEventListener('change',updateRoutingVisibility);
$('config-form').addEventListener('submit',async ev=>{ev.preventDefault();const button=$('save-config');button.disabled=true;const q=new URLSearchParams({mode:$('route-manual').checked?'1':'0',start:$('start-universe').value,merge:$('merge-mode').value});for(let i=0;i<4;i++){q.set('u'+i,$('cfg-u'+i).value);q.set('e'+i,$('cfg-e'+i).value);q.set('p'+i,$('cfg-p'+i).value)}try{const r=await fetch('/api/config?'+q.toString(),{cache:'no-store'}),j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error||'Configuration rejected');configLoaded=false;showToast('Configuration saved');await refreshStatus()}catch(err){showToast(err.message,true)}finally{button.disabled=false}});

refreshStatus();setInterval(refreshStatus,1000);
</script>
</body>
</html>
)NDWEB";

// W5500/EthernetClient may accept only part of a large write. This routine
// retries partial writes in small blocks and services UDP between blocks, so
// opening the dashboard does not starve Art-Net/sACN reception on Core 0.
bool writeAll(EthernetClient& client, const uint8_t* data, size_t length) {
  const size_t CHUNK_SIZE = 512;
  size_t sent = 0;
  uint32_t lastProgressMs = millis();

  while (sent < length && client.connected()) {
    const size_t remaining = length - sent;
    const size_t block = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
    const size_t written = client.write(data + sent, block);

    if (written > 0) {
      sent += written;
      lastProgressMs = millis();

      // Keep network DMX responsive while a browser downloads the page.
      handleNetworkPackets();
      watchdog_update();
    } else {
      if (millis() - lastProgressMs > 1500) break;
      handleNetworkPackets();
      watchdog_update();
      delay(1);
    }
  }
  return sent == length;
}

bool writeAll(EthernetClient& client, const char* data, size_t length) {
  return writeAll(client, reinterpret_cast<const uint8_t*>(data), length);
}

void sendStaticHeader(EthernetClient& client, const char* status, const char* contentType, size_t contentLength, const char* cacheControl) {
  char header[320];
  const int count = snprintf(
    header, sizeof(header),
    "HTTP/1.1 %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %lu\r\n"
    "Cache-Control: %s\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n\r\n",
    status, contentType, static_cast<unsigned long>(contentLength), cacheControl
  );
  if (count > 0) writeAll(client, header, static_cast<size_t>(count));
}

void sendDynamicJsonHeader(EthernetClient& client, bool ok = true) {
  client.println(ok ? F("HTTP/1.1 200 OK") : F("HTTP/1.1 400 Bad Request"));
  client.println(F("Content-Type: application/json; charset=utf-8"));
  client.println(F("Cache-Control: no-store, no-cache, must-revalidate"));
  client.println(F("X-Content-Type-Options: nosniff"));
  client.println(F("Connection: close"));
  client.println();
}

void printJsonString(EthernetClient& client, const char* value) {
  client.print('"');
  if (value) {
    for (const char* p = value; *p; ++p) {
      const char ch = *p;
      if (ch == '"' || ch == '\\') {
        client.print('\\');
        client.print(ch);
      } else if (ch == '\n') {
        client.print(F("\\n"));
      } else if (static_cast<uint8_t>(ch) >= 0x20) {
        client.print(ch);
      }
    }
  }
  client.print('"');
}

void printMacString(EthernetClient& client) {
  client.print('"');
  for (int i = 0; i < 6; ++i) {
    if (i) client.print(':');
    if (mac[i] < 16) client.print('0');
    client.print(mac[i], HEX);
  }
  client.print('"');
}

void sendStatusJson(EthernetClient& client) {
  sendDynamicJsonHeader(client, true);
  const uint32_t now = millis();

  client.print(F("{\"model\":")); printJsonString(client, DEVICE_MODEL);
  client.print(F(",\"firmware\":")); printJsonString(client, FIRMWARE_VERSION);
  client.print(F(",\"hardware\":")); printJsonString(client, HARDWARE_REVISION);
  client.print(F(",\"build\":"));
  client.print('"'); client.print(__DATE__); client.print(' '); client.print(__TIME__); client.print('"');
  client.print(F(",\"mac\":")); printMacString(client);
  client.print(F(",\"ip\":"));
  client.print('"'); printIP(client, Ethernet.localIP()); client.print('"');
  client.print(F(",\"link\":")); client.print(Ethernet.linkStatus() == LinkON ? F("true") : F("false"));
  client.print(F(",\"mode\":")); client.print(cfg.manualMode);
  client.print(F(",\"start\":")); client.print(cfg.startUniverse);
  client.print(F(",\"merge\":")); client.print(cfg.mergeMode);
  client.print(F(",\"uptime\":")); client.print(now / 1000);
  client.print(F(",\"c0\":")); client.print(sysLog.core0Loops);
  client.print(F(",\"c1\":")); client.print(sysLog.core1Loops);
  client.print(F(",\"net\":")); client.print(sysLog.maxNetTimeUs);
  client.print(F(",\"packets\":")); client.print(sysLog.totalPacketsReceived);
  client.print(F(",\"temp\":"));
  if (isfinite(sysLog.mcuTemperatureC)) {
    client.print(sysLog.mcuTemperatureC, 1);
  } else {
    // JSON has no NaN value. Use null when the RP2040 sensor is unavailable.
    client.print(F("null"));
  }
  client.print(F(",\"ports\":["));

  for (int i = 0; i < 4; ++i) {
    if (i) client.print(',');
    client.print(F("{\"port\":")); client.print(i + 1);
    client.print(F(",\"en\":")); client.print(cfg.portEnabled[i] ? 1 : 0);
    client.print(F(",\"pmode\":")); client.print(cfg.portProtocol[i]);
    client.print(F(",\"universe\":")); client.print(ports[i].universe);
    client.print(F(",\"active\":")); client.print(ports[i].active ? F("true") : F("false"));
    client.print(F(",\"conflict\":")); client.print(ports[i].conflict ? F("true") : F("false"));
    client.print(F(",\"fps\":")); client.print(ports[i].fps, 1);
    client.print(F(",\"dmxFps\":")); client.print(ports[i].dmxFps, 1);
    client.print(F(",\"used\":")); client.print(ports[i].usedChannels);
    client.print(F(",\"age\":")); client.print(ports[i].active ? now - ports[i].lastPacketMs : 0);
    client.print(F(",\"prio\":")); client.print(ports[i].currentPriority);
    client.print(F(",\"test\":")); client.print(ports[i].testMode);
    client.print(F(",\"proto\":")); printJsonString(client, ports[i].protocol);
    client.print(F(",\"sender\":")); client.print('"'); printIP(client, ports[i].sender); client.print('"');
    client.print(F(",\"c_ip\":")); client.print('"'); printIP(client, ports[i].conflictIP); client.print('"');
    client.print('}');
  }

  client.print(F("],\"events\":["));
  for (uint8_t n = 0; n < eventLogCount; ++n) {
    if (n) client.print(',');
    const int index = (eventLogHead + EVENT_LOG_SIZE - 1 - n) % EVENT_LOG_SIZE;
    client.print(F("{\"t\":")); client.print(eventLog[index].timestampMs);
    client.print(F(",\"m\":")); printJsonString(client, eventLog[index].message);
    client.print('}');
  }
  client.print(F("]}"));
}

void sendJsonResult(EthernetClient& client, bool ok, const char* message = nullptr) {
  sendDynamicJsonHeader(client, ok);
  client.print(F("{\"ok\":")); client.print(ok ? F("true") : F("false"));
  if (message) {
    client.print(ok ? F(",\"message\":") : F(",\"error\":"));
    printJsonString(client, message);
  }
  client.print('}');
}

void sendTestResponse(EthernetClient& client, const char* requestLine) {
  const int port = getParamValue(requestLine, "p");
  const int mode = getParamValue(requestLine, "m");

  if (port < 0 || port >= 4 || mode < TEST_NETWORK || mode > TEST_CHASE) {
    sendJsonResult(client, false, "Invalid port or test mode");
    return;
  }
  if (mode != TEST_NETWORK && !cfg.portEnabled[port]) {
    sendJsonResult(client, false, "Enable the port before starting a test");
    return;
  }

  const uint32_t irq = spin_lock_blocking(dmx_lock);
  ports[port].testMode = static_cast<uint8_t>(mode);
  ports[port].chaseChannel = 1;
  ports[port].lastChaseStepMs = 0;
  spin_unlock(dmx_lock, irq);

  char detail[50];
  snprintf(detail, sizeof(detail), "test mode %s", testModeName(static_cast<uint8_t>(mode)));
  addPortEvent(port, detail);
  sendJsonResult(client, true, "Test mode updated");
}

bool applyConfigFromRequest(const char* requestLine, char* error, size_t errorSize) {
  const int mode = getParamValue(requestLine, "mode");
  const int start = getParamValue(requestLine, "start");
  const int merge = getParamValue(requestLine, "merge");

  if (mode < 0 || mode > 1) {
    snprintf(error, errorSize, "Routing mode must be 0 or 1");
    return false;
  }
  if (start < 1 || start > 63996) {
    snprintf(error, errorSize, "Start universe must be 1 to 63996");
    return false;
  }
  if (merge < 0 || merge > 2) {
    snprintf(error, errorSize, "Merge mode must be 0 to 2");
    return false;
  }

  Config next = cfg;
  next.manualMode = static_cast<uint8_t>(mode);
  next.startUniverse = static_cast<uint16_t>(start);
  next.mergeMode = static_cast<uint8_t>(merge);

  for (int i = 0; i < 4; ++i) {
    char key[4];
    snprintf(key, sizeof(key), "u%d", i);
    const int universe = getParamValue(requestLine, key);
    snprintf(key, sizeof(key), "e%d", i);
    const int enabled = getParamValue(requestLine, key);
    snprintf(key, sizeof(key), "p%d", i);
    const int protocol = getParamValue(requestLine, key);

    if (universe < 1 || universe > 63999 || enabled < 0 || enabled > 1 || protocol < 0 || protocol > 2) {
      snprintf(error, errorSize, "Invalid configuration for Port %d", i + 1);
      return false;
    }
    next.portUniverses[i] = static_cast<uint16_t>(universe);
    next.portEnabled[i] = static_cast<uint8_t>(enabled);
    next.portProtocol[i] = static_cast<uint8_t>(protocol);
  }

  cfg = next;
  saveConfig();
  updateUniverseMap();
  addEvent("Configuration saved");
  return true;
}

void sendConfigResponse(EthernetClient& client, const char* requestLine) {
  char error[72] = {0};
  const bool ok = applyConfigFromRequest(requestLine, error, sizeof(error));
  sendJsonResult(client, ok, ok ? "Configuration saved" : error);
}

void sendDashboardPage(EthernetClient& client) {
  const size_t pageLength = sizeof(INDEX_HTML) - 1;
  sendStaticHeader(client, "200 OK", "text/html; charset=utf-8", pageLength, "no-store, no-cache, must-revalidate");
  writeAll(client, INDEX_HTML, pageLength);
}

bool readRequestLine(EthernetClient& client, char* output, size_t capacity) {
  if (!output || capacity < 2) return false;
  size_t index = 0;
  const uint32_t started = millis();

  while (client.connected() && millis() - started < 300) {
    while (client.available()) {
      const char ch = static_cast<char>(client.read());
      if (ch == '\n') {
        output[index] = '\0';
        return index > 0;
      }
      if (ch != '\r' && index < capacity - 1) output[index++] = ch;
    }
    watchdog_update();
    delay(1);
  }
  output[index] = '\0';
  return index > 0;
}

void handleWeb() {
  EthernetClient client = server.available();
  if (!client) return;

  char requestLine[512] = {0};
  if (!readRequestLine(client, requestLine, sizeof(requestLine))) {
    client.stop();
    return;
  }

  if (strncmp(requestLine, "GET /api/status", 15) == 0) {
    sendStatusJson(client);
  } else if (strncmp(requestLine, "GET /api/test?", 14) == 0) {
    sendTestResponse(client, requestLine);
  } else if (strncmp(requestLine, "GET /api/config?", 16) == 0) {
    sendConfigResponse(client, requestLine);
  } else if (strncmp(requestLine, "GET /set?", 9) == 0) {
    char error[72] = {0};
    const bool ok = applyConfigFromRequest(requestLine, error, sizeof(error));
    if (ok) {
      client.println(F("HTTP/1.1 303 See Other"));
      client.println(F("Location: /"));
      client.println(F("Connection: close"));
      client.println();
    } else {
      sendJsonResult(client, false, error);
    }
  } else if (strncmp(requestLine, "GET /favicon.ico", 16) == 0) {
    client.println(F("HTTP/1.1 204 No Content"));
    client.println(F("Connection: close"));
    client.println();
  } else if (strncmp(requestLine, "GET / ", 6) == 0 || strncmp(requestLine, "GET /index.html", 15) == 0) {
    sendDashboardPage(client);
  } else {
    static const char NOT_FOUND[] = "Not found";
    sendStaticHeader(client, "404 Not Found", "text/plain; charset=utf-8", sizeof(NOT_FOUND) - 1, "no-store");
    writeAll(client, NOT_FOUND, sizeof(NOT_FOUND) - 1);
  }

  delay(2);
  client.stop();
}


void setup() {
  // USB CDC is provided by the Arduino-Pico USB stack.
  // Do not wait forever for Serial: the node must also boot without a computer attached.
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("[BOOT] ND DMX NODE 4U v2.0.0"));
  Serial.println(F("[BOOT] USB CDC started"));

  pinMode(HARDWARE_LED_PIN, OUTPUT);
  digitalWrite(HARDWARE_LED_PIN, LOW);

  sysLog.mcuTemperatureC = readMcuTemperatureC();
  Serial.println(F("[BOOT] ADC ready"));

  // EEPROM depends on the flash size selected in Arduino IDE.
  // For a standard Raspberry Pi Pico, select the real 2MB flash layout.
  loadConfig();
  Serial.println(F("[BOOT] Configuration loaded"));

  // Initialize all DMX PIO state machines before releasing Core 1.
  for (int i = 0; i < 4; i++) {
    dmxOutputs[i].begin(i < 2 ? pio0 : pio1, i % 2, RS485_PINS[i]);
  }
  Serial.println(F("[BOOT] DMX PIO ready"));

  int lock_num = spin_lock_claim_unused(true);
  dmx_lock = spin_lock_init(lock_num);
  Serial.println(F("[BOOT] Spinlock ready"));

  // Initialize the W5500 BEFORE opening UDP sockets.
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(100);
  digitalWrite(PIN_RST, HIGH);
  delay(500);

  SPI.setRX(PIN_MISO);
  SPI.setCS(PIN_CS);
  SPI.setSCK(PIN_SCK);
  SPI.setTX(PIN_MOSI);
  SPI.begin();
  Ethernet.init(PIN_CS);
  Serial.println(F("[BOOT] SPI/W5500 interface ready"));

  IPAddress staticIP(10, 10, 10, 10);
  IPAddress dnsServer(10, 10, 10, 1);
  IPAddress gateway(10, 10, 10, 1);
  IPAddress subnetMask(255, 255, 255, 0);
  Ethernet.begin(mac, staticIP, dnsServer, gateway, subnetMask);

  lastLinkStatus = Ethernet.linkStatus();

  // updateUniverseMap() also opens the sACN sockets, so it must run only
  // after SPI and Ethernet have been initialized.
  updateUniverseMap();

  server.begin();
  artnetUdp.begin(ARTNET_PORT);

  addEvent("System boot complete");
  addEvent(lastLinkStatus == LinkON ? "Ethernet link connected" : "Ethernet link unavailable");

  Serial.print(F("[BOOT] IP: "));
  Serial.println(Ethernet.localIP());
  Serial.println(F("[BOOT] Network services ready"));

  watchdog_enable(8388, 1);

  asm volatile("dmb" : : : "memory");
  systemReady = true;
  asm volatile("sev");

  Serial.println(F("[BOOT] Core 1 released"));
  Serial.println(F("[BOOT] Startup complete"));
}

void loop() {
  watchdog_update();
  
  // Synchronized security protects both Core 1 and Core 1 from Watchdog inter-core
  static uint32_t lastWDCheck = 0;
  static uint32_t last_hb = 0;
  if (millis() - lastWDCheck >= 1000) {
    uint32_t current_hb = core1_heartbeat;
    if (current_hb != last_hb) {
      last_hb = current_hb; // Core 1 is working normally
      watchdog_update();   // Maintain a safe Watchdog pulse rate
    }
    lastWDCheck = millis();
  }
  
  static uint32_t lastLinkCheck = 0;
  if (millis() - lastLinkCheck >= 1000) {
    auto currentLink = Ethernet.linkStatus();
    if (lastLinkStatus == LinkOFF && currentLink == LinkON) {
      artnetUdp.stop(); setupSacnSockets(); artnetUdp.begin(ARTNET_PORT);
      addEvent("Ethernet link restored");
    } else if (lastLinkStatus == LinkON && currentLink == LinkOFF) {
      addEvent("Ethernet link lost");
    }
    lastLinkStatus = currentLink; lastLinkCheck = millis();
  }

  uint32_t t_start = micros();
  handleNetworkPackets(); 
  uint32_t t_duration = micros() - t_start;
  if (t_duration > maxNetTime) maxNetTime = t_duration;

  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    if (ports[i].active && (now - ports[i].lastPacketMs > 1000)) {
      ports[i].active = false; strcpy(ports[i].protocol, "-"); ports[i].fps = 0; ports[i].usedChannels = 0; ports[i].currentPriority = 0;
      addPortEvent(i, "network source lost");
    }
    // Automatically remove the Conflict flag after 2 seconds if IP duplication stops
    if (ports[i].conflict && (now - ports[i].lastConflictMs > 2000)) {
      ports[i].conflict = false;
      addPortEvent(i, "source conflict cleared");
    }
    if (now - ports[i].lastFpsMs >= 1000) {
      ports[i].fps = (ports[i].frameCount * 1000.0f) / (now - ports[i].lastFpsMs);
      ports[i].frameCount = 0; ports[i].lastFpsMs = now;
    }
    if (now - ports[i].lastDmxFpsMs >= 1000) {
      uint32_t outputFrames = __atomic_exchange_n(&ports[i].dmxFrameCount, 0u, __ATOMIC_RELAXED);
      ports[i].dmxFps = (outputFrames * 1000.0f) / (now - ports[i].lastDmxFpsMs);
      ports[i].lastDmxFpsMs = now;
    }
  }
  
  handleWeb();          

  lCount0++;
  static uint32_t lastReportMs = 0;
  if (millis() - lastReportMs >= 1000) {
    sysLog.core0Loops = lCount0; sysLog.core1Loops = lCount1; sysLog.maxNetTimeUs = maxNetTime;
    sysLog.mcuTemperatureC = readMcuTemperatureC();
    lCount0 = 0; lCount1 = 0; maxNetTime = 0; lastReportMs = millis();
  }

  static uint32_t lastLED = 0;
  if (millis() - lastLED > 300) { digitalWrite(HARDWARE_LED_PIN, !digitalRead(HARDWARE_LED_PIN)); lastLED = millis(); }
}

// ================= CORE 1: INDEPENDENT DMX SIGNAL OUTPUT =================
void setup1() {
  // It's completely empty because the hardware has been safely initialized by Core 0 in setup()
}

// DMX statefuls transmission system is non-blocking and multi-thread safe
void handleDmxTransmit(int i) {
  uint32_t nowMs = millis();
  uint32_t nowUs = micros();
  uint pin = RS485_PINS[i];
  PIO pio = (i < 2) ? pio0 : pio1;
  uint sm = i % 2;
  
  switch (ports[i].state) {
    case STATE_IDLE:
      {
        uint32_t irq_status = spin_lock_blocking(dmx_lock);
        int32_t time_remaining = (int32_t)(ports[i].nextFrameTimeUs - nowUs);
        
        // PULSE DROP FIX: Continuous DMX 40Hz playback (every 25ms) never interrupts the stream, even when the scene is still
        if (time_remaining <= 0) {
          // In test mode, only the front-buffer is replaced; network data continues to update in the back-buffer
          uint8_t testMode = ports[i].testMode;
          if (testMode == TEST_BLACKOUT) {
            memset(ports[i].activeDmxBuffer, 0, 513);
          } else if (testMode == TEST_HALF) {
            memset(ports[i].activeDmxBuffer + 1, 128, 512);
            ports[i].activeDmxBuffer[0] = 0;
          } else if (testMode == TEST_FULL) {
            memset(ports[i].activeDmxBuffer + 1, 255, 512);
            ports[i].activeDmxBuffer[0] = 0;
          } else if (testMode == TEST_CHASE) {
            if (nowMs - ports[i].lastChaseStepMs >= 100) {
              ports[i].chaseChannel++;
              if (ports[i].chaseChannel > 512) ports[i].chaseChannel = 1;
              ports[i].lastChaseStepMs = nowMs;
            }
            memset(ports[i].activeDmxBuffer, 0, 513);
            ports[i].activeDmxBuffer[ports[i].chaseChannel] = 255;
          } else if (ports[i].hasNewData) {
            memcpy(ports[i].activeDmxBuffer, ports[i].dmxBuffer, 513);
            ports[i].hasNewData = false;
          }
          spin_unlock(dmx_lock, irq_status); // Unlock immediately after buffer processing
          
          // Count the 25 ms frame period from BREAK, not from FIFO-fill completion.
          ports[i].nextFrameTimeUs = nowUs + 25000;

          // 1. Switch the pin to SIO and pull LOW to create a physical BREAK pulse
          pinMode(pin, OUTPUT);
          digitalWrite(pin, LOW);
          
          ports[i].state = STATE_BREAK;
          ports[i].stateStartTimeUs = nowUs;
          __atomic_fetch_add(&ports[i].dmxFrameCount, 1u, __ATOMIC_RELAXED);
        } else {
          spin_unlock(dmx_lock, irq_status);
        }
      }
      break;
      
    case STATE_BREAK:
      // Minimum break time is 88us (Wait 105us for the signal to become extremely stable)
      if (nowUs - ports[i].stateStartTimeUs >= 105) {
        // 2. Pull the pin to HIGH to generate the Mark After Break (MAB) pulse.
        digitalWrite(pin, HIGH);
        
        ports[i].state = STATE_MAB;
        ports[i].stateStartTimeUs = nowUs;
      }
      break;
      
    case STATE_MAB:
      // Minimum MAB time is 8us (Wait 14us for safety)
      if (nowUs - ports[i].stateStartTimeUs >= 14) {
        
        // 3. Reset SM, return the GPIO to PIO and hold MARK HIGH at PULL BLOCK
        dmxOutputs[i].reset_sm();

        ports[i].state = STATE_SENDING;
        ports[i].txIndex = 0;
      }
      break;
      
    case STATE_SENDING:
      // Pushing data into a non-blocking TX FIFO is fair (Maximum limit of 16 bytes per scan)
      {
        int fed = 0;
        while (ports[i].txIndex < 513 && fed < 16) {
          if (!dmxOutputs[i].put_byte(ports[i].activeDmxBuffer[ports[i].txIndex])) {
            break; 
          }
          ports[i].txIndex++;
          fed++;
        }
      }
      
      // Complete loading the data of the entire DMX frame into the FIFO
      if (ports[i].txIndex >= 513) {
        // nextFrameTimeUs was set when BREAK began, producing a real ~40 Hz frame period.
        ports[i].state = STATE_IDLE;
      }
      break;
  }
}

void loop1() {
  // Wait for the Core 0 system to be ready to use the Pico SDK Memory Barrier
  if (!systemReady) {
    while (!systemReady) {
      asm volatile("wfe");
    }
    asm volatile("dmb" : : : "memory");
  }

  lCount1++; 
  core1_heartbeat++; // An increased heart rate signals a rhythmic response in Core 0
  
  for (int i = 0; i < 4; i++) {
    if (cfg.portEnabled[i] == 1) {
      handleDmxTransmit(i);
    }
  }
}