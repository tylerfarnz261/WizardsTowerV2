/*
  Wizard's Tower - Staircase ESP32 Controller
  ========================================
  
  This ESP32 controls the staircase puzzle with 5 buttons and NeoPixel strips.
  Each button controls a section of the NeoPixel strip with color cycling.
  When all stairs show the correct color sequence, the puzzle is solved.
  
  Hardware:
  - ESP32 development board
  - 5 momentary push buttons (one per stair)
  - NeoPixel LED strip (80 LEDs total, 16 per stair)
  - Pull-up resistors (or use internal pull-ups)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/staircase/solved
  
  Author: Tyler Farnsworth
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "staircase_esp32";

// Audio server settings
const char* audio_server_ip = "192.168.0.156";
const int audio_server_port = 15000;

// MQTT topics
const char* topic_staircase_solved = "escaperoom/esp32/staircase/solved";
const char* topic_reset = "escaperoom/system/reset";

// Hardware pins
const int BUTTON_PINS[5] = {16, 17, 5, 18, 19};  // 5 stair buttons
const int NEOPIXEL_PIN = 13;

// NeoPixel configuration
const int PIXELS_PER_STAIR = 16;
const int TOTAL_PIXELS = 80;  // 5 stairs * 16 pixels each
const int NUM_STAIRS = 5;

// Color definitions (RGB values) - 5 colors plus OFF
const uint32_t COLORS[] = {
  0x000000,  // OFF (BLACK)
  0xFF0000,  // RED
  0x00FF00,  // GREEN  
  0x0000FF,  // BLUE
  0xFFFF00,  // YELLOW
  0x800080   // PURPLE
};

const int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

// Solution colors for each stair (indices into COLORS array)
const int SOLUTION_COLORS[5] = {1, 2, 3, 4, 5};  // RED, GREEN, BLUE, YELLOW, PURPLE

// NeoPixel object
Adafruit_NeoPixel strip(TOTAL_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Button states and debouncing
bool button_states[5] = {HIGH, HIGH, HIGH, HIGH, HIGH};
bool last_button_states[5] = {HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long last_debounce_times[5] = {0, 0, 0, 0, 0};
const unsigned long DEBOUNCE_DELAY = 50;

// Current color for each stair
int current_stair_colors[5] = {0, 0, 0, 0, 0};  // Start with first color (RED)
bool stair_active[5] = {false, false, false, false, false};

// Game state
bool puzzle_solved = false;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Staircase ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize button pins
  for (int i = 0; i < NUM_STAIRS; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  }
  
  // Initialize NeoPixel strip
  strip.begin();
  strip.clear();
  strip.show();
  delay(100);
  
  // Show initial pattern (all stairs with first color)
  update_all_stairs();
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Staircase puzzle ready - press buttons to cycle colors!");
  print_solution();
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Process button inputs
  process_buttons();
  
  // Check for puzzle solution
  check_solution();
  
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
      
      // Subscribe to own topic to prevent re-triggering after manual activation
      mqtt_client.subscribe(topic_staircase_solved);
      Serial.println("Subscribed to self-publishing topic");
      
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
  if (String(topic) == topic_reset && message.equalsIgnoreCase("true")) {
    Serial.println("RESET COMMAND RECEIVED - Resetting to default state");
    reset_game_state();
  }
  // Handle staircase solved (prevents re-triggering)
  else if (String(topic) == topic_staircase_solved && message.equalsIgnoreCase("true")) {
    Serial.println("Staircase marked as solved - preventing physical re-trigger");
    puzzle_solved = true;
  }
}

void process_buttons() {
  for (int i = 0; i < NUM_STAIRS; i++) {
    bool reading = digitalRead(BUTTON_PINS[i]);
    
    // Handle debouncing
    if (reading != last_button_states[i]) {
      last_debounce_times[i] = millis();
    }
    
    if ((millis() - last_debounce_times[i]) > DEBOUNCE_DELAY) {
      if (reading != button_states[i]) {
        button_states[i] = reading;
        
        // Button pressed (LOW with pull-up)
        if (button_states[i] == LOW && !puzzle_solved) {
          button_pressed(i);
        }
      }
    }
    
    last_button_states[i] = reading;
  }
}

void button_pressed(int stair_index) {
  Serial.print("Stair ");
  Serial.print(stair_index + 1);
  Serial.print(" button pressed - ");

  
  // Cycle to next color (now 6 options including OFF)
  current_stair_colors[stair_index] = (current_stair_colors[stair_index] + 1) % 6;
  
  Serial.print("Color index: ");
  Serial.println(current_stair_colors[stair_index]);
  
  // Update the stair display
  update_stair(stair_index);

  // Trigger random stair sound effect (1-7)
  int random_sound = random(1, 8);  // random(1, 8) gives 1-7
  String event_name = "stair_" + String(random_sound);
  trigger_audio_event(event_name);
  
  // Mark stair as active
  stair_active[stair_index] = true;
}

void trigger_audio_event(String event_name) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + String(audio_server_ip) + ":15000/" + event_name;
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

void update_stair(int stair_index) {
  // Calculate the pixel range for this stair
  int start_pixel = stair_index * PIXELS_PER_STAIR;
  int end_pixel = start_pixel + PIXELS_PER_STAIR;
  
  // Get the color for this stair
  uint32_t color = COLORS[current_stair_colors[stair_index]];
  
  // Set all pixels for this stair to the current color
  for (int pixel = start_pixel; pixel < end_pixel; pixel++) {
    strip.setPixelColor(pixel, color);
  }
  
  strip.show();
}

void update_all_stairs() {
  for (int i = 0; i < NUM_STAIRS; i++) {
    update_stair(i);
  }
}

void check_solution() {
  if (puzzle_solved) return;
  
  // Check if all stairs are active and showing correct colors
  bool all_correct = true;
  
  for (int i = 0; i < NUM_STAIRS; i++) {
    if (!stair_active[i] || current_stair_colors[i] != SOLUTION_COLORS[i]) {
      all_correct = false;
      break;
    }
  }
  
  if (all_correct) {
    puzzle_solved = true;
    staircase_solved();
  }
}

void staircase_solved() {
  // Check if already solved (prevents re-triggering)
  if (puzzle_solved) {
    Serial.println("Staircase already solved - ignoring physical trigger");
    return;
  }
  
  Serial.println("=== STAIRCASE PUZZLE SOLVED! ===");
  puzzle_solved = true;  // Mark as solved
  
  // Trigger staircase completion sound
  trigger_audio_event("staircase_victory");
  
  
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_staircase_solved, "true");
    Serial.println("Published: Staircase solved");
  } else {
    Serial.println("MQTT not connected - message not sent!");
  }
}


void print_solution() {
  Serial.println("=== SOLUTION COLORS ===");
  const char* color_names[] = {"OFF", "RED", "GREEN", "BLUE", "YELLOW", "PURPLE"};
  
  for (int i = 0; i < NUM_STAIRS; i++) {
    Serial.print("Stair ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(color_names[SOLUTION_COLORS[i]]);
  }
  Serial.println("======================");
}

void reset_game_state() {
  Serial.println("=== RESETTING GAME STATE ===");
  
  // Turn off all NeoPixels
  strip.clear();
  strip.show();
  
  // Reset all game states
  puzzle_solved = false;
  
  // Reset stair states
  for (int i = 0; i < NUM_STAIRS; i++) {
    current_stair_colors[i] = 0;  // Start with first color (OFF)
    stair_active[i] = false;
    button_states[i] = HIGH;
    last_button_states[i] = HIGH;
    last_debounce_times[i] = 0;
  }
  
  // Show initial pattern (all stairs with first color)
  update_all_stairs();
  
  Serial.println("Game state reset to defaults - ready for new game!");
}


void publish_heartbeat() {
  static unsigned long last_heartbeat = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_heartbeat >= 30000) {
    if (mqtt_client.connected()) {
      String heartbeat_topic = String("escaperoom/heartbeat/") + device_id;
      String heartbeat_data = String("{\"uptime\":") + (current_time / 1000) + 
                             ",\"free_heap\":" + ESP.getFreeHeap() + 
                             ",\"puzzle_solved\":" + (puzzle_solved ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}