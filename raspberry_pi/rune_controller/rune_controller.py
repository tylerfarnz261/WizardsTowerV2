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
#TODO Paradox rune, final puzzle, and 5th crystal placed squence
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
            'torch_puzzle_solved': False,  # Track if torch puzzle has been solved
            'dream_runes_unlocked': False,
            'spotlight_completed': False,
            'mirror_runes_unlocked': False,
            'first_four_crystals_placed': False,
            'paradox_rune_unlocked': False,
            'shadow_rune_used_before': False,
            'in_shadow_realm': False,  # Track if currently in shadow realm
            'shadow_realm_toggle_active': True,  # Track if shadow realm toggle is still active
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
            
            # Track previous button states for edge detection
            self.prev_button_states = {}
            for name, (pin_type, button_device) in self.rune_buttons.items():
                if pin_type == 'mcp':
                    self.prev_button_states[name] = True  # MCP buttons need edge detection
                else:  # GPIO buttons also need state tracking
                    self.prev_button_states[name] = False  # GPIO buttons start not pressed
            
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
                self.config['runes']['torch_runes_disable'],  # Listen for torch puzzle solved
                self.config['esp32']['cauldron'],  # Listen for cauldron solved to unlock dream runes
                self.config['esp32']['wand_cabinet'],
                self.config['esp32']['crystals_first_four'],
                self.config['game_state']['win_condition'],  # Listen for win condition
                self.config['system']['reset_game']  # Listen for system reset
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
            
            elif topic == self.config['runes']['torch_runes_disable']:
                if payload.lower() == 'true':
                    self.game_state['torch_runes_disabled'] = True
                    self.game_state['torch_puzzle_solved'] = True  # Track that torch puzzle is solved
                    logger.info("🔥 Torch runes disabled - puzzle solved by central controller!")
            
            elif topic == self.config['esp32']['cauldron']:
                self.game_state['cauldron_solved'] = payload.lower() == 'true'
                if self.game_state['cauldron_solved']:
                    self.game_state['dream_runes_unlocked'] = True
                    logger.info("Dream runes unlocked - cauldron solved!")
            
            elif topic == self.config['esp32']['wand_cabinet']:
                if payload.lower() == 'true':
                    time.sleep(36.8)
                    self.runes_enabled = True
                    logger.info("Wand cabinet opened - runes enabled!")
                    self._publish_mqtt(self.config['runes']['enable'], 'true')
            
            elif topic == self.config['esp32']['crystals_first_four']:
                if payload.lower() == 'true':
                    self.game_state['first_four_crystals_placed'] = True
                    self.game_state['paradox_rune_unlocked'] = True
                    logger.info("First four crystals placed - Paradox rune unlocked!")
            
            elif topic == self.config['game_state']['win_condition']:
                if payload.lower() == 'true':
                    logger.info("WIN CONDITION RECEIVED - Deactivating all runes and stopping system")
                    self._deactivate_all_runes()
                    self.system_enabled = False
                    self.rune_system_enabled = False
            
            elif topic == self.config['system']['reset_game']:
                if payload.lower() == 'true':
                    logger.info("SYSTEM RESET RECEIVED - Resetting all game state to defaults")
                    self._reset_system_to_defaults()
            
        except Exception as e:
            logger.error(f"Error processing MQTT message: {e}")
    
    def _reset_system_to_defaults(self):
        """Reset the entire rune system to default state."""
        try:
            # Deactivate all runes and turn off lights
            self._deactivate_all_runes()
            
            # Reset runes enabled state
            self.runes_enabled = False
            
            # Reset all game state to defaults
            self.game_state = {
                'cauldron_solved': False,
                'torch_states': {f'torch_{i}': True for i in range(1, 6)},  # Torches start ON
                'torch_puzzle_solved': False,
                'dream_runes_unlocked': False,
                'spotlight_completed': False,
                'mirror_runes_unlocked': False,
                'first_four_crystals_placed': False,
                'paradox_rune_unlocked': False,
                'shadow_rune_used_before': False,
                'in_shadow_realm': False,
                'shadow_realm_toggle_active': True,
                'owl_activation_count': 0,
                'owl_active': False,
                'owl_disabled': False,
                'torch_runes_disabled': False,
                'fireplace_rune_disabled': False,
                'rat_rune_disabled': False,
                'mirror_1_disabled': False,
                'mirror_2_disabled': False
            }
            
            logger.info("Rune system reset to defaults - ready for new game!")
            
        except Exception as e:
            logger.error(f"Error resetting system to defaults: {e}")
    
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
                
                # Always return rune to normal waiting state after spell success
                # This stops the pulsing and makes it ready for new input
                self._return_rune_to_normal_state(self.active_rune)
                
                return jsonify({
                    'status': 'success', 
                    'rune': result.get('rune', self.active_rune),
                    'actions': result.get('actions', []),
                    'message': 'Rune returned to normal state - ready for new input'
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
                'torch_puzzle_solved': False,
                'dream_runes_unlocked': False,
                'spotlight_completed': False,
                'mirror_runes_unlocked': False,
                'first_four_crystals_placed': False,
                'paradox_rune_unlocked': False,
                'shadow_rune_used_before': False,
                'in_shadow_realm': False,
                'shadow_realm_toggle_active': True,
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
        
        @self.flask_app.route('/rune/<rune_name>/trigger', methods=['POST'])
        def trigger_rune_endpoint(rune_name):
            """Manually trigger a rune (for testing) - bypasses unlock checks."""
            try:
                # Process the manual rune trigger
                result = self._process_manual_rune_trigger(rune_name)
                
                return jsonify({
                    'status': 'success',
                    'rune': rune_name,
                    'actions': result['actions']
                })
                
            except Exception as e:
                logger.error(f"Error manually triggering rune {rune_name}: {e}")
                return jsonify({'status': 'error', 'message': str(e)}), 500
        
        @self.flask_app.route('/rune/<rune_name>/unlock', methods=['POST'])
        def unlock_rune_endpoint(rune_name):
            """Manually unlock a rune type (for testing) - sets unlock state."""
            try:
                # Process the manual rune unlock
                result = self._process_rune_unlock(rune_name)
                
                return jsonify({
                    'status': 'success',
                    'rune': rune_name,
                    'actions': result['actions']
                })
                
            except Exception as e:
                logger.error(f"Error manually unlocking rune {rune_name}: {e}")
                return jsonify({'status': 'error', 'message': str(e)}), 500
    
    def _process_spell_success(self, rune_name: str) -> Dict[str, Any]:
        """Process spell success for specific rune type."""
        actions = []
        
        try:
            # Fire runes controlling torches
            if rune_name.startswith('fire_torch_'):
                torch_num = rune_name.split('_')[-1]
                torch_topic = self.config['torches'][f'torch_{torch_num}']
                logger.info(f"Publishing torch control: topic={torch_topic}, payload=true")  # Debug logging
                self._publish_mqtt(torch_topic, 'true')  # Tell central controller to toggle torch
                actions.append(f"Toggled torch {torch_num} light (sent to central controller)")
                
                logger.info(f"Torch {torch_num} rune spell success - torch light toggled")
                return {'rune': rune_name, 'actions': actions}
            
            # Fire rune for fireplace door
            elif rune_name == 'fire_fireplace':
                self._request_audio_play('fireplace_door')
                self._publish_mqtt(self.config['runes']['fire_fireplace'], 'true')  # Publish to rune topic
                actions.append("Fire fireplace rune activated - sent to central controller")
                
                # Deactivate fireplace rune after successful casting
                self.game_state['fireplace_rune_disabled'] = True
                actions.append("Fireplace rune permanently disabled")
                logger.info("Fireplace rune disabled permanently after successful casting")
            
            # Dream rune with owl and audio
            elif rune_name == 'dream_owl':
                if self.game_state['dream_runes_unlocked']:
                    self._activate_owl_with_audio()
                    self.game_state['spotlight_completed'] = True
                    self.game_state['mirror_runes_unlocked'] = True
                    actions.append("Activated owl with audio - MIRROR RUNES UNLOCKED!")
                    logger.info("Owl rune completed - Mirror runes are now unlocked!")
                else:
                    # FIZZLE: Dream rune is locked
                    self._request_audio_play('rune_fizzle')  # Optional fizzle sound
                    actions.append("Dream rune fizzled - cauldron not solved yet")
                    logger.info(f"Dream rune {rune_name} fizzled - cauldron not solved")
            
            # Dream rune for rat cage
            elif rune_name == 'dream_rat_cage':
                if self.game_state['dream_runes_unlocked']:
                    self._play_audio_then_activate_rat_rune()
                    actions.append("Playing audio then activating rat cage rune")
                    
                    # Deactivate rat rune after successful casting
                    self.game_state['rat_rune_disabled'] = True
                    actions.append("Rat rune permanently disabled")
                    logger.info("Rat rune disabled permanently after successful casting")
                else:
                    # FIZZLE: Dream rune is locked
                    self._request_audio_play('rune_fizzle')  # Optional fizzle sound
                    actions.append("Dream rune fizzled - cauldron not solved yet")
                    logger.info(f"Dream rune {rune_name} fizzled - cauldron not solved")
            
            # Mirror runes
            elif rune_name in ['mirror_1', 'mirror_2']:
                if self.game_state['mirror_runes_unlocked']:
                    mirror_num = rune_name.split('_')[1]
                    backlight_topic = self.config['lighting'][f'mirror_backlight_{mirror_num}']
                    self._request_audio_play('mirror')
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
                    # FIZZLE: Mirror rune is locked
                    self._request_audio_play('rune_fizzle')  # Optional fizzle sound
                    actions.append("Mirror rune fizzled - owl rune not completed yet")
                    logger.info(f"Mirror rune {rune_name} fizzled - owl not completed")
            
            # Shadow realm rune - Toggle between shadow realm and normal realm
            elif rune_name == 'shadow_realm':
                if not self.game_state['shadow_realm_toggle_active']:
                    actions.append("Shadow realm toggle disabled - paradox rune has been used")
                    return {'rune': rune_name, 'actions': actions}
                
                if not self.game_state['in_shadow_realm']:
                    # Entering shadow realm
                    if not self.game_state['shadow_rune_used_before']:
                        # First time ever - play discovery audio
                        self._request_audio_play('shadow_first_time')
                        self.game_state['shadow_rune_used_before'] = True
                        actions.append("First shadow realm discovery - entered shadow realm")
                    else:
                        # Returning to shadow realm - play return audio
                        self._request_audio_play('shadow_enter')
                        actions.append("Returned to shadow realm")
                    
                    # Activate shadow realm effects
                    self._publish_mqtt(self.config['lighting']['blacklights'], 'true')
                    self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'activate')
                    self.game_state['in_shadow_realm'] = True
                    
                    # Disable all other runes while in shadow realm
                    self._disable_runes_for_shadow_realm()
                    actions.append("Activated blacklights, shadow sprites, and disabled other runes")
                    
                else:
                    # Leaving shadow realm back to normal realm
                    self._request_audio_play('shadow_exit')
                    
                    # Deactivate shadow realm effects
                    self._publish_mqtt(self.config['lighting']['blacklights'], 'false')
                    self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'deactivate')
                    self.game_state['in_shadow_realm'] = False
                    
                    # Reset torch states to default ON (unless already solved)
                    if not self.game_state.get('torch_puzzle_solved', False):
                        self._reset_torches_to_default()
                        actions.append("Reset torches to default ON state")
                    
                    # Re-enable all runes (except permanently disabled ones)
                    self._enable_runes_from_shadow_realm()
                    actions.append("Exited shadow realm - returned to normal realm and re-enabled runes")
            
            # Paradox rune
            #TODO retrieve audio from FIVER and put into Houdini
            elif rune_name == 'paradox':
                if self.game_state['paradox_rune_unlocked']:
                    self._request_audio_play('paradox_rune')
                    time.sleep(10) #TODO Adjust sleep for professor paradox
                    self._publish_mqtt(self.config['sprite_players']['paradox'], 'activate')
                    
                    # Disable shadow realm toggle permanently
                    self.game_state['shadow_realm_toggle_active'] = False
                    
                    # If currently in shadow realm, exit it
                    if self.game_state['in_shadow_realm']:
                        self._publish_mqtt(self.config['lighting']['blacklights'], 'false')
                        self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'deactivate')
                        self.game_state['in_shadow_realm'] = False
                        self._enable_runes_from_shadow_realm()
                        actions.append("Paradox activated - exited shadow realm and disabled shadow realm toggle")
                    else:
                        actions.append("Paradox activated - shadow realm toggle disabled")
                    
                    actions.append("Activated paradox sprite players")
                else:
                    # FIZZLE: Paradox rune is locked
                    self._request_audio_play('rune_fizzle')  # Optional fizzle sound
                    actions.append("Paradox rune fizzled - first four crystals not placed yet")
                    logger.info(f"Paradox rune fizzled - first four crystals not placed")
            
            logger.info(f"Processed spell success for {rune_name}: {actions}")
            return {'rune': rune_name, 'actions': actions}
            
        except Exception as e:
            logger.error(f"Error processing spell success for {rune_name}: {e}")
            raise
    
    def _process_manual_rune_trigger(self, rune_name: str) -> Dict[str, Any]:
        """Process manual rune trigger - bypasses unlock checks for testing."""
        actions = []
        
        try:
            # Mirror runes - bypass unlock checks
            if rune_name in ['mirror_1', 'mirror_2']:
                mirror_num = rune_name.split('_')[1]
                backlight_topic = self.config['lighting'][f'mirror_backlight_{mirror_num}']
                
                # TAKE CARE OF AUDIO IN HOUDINI
                #self._request_audio_play('mirror')
                #actions.append(f"Played mirror audio")
                
                # Activate mirror backlight
                self._publish_mqtt(backlight_topic, 'true')
                actions.append(f"Activated mirror {mirror_num} backlight")
                
                # Disable only this specific mirror rune after successful casting
                self.game_state[f'{rune_name}_disabled'] = True
                actions.append(f"Mirror {mirror_num} rune permanently disabled")
                logger.info(f"Mirror {mirror_num} rune disabled permanently after manual trigger")
                
                # Permanently disable owl rune after successful mirror cast (if not already disabled)
                if not self.game_state['owl_disabled']:
                    self.game_state['owl_disabled'] = True
                    actions.append("Owl rune permanently disabled - mirror knowledge used")
                    logger.info("Owl rune disabled permanently - mirror spell knowledge utilized")

            elif rune_name == 'fire_fireplace':
                
                self._publish_mqtt(self.config['runes']['fire_fireplace'], 'true')  # Publish to rune topic
                actions.append("Fire fireplace rune activated - sent to central controller")
                
                # Deactivate fireplace rune after successful casting
                self.game_state['fireplace_rune_disabled'] = True
                actions.append("Fireplace rune permanently disabled")
                logger.info("Fireplace rune disabled permanently after successful casting")
            
            elif rune_name == 'dream_rat_cage':

                time.sleep(12)
                self._send_rat_cage_activation()
                actions.append("Playing audio then activating rat cage rune")
                
                # Deactivate rat rune after successful casting
                self.game_state['rat_rune_disabled'] = True
                actions.append("Rat rune permanently disabled")
                logger.info("Rat rune disabled permanently after successful casting")

            elif rune_name == 'shadow_realm':
                # For manual trigger, just toggle shadow realm effects without checking toggle state
                if not self.game_state['in_shadow_realm']:
                    # Activate shadow realm effects
                    self._publish_mqtt(self.config['lighting']['blacklights'], 'true')
                    self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'activate')
                    self.game_state['in_shadow_realm'] = True
                    
                    # Disable all other runes while in shadow realm
                    self._disable_runes_for_shadow_realm()
                    actions.append("Manually activated shadow realm - blacklights on and other runes disabled")
                    
                else:
                    # Deactivate shadow realm effects
                    self._publish_mqtt(self.config['lighting']['blacklights'], 'false')
                    self._publish_mqtt(self.config['sprite_players']['shadow_realm'], 'deactivate')
                    self.game_state['in_shadow_realm'] = False
                    
                    # Reset torch states to default ON (unless already solved)
                    if not self.game_state.get('torch_puzzle_solved', False):
                        self._reset_torches_to_default()


            
            # Add more rune types here as needed for future expansion
            # elif rune_name == 'some_other_rune':
            #     # Handle other rune manual triggers
            #     pass
            
            else:
                actions.append(f"Manual trigger not implemented for rune: {rune_name}")
                logger.warning(f"Manual trigger requested for unsupported rune: {rune_name}")
            
            logger.info(f"Processed manual trigger for {rune_name}: {actions}")
            return {'rune': rune_name, 'actions': actions}
            
        except Exception as e:
            logger.error(f"Error processing manual trigger for {rune_name}: {e}")
            raise
    
    def _process_rune_unlock(self, rune_name: str) -> Dict[str, Any]:
        """Process manual rune unlock - sets unlock state for rune types."""
        actions = []
        
        try:
            # Dream runes unlock
            if rune_name == 'dream' or rune_name.startswith('dream_'):
                self.game_state['dream_runes_unlocked'] = True
                actions.append("Dream runes unlocked")
                logger.info("Dream runes manually unlocked")
            
            # Mirror runes unlock
            elif rune_name == 'mirror' or rune_name.startswith('mirror_'):
                self.game_state['mirror_runes_unlocked'] = True
                actions.append("Mirror runes unlocked")
                logger.info("Mirror runes manually unlocked")
            
            # Paradox rune unlock
            elif rune_name == 'paradox':
                self.game_state['paradox_rune_unlocked'] = True
                actions.append("Paradox rune unlocked")
                logger.info("Paradox rune manually unlocked")
            
            # Special unlock: Enable all runes system
            elif rune_name == 'system' or rune_name == 'all':
                self.runes_enabled = True
                actions.append("Rune system enabled (wand cabinet bypass)")
                logger.info("Rune system manually enabled - bypassed wand cabinet requirement")
            
            # Add more unlock types here as needed for future expansion
            # elif rune_name == 'some_other_category':
            #     # Handle other unlock types
            #     pass
            
            else:
                actions.append(f"Unknown unlock type: {rune_name}")
                logger.warning(f"Unknown unlock type requested: {rune_name}")
            
            logger.info(f"Processed manual unlock for {rune_name}: {actions}")
            return {'rune': rune_name, 'actions': actions}
            
        except Exception as e:
            logger.error(f"Error processing manual unlock for {rune_name}: {e}")
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
            threading.Timer(10.0, self._turn_off_spotlight).start()
            
            logger.info(f"Owl activated (count: {self.game_state['owl_activation_count']}) - played: {audio_event}")
            
        except Exception as e:
            logger.error(f"Error in owl with audio: {e}")
    
    def _get_owl_audio_event(self) -> str:
        """Get the appropriate owl audio event based on usage count."""
        count = self.game_state['owl_activation_count']
        
        # First 3 activations are sequential
        if count == 0:
            return 'owl_discovery_1'
        elif count == 1:
            return 'owl_discovery_2'
        elif count == 2:
            return 'owl_discovery_3'
        elif count == 3:
            return 'owl_discovery_4'
        else:
            # After first 3, randomly pick from the random pool
            random_events = [
                'owl_knowledge_1',
                'owl_knowledge_2',
                'owl_knowledge_3',
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
    
    def _play_audio_then_activate_rat_rune(self):
        """Play audio then send rat cage activation to central controller."""
        try:
            # Play audio
            event_name = 'rat_cage_unlock'
            self._request_audio_play(event_name)
            
            # Send rat cage activation to central controller after audio (assuming 12 seconds)
            threading.Timer(12.0, self._send_rat_cage_activation).start()
            
        except Exception as e:
            logger.error(f"Error in audio then rat cage activation: {e}")
    
    def _send_rat_cage_activation(self):
        """Send rat cage activation signal to central controller."""
        self._publish_mqtt(self.config['runes']['dream_rat_cage'], 'true')
        logger.info("Sent rat cage activation to central controller")
    
    def _disable_runes_for_shadow_realm(self):
        """Temporarily disable all runes except shadow rune while in shadow realm."""
        # Note: in_shadow_realm state is used by button monitor loop to only allow shadow rune
        # We don't disable runes_enabled here as wand cabinet should stay "opened"  
        logger.info("In shadow realm - only shadow rune will respond to button presses")
    
    def _enable_runes_from_shadow_realm(self):
        """Re-enable runes when exiting shadow realm (except permanently disabled ones)."""
        # Note: runes_enabled stays True, just in_shadow_realm is set to False
        logger.info("Exited shadow realm - all runes (except permanently disabled) re-enabled")
    
    def _reset_torches_to_default(self):
        """Reset all torches to default ON state (used when exiting shadow realm)."""
        try:
            # Send reset command to central controller
            reset_topic = self.config['torches']['reset_to_default']
            self._publish_mqtt(reset_topic, 'true')
            
            # Update local game state
            for i in range(1, 6):
                self.game_state['torch_states'][f'torch_{i}'] = True
            
            logger.info("Sent torch reset command to central controller - all torches set to ON")
            
        except Exception as e:
            logger.error(f"Error resetting torches to default: {e}")
    
    def _request_audio_play(self, event_name: str):
        """Request audio play from Windows event listener."""
        try:
            print('hit')
            # Debug: Print config structure to help troubleshoot
            print(f"Config keys: {list(self.config.keys()) if self.config else 'None'}")
            if 'audio' in self.config:
                print(f"Audio config keys: {list(self.config['audio'].keys())}")
            else:
                print("No 'audio' key found in config")
                # Fallback to network config values
                windows_config = {
                    'ip': self.config['network']['windows_audio_ip'],
                    'port': self.config['network']['windows_audio_port'],
                    'timeout': 10
                }
                print(f"Using fallback config: {windows_config}")
            
            if 'audio' in self.config and 'windows_audio' in self.config['audio']:
                windows_config = self.config['audio']['windows_audio']
            else:
                # Fallback to network config values
                windows_config = {
                    'ip': self.config['network']['windows_audio_ip'],
                    'port': self.config['network']['windows_audio_port'],
                    'timeout': 10
                }
                logger.warning("Using fallback audio config from network section")
            
            # Simple GET request to trigger your Windows event listener
            url = f"http://{windows_config['ip']}:{windows_config['port']}/{event_name}"
            print(url)
            
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
            print(f"Full exception details: {str(e)}")
            import traceback
            traceback.print_exc()
    
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
    
    def _return_rune_to_normal_state(self, rune_name: str):
        """Return rune to normal waiting state after spell success."""
        # Clear active rune state
        if self.active_rune == rune_name:
            self.active_rune = None
            self.active_rune_start_time = None
            
        # Turn off rune light
        if rune_name in self.rune_lights:
            pin_type, light_device = self.rune_lights[rune_name]
            if pin_type == 'mcp':
                light_device.value = False
            else:  # GPIOZero LED
                light_device.off()
            
        logger.info(f"Rune {rune_name} returned to normal state - ready for new input")
    
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
                # Don't monitor ANY runes until wand cabinet is opened
                if not self.runes_enabled:
                    time.sleep(0.1)
                    continue
                
                for rune_name, (pin_type, button_device) in self.rune_buttons.items():
                    # Special case: In shadow realm, only monitor shadow rune
                    if self.game_state.get('in_shadow_realm', False) and rune_name != 'shadow_realm':
                        continue
                    
                    if pin_type == 'mcp':
                        # MCP23017 buttons - use edge detection
                        current_state = button_device.value
                        prev_state = self.prev_button_states[rune_name]
                        
                        # Detect button press (falling edge)
                        if prev_state and not current_state:
                            self._handle_rune_press(rune_name)
                        
                        self.prev_button_states[rune_name] = current_state
                        
                    else:  # GPIOZero button
                        # GPIOZero buttons - check for press edge detection
                        current_state = button_device.is_pressed
                        prev_state = self.prev_button_states[rune_name]
                        
                        # Detect button press (rising edge for is_pressed)
                        if current_state and not prev_state:
                            self._handle_rune_press(rune_name)
                        
                        self.prev_button_states[rune_name] = current_state
                
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
            
            # First check: NO runes work until wand cabinet is opened
            if not self.runes_enabled:
                logger.info(f"Rune {rune_name} press ignored - wand cabinet not opened yet")
                return
            
            # Special case: Shadow rune has additional checks for realm toggling
            if rune_name == 'shadow_realm':
                if not self.game_state['shadow_realm_toggle_active']:
                    logger.info(f"Shadow rune press ignored - toggle disabled after paradox rune")
                    return
                # Shadow rune passes all checks, allow it to proceed
            
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
            
            # REMOVED: Early return checks for locked runes - allow them to be activated but fizzle during spell_success
            # Dream runes, Mirror runes, and Paradox rune can now be pressed even when locked
            # They will fizzle out during spell_success if still locked
            
            # Activate the rune (regardless of lock status - let it fizzle during spell_success if locked)
            self.active_rune = rune_name
            self.active_rune_start_time = datetime.now()
            
            # Play rune activation sound effect
            try:
                self._request_audio_play("rune_activated")
                logger.debug(f"Played activation sound for rune {rune_name}")
            except Exception as audio_error:
                logger.warning(f"Failed to play activation sound for {rune_name}: {audio_error}")
            
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
                        self._request_audio_play("rune_fizzled")  # Play fizzle sound effect
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
    # Auto-detect config path based on environment
    import os
    if os.name == 'nt':  # Windows
        config_path = "c:/Users/tyler/WizardsClaude/escape_room_controller/config"
    else:  # Linux/Raspberry Pi
        config_path = "/home/pi/Wizards/config"
    
    print(f"Using config path: {config_path}")
    controller = RuneController(config_path)
    try:
        controller.run()
    except KeyboardInterrupt:
        controller.cleanup()