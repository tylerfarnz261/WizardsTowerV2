#!/usr/bin/env python3
"""
Wizards Rune Controller
======================

This module controls the rune system using MCP23017 GPIO expanders.
Each rune has an input button and output light with specific behaviors.

Functions:
- Handle rune button presses and light control
- Manage rune timing (active duration, pulse intervals)
- Process "Spell_Success" HTTP endpoints
- Send MQTT messages for various game events
- Interface with Windows audio system

Hardware:
- Raspberry Pi with two MCP23017 GPIO expanders (I2C addresses 0x20, 0x21)
- Multiple runes with buttons and LEDs
- MQTT network connectivity

Author: Wizards Control System
"""

import time
import threading
import logging
import yaml
import json
import random
from typing import Dict, Any, Optional
from datetime import datetime, timedelta
from flask import Flask, request, jsonify
from flask_cors import CORS
import paho.mqtt.client as mqtt
import requests
from gpiozero import Button, LED
import busio
import digitalio
import board
from adafruit_mcp230xx.mcp23017 import MCP23017

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('/var/log/rune_controller.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class RuneController:
    """Main controller class for the rune system."""
    
    def __init__(self, config_path: str = "/config"):
        """Initialize the rune controller."""
        self.config = self._load_config(config_path)
        self.flask_app = Flask(__name__)
        CORS(self.flask_app)
        
        # System state
        self.runes_enabled = False
        self.active_rune = None
        self.active_rune_start_time = None
        self.game_state = {
            'cauldron_solved': False,
            'torch_states': {f'torch_{i}': False for i in range(1, 6)},
            'dream_runes_unlocked': False,
            'spotlight_completed': False,
            'mirror_runes_unlocked': False,
            'first_four_crystals_placed': False,
            'paradox_rune_unlocked': False,
            'shadow_rune_used_before': False,
            'owl_activation_count': 0,  # Track owl usage
            'owl_active': False,        # Track if owl is currently active
            'owl_disabled': False,      # Track if owl is permanently disabled
            'torch_runes_disabled': False,  # Track if torch runes are permanently disabled
            'fireplace_rune_disabled': False,  # Track if fireplace rune is permanently disabled
            'rat_rune_disabled': False,  # Track if rat rune is permanently disabled
            'mirror_1_disabled': False,  # Track if mirror_1 rune is permanently disabled
            'mirror_2_disabled': False   # Track if mirror_2 rune is permanently disabled
        }
        
        # Initialize hardware
        self._init_i2c_and_gpio()
        
        # Initialize MQTT
        self._init_mqtt()
        
        # Initialize Flask routes
        self._init_flask_routes()
        
        # Start background threads
        self._start_background_threads()
        
        logger.info("Rune Controller initialized successfully")
    
    def _load_config(self, config_path: str) -> Dict[str, Any]:
        """Load configuration from YAML files."""
        config = {}
        try:
            with open(f"{config_path}/system_config.yaml", 'r') as f:
                config.update(yaml.safe_load(f))
            with open(f"{config_path}/mqtt_topics.yaml", 'r') as f:
                config.update(yaml.safe_load(f))
            logger.info("Configuration loaded successfully")
        except Exception as e:
            logger.error(f"Failed to load configuration: {e}")
            raise
        return config
    
    def _init_i2c_and_gpio(self):
        """Initialize I2C connection and MCP23017 GPIO expander + GPIO pins."""
        try:
            # Initialize I2C
            self.i2c = busio.I2C(board.SCL, board.SDA)
            
            # Initialize MCP23017 (0x20)
            self.mcp = MCP23017(self.i2c, address=self.config['rune_controller']['i2c_address'])
            
            # Setup rune pins
            self.rune_buttons = {}
            self.rune_lights = {}
            
            # Setup MCP23017 runes (Fire + Dream runes)
            for rune_name, pins in self.config['rune_controller']['runes'].items():
                button_pin = self.mcp.get_pin(pins['button_pin'])
                light_pin = self.mcp.get_pin(pins['light_pin'])
                
                button_pin.direction = digitalio.Direction.INPUT
                button_pin.pull = digitalio.Pull.UP
                
                light_pin.direction = digitalio.Direction.OUTPUT
                light_pin.value = False
                
                self.rune_buttons[rune_name] = ('mcp', button_pin)
                self.rune_lights[rune_name] = ('mcp', light_pin)
            
            # Setup GPIO runes (Mirror + Shadow + Paradox runes) using GPIOZero
            for rune_name, pins in self.config['rune_controller']['gpio_runes'].items():
                button_pin_num = pins['button_pin']
                light_pin_num = pins['light_pin']
                
                # Create GPIOZero devices
                button_device = Button(button_pin_num, pull_up=True)
                light_device = LED(light_pin_num)
                
                # Turn off LED initially
                light_device.off()
                
                self.rune_buttons[rune_name] = ('gpio', button_device)
                self.rune_lights[rune_name] = ('gpio', light_device)
            
            # Track previous button states for edge detection (only for MCP buttons)
            self.prev_button_states = {}
            for name, (pin_type, button_pin) in self.rune_buttons.items():
                if pin_type == 'mcp':
                    self.prev_button_states[name] = True  # MCP buttons need edge detection
            
            logger.info(f"GPIO and I2C initialized with {len(self.rune_buttons)} runes")
            logger.info(f"MCP23017 runes: {list(self.config['rune_controller']['runes'].keys())}")
            logger.info(f"GPIO runes: {list(self.config['rune_controller']['gpio_runes'].keys())}")
            
        except Exception as e:
            logger.error(f"Failed to initialize I2C/GPIO: {e}")
            raise
    
    def _init_mqtt(self):
        """Initialize MQTT client and connection."""
        try:
            self.mqtt_client = mqtt.Client()
            self.mqtt_client.on_connect = self._on_mqtt_connect
            self.mqtt_client.on_message = self._on_mqtt_message
            
            mqtt_config = self.config['mqtt']
            self.mqtt_client.connect(
                mqtt_config['broker_host'], 
                mqtt_config['broker_port'], 
                mqtt_config['keepalive']
            )
            self.mqtt_client.loop_start()
            
            logger.info("MQTT client initialized and connected")
            
        except Exception as e:
            logger.error(f"Failed to initialize MQTT: {e}")
            raise
    
    def _on_mqtt_connect(self, client, userdata, flags, rc):
        """Callback for MQTT connection."""
        if rc == 0:
            logger.info("Connected to MQTT broker")
            # Subscribe to necessary topics
            topics = [
                self.config['runes']['enable'],
                self.config['game_state']['cauldron_solved'],
                self.config['esp32']['wand_cabinet'],
                self.config['esp32']['crystals_first_four']
            ]
            for topic in topics:
                client.subscribe(topic)
                logger.debug(f"Subscribed to topic: {topic}")
        else:
            logger.error(f"Failed to connect to MQTT broker with result code {rc}")
    
    def _on_mqtt_message(self, client, userdata, msg):
        """Handle incoming MQTT messages."""
        try:
            topic = msg.topic
            payload = msg.payload.decode()
            logger.debug(f"Received MQTT message: {topic} - {payload}")
            
            if topic == self.config['runes']['enable']:
                self.runes_enabled = payload.lower() == 'true'
                logger.info(f"Runes enabled: {self.runes_enabled}")
                if not self.runes_enabled:
                    self._deactivate_all_runes()
            
            elif topic == self.config['game_state']['cauldron_solved']:
                self.game_state['cauldron_solved'] = payload.lower() == 'true'
                if self.game_state['cauldron_solved']:
                    self.game_state['dream_runes_unlocked'] = True
                    logger.info("Dream runes unlocked - cauldron solved!")
            
            elif topic == self.config['esp32']['wand_cabinet']:
                if payload.lower() == 'true':
                    self.runes_enabled = True
                    logger.info("Wand cabinet opened - runes enabled!")
                    self._publish_mqtt(self.config['runes']['enable'], 'true')
            
            elif topic == self.config['esp32']['crystals_first_four']:
                if payload.lower() == 'true':
                    self.game_state['first_four_crystals_placed'] = True
                    self.game_state['paradox_rune_unlocked'] = True
                    logger.info("First four crystals placed - Paradox rune unlocked!")
            
        except Exception as e:
            logger.error(f"Error processing MQTT message: {e}")
    
    def _init_flask_routes(self):
        """Initialize Flask HTTP routes."""
        
        @self.flask_app.route('/spell_success', methods=['POST'])
        def spell_success():
            """Handle spell success endpoint during active rune period."""
            if not self.active_rune:
                return jsonify({'status': 'error', 'message': 'No active rune'}), 400
            
            try:
                # Process the spell success for the active rune
                result = self._process_spell_success(self.active_rune)
                
                # Deactivate the current rune
                self._deactivate_rune(self.active_rune)
                
                return jsonify({
                    'status': 'success', 
                    'rune': self.active_rune,
                    'actions': result
                }), 200
                
            except Exception as e:
                logger.error(f"Error processing spell success: {e}")
                return jsonify({'status': 'error', 'message': str(e)}), 500
        
        @self.flask_app.route('/status', methods=['GET'])
        def get_status():
            """Get current system status."""
            return jsonify({
                'runes_enabled': self.runes_enabled,
                'active_rune': self.active_rune,
                'game_state': self.game_state,
                'timestamp': datetime.now().isoformat()
            })
        
        @self.flask_app.route('/reset', methods=['POST'])
        def reset_system():
            """Reset the rune system."""
            self._deactivate_all_runes()
            self.game_state = {
                'cauldron_solved': False,
                'torch_states': {f'torch_{i}': False for i in range(1, 6)},
                'dream_runes_unlocked': False,
                'spotlight_completed': False,
                'mirror_runes_unlocked': False,
                'first_four_crystals_placed': False,
                'paradox_rune_unlocked': False,
                'shadow_rune_used_before': False,
                'owl_activation_count': 0,
                'owl_active': False,
                'owl_disabled': False,
                'torch_runes_disabled': False,
                'fireplace_rune_disabled': False,
                'rat_rune_disabled': False,
                'mirror_1_disabled': False,
                'mirror_2_disabled': False
            }
            return jsonify({'status': 'success', 'message': 'System reset'})
    
    def _process_spell_success(self, rune_name: str) -> Dict[str, Any]:
        """Process spell success for specific rune type."""
        actions = []
        
        try:
            # Fire runes controlling torches
            if rune_name.startswith('fire_torch_'):
                torch_num = rune_name.split('_')[-1]
                torch_topic = self.config['torches'][f'torch_{torch_num}']
                self._publish_mqtt(torch_topic, 'true')
                self.game_state['torch_states'][f'torch_{torch_num}'] = True
                actions.append(f"Activated torch {torch_num}")
                
                # Check if fireplace mantle should unlock
                self._check_fireplace_mantle_unlock()
            
            # Fire rune for fireplace door
            elif rune_name == 'fire_fireplace':
                self._publish_mqtt(self.config['maglocks']['fireplace_door'], 'unlock')
                actions.append("Unlocked fireplace door")
                
                # Deactivate fireplace rune after successful casting
                self.game_state['fireplace_rune_disabled'] = True
                actions.append("Fireplace rune permanently disabled")
                logger.info("Fireplace rune disabled permanently after unlocking door")
            
            # Dream rune with owl and audio
            elif rune_name == 'dream_owl':
                if self.game_state['dream_runes_unlocked']:
                    self._activate_owl_with_audio()
                    self.game_state['spotlight_completed'] = True
                    self.game_state['mirror_runes_unlocked'] = True
                    actions.append("Activated owl with audio - MIRROR RUNES UNLOCKED!")
                    logger.info("Owl rune completed - Mirror runes are now unlocked!")
                else:
                    actions.append("Dream rune locked - cauldron not solved")
            
            # Dream rune for rat cage
            elif rune_name == 'dream_rat_cage':
                if self.game_state['dream_runes_unlocked']:
                    self._play_audio_then_unlock_rat_cage()
                    actions.append("Playing audio then unlocking rat cage")
                    
                    # Deactivate rat rune after successful casting
                    self.game_state['rat_rune_disabled'] = True
                    actions.append("Rat rune permanently disabled")
                    logger.info("Rat rune disabled permanently after unlocking cage")
                else:
                    actions.append("Dream rune locked - cauldron not solved")
            
            # Mirror runes
            elif rune_name in ['mirror_1', 'mirror_2']:
                if self.game_state['mirror_runes_unlocked']:
                    mirror_num = rune_name.split('_')[1]
                    backlight_topic = self.config['lighting'][f'mirror_backlight_{mirror_num}']
                    self._publish_mqtt(backlight_topic, 'true')
                    actions.append(f"Activated mirror {mirror_num} backlight")
                    
                    # Disable only this specific mirror rune after successful casting
                    self.game_state[f'{rune_name}_disabled'] = True
                    actions.append(f"Mirror {mirror_num} rune permanently disabled")
                    logger.info(f"Mirror {mirror_num} rune disabled permanently after successful casting")
                    
                    # Permanently disable owl rune after successful mirror cast (if not already disabled)
                    if not self.game_state['owl_disabled']:
                        self.game_state['owl_disabled'] = True
                        actions.append("Owl rune permanently disabled - mirror knowledge used")
                        logger.info("Owl rune disabled permanently - mirror spell knowledge utilized")
                else:
                    actions.append("Mirror runes locked - spotlight rune not completed")
            
            # Shadow realm rune
            elif rune_name == 'shadow_realm':
                # Check if this is first time or repeat usage
                if not self.game_state['shadow_rune_used_before']:
                    # First time - play discovery audio
                    event_name = self.config['audio']['audio_events']['shadow_first_time']
                    self._request_audio_play(event_name)
                    self.game_state['shadow_rune_used_before'] = True
                    actions.append("First shadow realm discovery - played discovery audio")
                else:
                    # Repeat usage - play return audio
                    event_name = self.config['audio']['audio_events']['shadow_repeat']
                    self._request_audio_play(event_name)
                    actions.append("Shadow realm return - played return audio")
                
                # Activate shadow realm effects
                self._publish_mqtt(self.config['lighting']['blacklights'], 'true')
                self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'activate')
                actions.append("Activated blacklights and shadow realm sprites")
            
            # Paradox rune
            elif rune_name == 'paradox':
                if self.game_state['paradox_rune_unlocked']:
                    self._publish_mqtt(self.config['sprite_players']['paradox'], 'activate')
                    actions.append("Activated paradox sprite players")
                else:
                    actions.append("Paradox rune locked - first four crystals not placed")
            
            logger.info(f"Processed spell success for {rune_name}: {actions}")
            return {'rune': rune_name, 'actions': actions}
            
        except Exception as e:
            logger.error(f"Error processing spell success for {rune_name}: {e}")
            raise
    
    def _activate_owl_with_audio(self):
        """Activate owl with sequential/random audio system."""
        try:
            # Turn on spotlight
            self._publish_mqtt(self.config['lighting']['spotlight'], 'true')
            self.game_state['owl_active'] = True
            
            # Determine which audio to play based on activation count
            audio_event = self._get_owl_audio_event()
            self._request_audio_play(audio_event)
            
            # Increment owl usage counter
            self.game_state['owl_activation_count'] += 1
            
            # Schedule spotlight turn-off (assuming 30 seconds for audio)
            threading.Timer(30.0, self._turn_off_spotlight).start()
            
            logger.info(f"Owl activated (count: {self.game_state['owl_activation_count']}) - played: {audio_event}")
            
        except Exception as e:
            logger.error(f"Error in owl with audio: {e}")
    
    def _get_owl_audio_event(self) -> str:
        """Get the appropriate owl audio event based on usage count."""
        count = self.game_state['owl_activation_count']
        
        # First 3 activations are sequential
        if count == 0:
            return self.config['audio']['audio_events']['owl_sequence_1']
        elif count == 1:
            return self.config['audio']['audio_events']['owl_sequence_2'] 
        elif count == 2:
            return self.config['audio']['audio_events']['owl_sequence_3']
        else:
            # After first 3, randomly pick from the random pool
            random_events = [
                self.config['audio']['audio_events']['owl_random_1'],
                self.config['audio']['audio_events']['owl_random_2'],
                self.config['audio']['audio_events']['owl_random_3']
            ]
            return random.choice(random_events)
    
    def _turn_off_spotlight(self):
        """Turn off the spotlight (used for owl rune) - scheduled timer."""
        try:
            self._publish_mqtt(self.config['lighting']['spotlight'], 'false')
            self.game_state['owl_active'] = False
            logger.info("Owl spotlight automatically turned off after timer")
        except Exception as e:
            logger.error(f"Error turning off spotlight: {e}")
    
    def _play_audio_then_unlock_rat_cage(self):
        """Play audio then unlock rat cage."""
        try:
            # Play audio
            event_name = self.config['audio']['audio_events']['dream_rune']
            self._request_audio_play(event_name)
            
            # Unlock rat cage after audio (assuming 10 seconds)
            threading.Timer(10.0, self._unlock_rat_cage).start()
            
        except Exception as e:
            logger.error(f"Error in audio then rat cage unlock: {e}")
    
    def _unlock_rat_cage(self):
        """Unlock the rat cage."""
        self._publish_mqtt(self.config['maglocks']['rat_cage'], 'unlock')
    
    def _request_audio_play(self, event_name: str):
        """Request audio play from Windows event listener."""
        try:
            windows_config = self.config['audio']['windows_audio']
            # Simple GET request to trigger your Windows event listener
            url = f"http://{windows_config['ip']}/{event_name}"
            
            response = requests.get(
                url, 
                timeout=windows_config['timeout']
            )
            
            if response.status_code == 200:
                logger.info(f"Successfully triggered audio event: {event_name}")
            else:
                logger.warning(f"Audio event request failed with status {response.status_code}")
                
        except Exception as e:
            logger.error(f"Failed to trigger audio event: {e}")
    
    def _check_fireplace_mantle_unlock(self):
        """Check if fireplace mantle should unlock based on torch states."""
        solution = self.config['game_logic']['torch_solution']
        current_state = self.game_state['torch_states']
        
        # Check if current state matches solution
        matches_solution = all(
            current_state.get(f'torch_{i}', False) == solution[f'torch_{i}']
            for i in range(1, 6)
        )
        
        if matches_solution and not self.game_state['torch_runes_disabled']:
            # Play sound effect first, then turn on all torches, then deactivate runes
            self._handle_torch_puzzle_completion()
            
            # Unlock fireplace mantle
            self._publish_mqtt(self.config['maglocks']['fireplace_mantle'], 'unlock')
            logger.info("Fireplace mantle unlocked - torch puzzle solved!")
            
            # Deactivate all torch runes permanently
            self.game_state['torch_runes_disabled'] = True
            logger.info("All torch runes disabled permanently - torch puzzle solved!")
    
    def _handle_torch_puzzle_completion(self):
        """Handle torch puzzle completion: play sound effect then turn on all torches."""
        try:
            # Send HTTP request to Windows audio system for torch puzzle completion sound
            audio_url = f"http://{self.config['audio']['windows_audio']['ip']}/torch_puzzle_complete"
            
            response = requests.get(audio_url, timeout=5)
            if response.status_code == 200:
                logger.info("Torch puzzle completion sound effect triggered")
            else:
                logger.warning(f"Failed to trigger torch completion audio: {response.status_code}")
                
            # Wait a moment for the sound effect, then turn on all torches
            time.sleep(2)  # Adjust timing as needed
            
            # Turn on all torches
            for i in range(1, 6):
                torch_topic = self.config['torches'][f'torch_{i}']
                self._publish_mqtt(torch_topic, 'true')
                logger.info(f"Turned on torch {i} for puzzle completion")
            
        except Exception as e:
            logger.error(f"Error in torch puzzle completion handling: {e}")
            # Still turn on torches even if audio fails
            for i in range(1, 6):
                torch_topic = self.config['torches'][f'torch_{i}']
                self._publish_mqtt(torch_topic, 'true')
    
    def _deactivate_all_runes(self):
        """Deactivate all runes and turn off all lights."""
        self.active_rune = None
        self.active_rune_start_time = None
        
        for rune_name, (pin_type, light_device) in self.rune_lights.items():
            if pin_type == 'mcp':
                light_device.value = False
            else:  # GPIOZero LED
                light_device.off()
        
        logger.info("All runes deactivated")
    
    def _deactivate_rune(self, rune_name: str):
        """Deactivate specific rune."""
        if self.active_rune == rune_name:
            self.active_rune = None
            self.active_rune_start_time = None
            
        if rune_name in self.rune_lights:
            pin_type, light_device = self.rune_lights[rune_name]
            if pin_type == 'mcp':
                light_device.value = False
            else:  # GPIOZero LED
                light_device.off()
            
        logger.info(f"Rune {rune_name} deactivated")
    
    def _publish_mqtt(self, topic: str, payload: str):
        """Publish MQTT message."""
        try:
            self.mqtt_client.publish(topic, payload)
            logger.debug(f"Published MQTT: {topic} - {payload}")
        except Exception as e:
            logger.error(f"Failed to publish MQTT message: {e}")
    
    def _start_background_threads(self):
        """Start background monitoring threads."""
        # Button monitoring thread
        button_thread = threading.Thread(target=self._button_monitor_loop, daemon=True)
        button_thread.start()
        
        # Rune timing thread
        timing_thread = threading.Thread(target=self._rune_timing_loop, daemon=True)
        timing_thread.start()
        
        logger.info("Background threads started")
    
    def _button_monitor_loop(self):
        """Monitor rune buttons for presses."""
        while True:
            try:
                if not self.runes_enabled:
                    time.sleep(0.1)
                    continue
                
                for rune_name, (pin_type, button_device) in self.rune_buttons.items():
                    if pin_type == 'mcp':
                        # MCP23017 buttons - use edge detection as before
                        current_state = button_device.value
                        prev_state = self.prev_button_states[rune_name]
                        
                        # Detect button press (falling edge)
                        if prev_state and not current_state:
                            self._handle_rune_press(rune_name)
                        
                        self.prev_button_states[rune_name] = current_state
                        
                    else:  # GPIOZero button
                        # GPIOZero buttons - check if pressed (inverted logic since we use pull-up)
                        if button_device.is_pressed:
                            # Simple debouncing - check if this is a new press
                            if not hasattr(button_device, '_was_pressed'):
                                button_device._was_pressed = False
                            
                            if not button_device._was_pressed:
                                self._handle_rune_press(rune_name)
                                button_device._was_pressed = True
                        else:
                            if hasattr(button_device, '_was_pressed'):
                                button_device._was_pressed = False
                
                time.sleep(0.05)  # 50ms polling interval
                
            except Exception as e:
                logger.error(f"Error in button monitor loop: {e}")
                time.sleep(1)
    
    def _handle_rune_press(self, rune_name: str):
        """Handle rune button press."""
        try:
            # Check if any rune is already active (including this same rune)
            if self.active_rune:
                logger.info(f"Rune {rune_name} press ignored - {self.active_rune} is currently active")
                return
            
            # Check if owl rune is permanently disabled
            if rune_name == 'dream_owl' and self.game_state['owl_disabled']:
                logger.info(f"Owl rune {rune_name} press ignored - owl knowledge already used")
                return
            
            # Check if torch runes are permanently disabled
            if rune_name.startswith('fire_torch_') and self.game_state['torch_runes_disabled']:
                logger.info(f"Torch rune {rune_name} press ignored - torch puzzle already solved")
                return
            
            # Check if fireplace rune is permanently disabled
            if rune_name == 'fire_fireplace' and self.game_state['fireplace_rune_disabled']:
                logger.info(f"Fireplace rune {rune_name} press ignored - fireplace door already unlocked")
                return
            
            # Check if rat rune is permanently disabled
            if rune_name == 'dream_rat_cage' and self.game_state['rat_rune_disabled']:
                logger.info(f"Rat rune {rune_name} press ignored - rat cage already unlocked")
                return
            
            # Check if specific mirror runes are permanently disabled
            if rune_name == 'mirror_1' and self.game_state['mirror_1_disabled']:
                logger.info(f"Mirror 1 rune {rune_name} press ignored - mirror 1 already activated")
                return
                
            if rune_name == 'mirror_2' and self.game_state['mirror_2_disabled']:
                logger.info(f"Mirror 2 rune {rune_name} press ignored - mirror 2 already activated")
                return
            
            # Check if dream runes are unlocked
            if rune_name.startswith('dream_') and not self.game_state['dream_runes_unlocked']:
                logger.info(f"Dream rune {rune_name} press ignored - cauldron not solved")
                return
            
            # Check if mirror runes are unlocked
            if rune_name.startswith('mirror_') and not self.game_state['mirror_runes_unlocked']:
                logger.info(f"Mirror rune {rune_name} press ignored - owl rune not completed")
                return
            
            # Check if paradox rune is unlocked
            if rune_name == 'paradox' and not self.game_state['paradox_rune_unlocked']:
                logger.info(f"Paradox rune press ignored - first four crystals not placed")
                return
            
            # Activate the rune
            self.active_rune = rune_name
            self.active_rune_start_time = datetime.now()
            
            logger.info(f"Rune {rune_name} activated")
            
        except Exception as e:
            logger.error(f"Error handling rune press {rune_name}: {e}")
    
    def _rune_timing_loop(self):
        """Handle rune timing and light pulsing."""
        while True:
            try:
                if self.active_rune and self.active_rune_start_time:
                    elapsed = (datetime.now() - self.active_rune_start_time).total_seconds()
                    active_duration = self.config['rune_controller']['rune_active_duration']
                    pulse_interval = self.config['rune_controller']['rune_pulse_interval']
                    
                    if elapsed >= active_duration:
                        # Deactivate the rune after timeout
                        logger.info(f"Rune {self.active_rune} timed out after {active_duration} seconds")
                        self._deactivate_rune(self.active_rune)
                    else:
                        # Pulse the light
                        pulse_cycle = int(elapsed) % (pulse_interval * 2)
                        light_on = pulse_cycle < pulse_interval
                        
                        if self.active_rune in self.rune_lights:
                            pin_type, light_device = self.rune_lights[self.active_rune]
                            if pin_type == 'mcp':
                                light_device.value = light_on
                            else:  # GPIOZero LED
                                if light_on:
                                    light_device.on()
                                else:
                                    light_device.off()
                
                time.sleep(0.1)
                
            except Exception as e:
                logger.error(f"Error in rune timing loop: {e}")
                time.sleep(1)
    
    def run(self):
        """Run the Flask application."""
        try:
            flask_config = self.config['flask']
            self.flask_app.run(
                host='0.0.0.0',
                port=flask_config['rune_controller_port'],
                debug=flask_config['debug']
            )
        except KeyboardInterrupt:
            logger.info("Shutting down rune controller...")
        finally:
            self.cleanup()
    
    def cleanup(self):
        """Cleanup resources and connections."""
        try:
            # GPIOZero handles GPIO cleanup automatically
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()
            logger.info("Cleanup completed")
        except Exception as e:
            logger.error(f"Error during cleanup: {e}")

if __name__ == "__main__":
    # Initialize and run the rune controller
    controller = RuneController("/home/pi/Wizards/config")
    try:
        controller.run()
    except KeyboardInterrupt:
        controller.cleanup()