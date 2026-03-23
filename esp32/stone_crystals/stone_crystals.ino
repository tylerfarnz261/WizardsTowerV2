/*
  Escape Room - Stone Crystals ESP32 Controller
  ============================================
  
  This ESP32 controls the crystal placement puzzle with 5 colored crystals.
  Each crystal has a sensor to detect placement/removal.
  GREEN crystal is locked until RED, BLUE, PURPLE, and WHITE are placed.
  When first 4 crystals are placed, it unlocks paradox compartment.
  When all 5 crystals are placed, it triggers win sequence.
  
  Hardware:
  - ESP32 development board
  - 5 crystal presence sensors (hall sensors, optical, or mechanical switches)
  - Pull-up/pull-down resistors as needed
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/crystals/first_four_placed
  - Publishes: escaperoom/esp32/crystals/all_placed
  
  Crystal Colors: RED, GREEN, BLUE, PURPLE, WHITE
  Sequence: First 4 (RED, BLUE, PURPLE, WHITE) must be placed before GREEN
  
  Author: Escape Room Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "stone_crystals_esp32";

// Audio server settings
const char* audio_server_ip = "192.168.0.156";
const int audio_server_port = 15000;

// MQTT topics
const char* topic_first_four_placed = "escaperoom/esp32/crystals/first_four_placed";
const char* topic_all_placed = "escaperoom/esp32/crystals/all_placed";
const char* topic_win_condition = "escaperoom/gamestate/win";
const char* topic_reset = "escaperoom/system/reset";

// Hardware pins for crystal sensors
const int CRYSTAL_PINS[5] = {2, 4, 5, 18, 19};  // RED, GREEN, BLUE, PURPLE, WHITE
const int SWORD_PIN = 21;  // Sword input pin

// Crystal definitions
enum Crystal {
  RED = 0,
  GREEN = 1,
  BLUE = 2,
  PURPLE = 3,
  WHITE = 4
};

const char* CRYSTAL_NAMES[5] = {"RED", "GREEN", "BLUE", "PURPLE", "WHITE"};

// Crystal states
bool crystal_placed[5] = {false, false, false, false, false};
bool crystal_prev_states[5] = {false, false, false, false, false};
unsigned long crystal_debounce_times[5] = {0, 0, 0, 0, 0};
const unsigned long DEBOUNCE_DELAY = 100;  // Longer debounce for crystal sensors

// Game state
bool first_four_complete = false;
bool green_unlocked = false;
bool all_crystals_complete = false;
bool sequence_in_progress = false;
bool first_four_ever_completed = false;  // Track if fanfare has ever been played
bool sword_enabled = false;
bool sword_placed = false;
bool sword_prev_state = false;
unsigned long sword_debounce_time = 0;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Stone Crystals ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize crystal sensor pins
  for (int i = 0; i < 5; i++) {
    pinMode(CRYSTAL_PINS[i], INPUT_PULLUP);  // Assuming HIGH = not placed, LOW = placed
  }
  pinMode(SWORD_PIN, INPUT_PULLUP);  // Sword input pin
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Crystal placement puzzle ready!");
  print_instructions();
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  

  
  // Process crystal sensors
  process_crystal_sensors();
  
  // Process sword input if enabled
  if (sword_enabled) {
    process_sword_input();
  }
  
  // Check game state
  check_game_state();
  
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
      
      String status_topic = String("escaperoom/status/") + device_id;
      mqtt_client.publish(status_topic.c_str(), "online");
      
      // Subscribe to reset topic
      mqtt_client.subscribe(topic_reset);
      Serial.println("Subscribed to reset topic");
      
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Received MQTT message [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  // Handle reset command
  if (String(topic) == topic_reset && message.toLowerCase() == "true") {
    Serial.println("RESET COMMAND RECEIVED - Resetting to default state");
    reset_game_state();
  }
}

void process_crystal_sensors() {
  for (int i = 0; i < 5; i++) {
    // Read sensor (LOW = crystal placed with pull-up)
    bool sensor_reading = !digitalRead(CRYSTAL_PINS[i]);  // Invert for placed = true
    
    // Handle debouncing
    if (sensor_reading != crystal_prev_states[i]) {
      crystal_debounce_times[i] = millis();
    }
    
    if ((millis() - crystal_debounce_times[i]) > DEBOUNCE_DELAY) {
      if (sensor_reading != crystal_placed[i]) {
        // State changed
        crystal_placed[i] = sensor_reading;
        
        if (crystal_placed[i]) {
          crystal_placed_event(i);
        } else {
          crystal_removed_event(i);
        }
      }
    }
    
    crystal_prev_states[i] = sensor_reading;
  }
}

void trigger_audio_event(String event_name) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + String(audio_server_ip)+":15000" + "/" + event_name;
    http.begin(url);
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
      Serial.println("Audio event triggered: " + event_name);
    } else {
      Serial.println("Audio request failed: " + String(httpResponseCode));
    }
    
    http.end();
  } else {
    Serial.println("WiFi not connected - cannot trigger audio");
  }
}

void crystal_placed_event(int crystal_index) {
  Serial.print(CRYSTAL_NAMES[crystal_index]);
  Serial.println(" crystal PLACED");
  
  // Check if it's the GREEN crystal being placed too early
  if (crystal_index == GREEN && !green_unlocked) {
    Serial.println("WARNING: GREEN crystal placed but not unlocked yet!");
    Serial.println("GREEN crystal REJECTED - first four crystals must be placed first!");
    // Don't acknowledge invalid placement - no LED flash or audio
    return;
  }
  
  // Trigger individual crystal placement sound for valid placements
  String audio_event = "crystal_1_sound";
  
  if (audio_event != "") {
    trigger_audio_event(audio_event);
  }
  
  
  print_crystal_status();
}

void crystal_removed_event(int crystal_index) {
  Serial.print(CRYSTAL_NAMES[crystal_index]);
  Serial.println(" crystal REMOVED");
  
  // If first four were complete but a non-GREEN crystal was removed, reset first_four_complete
  // But keep green_unlocked true permanently once it's been unlocked
  if (first_four_complete && crystal_index != GREEN) {
    first_four_complete = false;
    Serial.println("First four progress RESET due to crystal removal");
    Serial.println("GREEN crystal remains UNLOCKED permanently");
  }
  
  // If all were complete but any crystal removed, reset all progress
  // But still keep green_unlocked true permanently
  if (all_crystals_complete) {
    all_crystals_complete = false;
    first_four_complete = false;
    Serial.println("ALL progress RESET due to crystal removal");
    Serial.println("GREEN crystal remains UNLOCKED permanently");
  }
  
  print_crystal_status();
}

void check_game_state() {
  if (all_crystals_complete || sequence_in_progress) return;
  
  // Check if first four crystals are placed (RED, BLUE, PURPLE, WHITE)
  bool first_four_placed = crystal_placed[RED] && 
                          crystal_placed[BLUE] && 
                          crystal_placed[PURPLE] && 
                          crystal_placed[WHITE];
  
  if (first_four_placed && !first_four_complete) {
    first_four_complete = true;
    green_unlocked = true;
    first_four_complete_event();
  }
  
  // Check if all crystals are placed
  bool all_placed = first_four_placed && crystal_placed[GREEN];
  
  if (all_placed && first_four_complete && !all_crystals_complete) {
    all_crystals_complete = true;
    all_crystals_complete_event();
  }
}

void first_four_complete_event() {
  Serial.println("=== FIRST FOUR CRYSTALS PLACED! ===");
  Serial.println("GREEN crystal is now UNLOCKED!");
  
  // Only play fanfare the very first time this happens
  if (!first_four_ever_completed) {
    first_four_ever_completed = true;  // Mark that fanfare has been played
    if (mqtt_client.connected()) {
      mqtt_client.publish(topic_first_four_placed, "true");
      Serial.println("Published: First four crystals placed - paradox compartment unlocked");
      delay(12500);
    }
  } else {
    Serial.println("Subsequent completion - no fanfare (GREEN stays unlocked)");
  }
}

void all_crystals_complete_event() {
  Serial.println("=== ALL CRYSTALS PLACED! ===");
  Serial.println("PULL SWORD SEQUENCE INITIATED!");
  
  sequence_in_progress = true;
  sword_enabled = true;  // Enable the sword input
  
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_all_placed, "true");
    Serial.println("Published: All crystals placed - Pull sword audio should play!");
  }
  
  Serial.println("Sword input is now ENABLED! Pull the sword to win!");
  
}



void process_sword_input() {
  // Read sword sensor (LOW = sword pulled with pull-up)
  bool sensor_reading = !digitalRead(SWORD_PIN);  // Invert for pulled = true
  
  // Handle debouncing
  if (sensor_reading != sword_prev_state) {
    sword_debounce_time = millis();
  }
  
  if ((millis() - sword_debounce_time) > DEBOUNCE_DELAY) {
    if (sensor_reading != sword_placed) {
      sword_placed = sensor_reading;
      
      if (sword_placed) {
        sword_pulled_event();
      }
    }
  }
  
  sword_prev_state = sensor_reading;
}

void sword_pulled_event() {
  Serial.println("=== SWORD PULLED! ===");
  Serial.println("WIN CONDITION TRIGGERED!");
  
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_win_condition, "true");
    Serial.println("Published: WIN CONDITION - Game Won!");
  }
  
}

void print_instructions() {
  Serial.println("=== CRYSTAL PLACEMENT INSTRUCTIONS ===");
  Serial.println("1. Place RED, BLUE, PURPLE, and WHITE crystals first");
  Serial.println("2. GREEN crystal will unlock after first 4 are placed");
  Serial.println("3. Place GREEN crystal to enable sword");
  Serial.println("4. Pull the sword to trigger win condition!");
  Serial.println("5. Removing any crystal will reset progress");
  Serial.println("6. Send 'true' to escaperoom/system/reset to reset game");
  Serial.println("=======================================");
}

void print_crystal_status() {
  Serial.println("=== CRYSTAL STATUS ===");
  for (int i = 0; i < 5; i++) {
    Serial.print(CRYSTAL_NAMES[i]);
    Serial.print(": ");
    Serial.println(crystal_placed[i] ? "PLACED" : "MISSING");
  }
  Serial.print("First four complete: ");
  Serial.println(first_four_complete ? "YES" : "NO");
  Serial.print("Green unlocked: ");
  Serial.println(green_unlocked ? "YES" : "NO");
  Serial.print("All complete: ");
  Serial.println(all_crystals_complete ? "YES" : "NO");
  Serial.println("======================");
}

void reset_game_state() {
  Serial.println("=== RESETTING GAME STATE ===");
  
  // Reset crystal states
  for (int i = 0; i < 5; i++) {
    crystal_placed[i] = false;
    crystal_prev_states[i] = false;
    crystal_debounce_times[i] = 0;
  }
  
  // Reset game state variables
  first_four_complete = false;
  green_unlocked = false;
  all_crystals_complete = false;
  sequence_in_progress = false;
  first_four_ever_completed = false;
  
  // Reset sword state
  sword_enabled = false;
  sword_placed = false;
  sword_prev_state = false;
  sword_debounce_time = 0;
  
  
  Serial.println("Game state reset to defaults - ready for new game!");
  print_crystal_status();
}



void publish_heartbeat() {
  static unsigned long last_heartbeat = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_heartbeat >= 30000) {
    if (mqtt_client.connected()) {
      String heartbeat_topic = String("escaperoom/heartbeat/") + device_id;
      String heartbeat_data = String("{\"uptime\":") + (current_time / 1000) + 
                             ",\"free_heap\":" + ESP.getFreeHeap() + 
                             ",\"first_four_complete\":" + (first_four_complete ? "true" : "false") +
                             ",\"all_complete\":" + (all_crystals_complete ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}