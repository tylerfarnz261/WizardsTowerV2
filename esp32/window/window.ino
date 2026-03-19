/*
  Wizards - Window ESP32 Controller
  ==================================
  
  This ESP32 controls a sprite player to display different video sequences 
  based on game state. It listens for MQTT messages and sends single-byte
  commands to the sprite player via serial communication.
  
  Video Sequences:
  - 000: Default/Normal realm (on init and after transitions)
  - 001: Shadow realm active
  - 002: Paradox rune activation sequence
  
  Hardware:
  - ESP32 development board  
  - Serial connection to sprite player (TX/RX pins)
  - 3.3V TTL serial communication at 9600 baud
  
  MQTT Topics Subscribed:
  - escaperoom/lighting/blacklights (shadow realm state)
  - escaperoom/sprite_players/paradox (paradox activation)
  
  Author: Wizards Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "window1_esp32";

// MQTT topics to subscribe to
const char* topic_blacklights = "escaperoom/lighting/blacklights";
const char* topic_paradox = "escaperoom/sprite_players/paradox";

// Hardware pins for sprite player communication
const int SERIAL_TX = 6;     // Serial TX to sprite player (D4 on XIAO ESP32 C3)
const int SERIAL_RX = 7;     // Serial RX from sprite player (D5 on XIAO ESP32 C3)

// Serial communication
HardwareSerial SpriteSerial(1);  // Use hardware serial port 1

// Video file commands (single bytes - NUMERICAL values, not characters)
const byte VIDEO_000_NORMAL = 0;     // Default/Normal realm
const byte VIDEO_001_SHADOW = 1;     // Shadow realm
const byte VIDEO_002_PARADOX = 2;    // Paradox activation

// Game state tracking
bool in_shadow_realm = false;
bool paradox_activated = false;
unsigned long paradox_start_time = 0;
const unsigned long PARADOX_DURATION = 10000; // 10 seconds for paradox video

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// Status LED (built-in)
bool led_blink_state = false;
unsigned long last_blink_time = 0;
const unsigned long BLINK_INTERVAL = 1000;  // 1 second

// Sprite player communication
unsigned long last_sprite_check = 0;
const unsigned long SPRITE_CHECK_INTERVAL = 100; // Check sprite status every 100ms

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Window ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize status LED
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Initialize sprite player serial communication
  SpriteSerial.begin(9600, SERIAL_8N1, SERIAL_RX, SERIAL_TX);
  delay(500);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  // Initialize with default video (000)
  play_video(VIDEO_000_NORMAL);
  Serial.println("Sent initial command for video 000 (Normal realm)");
  
  Serial.println("Initialization complete");
  Serial.println("Listening for shadow realm and paradox events...");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Handle status LED blinking
  handle_status_led();
  
  // Handle sprite player communication
  handle_sprite_communication();
  
  // Check if paradox sequence should end and return to normal
  handle_paradox_timing();
  
  // Publish heartbeat periodically
  publish_heartbeat();
  
  delay(10);  // Small delay for stability
}

void setup_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed!");
  }
}

void reconnect_mqtt() {
  while (!mqtt_client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    if (mqtt_client.connect(device_id)) {
      Serial.println(" connected");
      
      // Subscribe to relevant topics
      mqtt_client.subscribe(topic_blacklights);
      mqtt_client.subscribe(topic_paradox);
      
      Serial.println("Subscribed to blacklights and paradox topics");
      
      // Publish online status
      String status_topic = String("escaperoom/status/") + device_id;
      mqtt_client.publish(status_topic.c_str(), "online");
      
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  // Convert payload to string
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Received MQTT [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  // Handle blacklights topic (shadow realm state)
  if (strcmp(topic, topic_blacklights) == 0) {
    if (message.equalsIgnoreCase("true")) {
      // Entering shadow realm
      if (!in_shadow_realm && !paradox_activated) {
        in_shadow_realm = true;
        play_video(VIDEO_001_SHADOW);
        Serial.println("Entering shadow realm - switched to video 001");
      }
    } else if (message.equalsIgnoreCase("false")) {
      // Leaving shadow realm
      if (in_shadow_realm && !paradox_activated) {
        in_shadow_realm = false;
        play_video(VIDEO_000_NORMAL);
        Serial.println("Leaving shadow realm - switched back to video 000");
      }
    }
  }
  
  // Handle paradox activation
  else if (strcmp(topic, topic_paradox) == 0) {
    if (message.equalsIgnoreCase("activate")) {
      // Paradox rune activated - override any current state
      paradox_activated = true;
      paradox_start_time = millis();
      in_shadow_realm = false; // Force exit shadow realm state
      
      play_video(VIDEO_002_PARADOX);
      Serial.println("Paradox rune activated - switched to video 002");
    }
  }
}

void play_video(byte video_number) {
  Serial.print("Sending sprite command for video ");
  Serial.print(video_number, DEC);
  Serial.print(" (0x");
  Serial.print(video_number, HEX);
  Serial.println(")");
  
  // Send single byte command to sprite player
  SpriteSerial.write(video_number);
  
  // Brief delay to allow command processing
  delay(50);
}

void handle_sprite_communication() {
  // Check for sprite player responses periodically
  unsigned long current_time = millis();
  
  if (current_time - last_sprite_check >= SPRITE_CHECK_INTERVAL) {
    if (SpriteSerial.available()) {
      while (SpriteSerial.available()) {
        byte response = SpriteSerial.read();
        
        if (response == 0xEE) {
          Serial.println("Sprite player: End of file reached");
        } else {
          Serial.print("Sprite player status: Playing file ");
          Serial.print(response, DEC);
          Serial.print(" (0x");
          Serial.print(response, HEX);
          Serial.println(")");
        }
      }
    }
    last_sprite_check = current_time;
  }
}

void handle_paradox_timing() {
  // Check if paradox sequence should end and return to normal
  if (paradox_activated) {
    unsigned long elapsed = millis() - paradox_start_time;
    
    if (elapsed >= PARADOX_DURATION) {
      // Paradox sequence complete - return to normal
      paradox_activated = false;
      play_video(VIDEO_000_NORMAL);
      Serial.println("Paradox sequence complete - returned to video 000");
      
      // Note: We don't reset in_shadow_realm here because paradox 
      // disables shadow realm toggle permanently in the rune controller
    }
  }
}

void handle_status_led() {
  unsigned long current_time = millis();
  
  if (current_time - last_blink_time >= BLINK_INTERVAL) {
    led_blink_state = !led_blink_state;
    digitalWrite(LED_BUILTIN, led_blink_state);
    last_blink_time = current_time;
  }
}

void publish_heartbeat() {
  static unsigned long last_heartbeat = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_heartbeat >= 30000) {
    if (mqtt_client.connected()) {
      String heartbeat_topic = String("escaperoom/heartbeat/") + device_id;
      String heartbeat_data = String("{\"uptime\":") + (current_time / 1000) + 
                             ",\"free_heap\":" + ESP.getFreeHeap() + 
                             ",\"in_shadow_realm\":" + (in_shadow_realm ? "true" : "false") +
                             ",\"paradox_active\":" + (paradox_activated ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}