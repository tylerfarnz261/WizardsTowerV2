#!/bin/bash
"""
Startup script for Rune Controller
=================================

This script sets up the environment and starts the rune controller service.
It should be run at system boot.
"""

# Set working directory
cd /home/pi/wizards/raspberry_pi/rune_controller

# Activate virtual environment if it exists
if [ -d "/home/pi/wizards/venv" ]; then
    source /home/pi/wizards/venv/bin/activate
    echo "Activated Python virtual environment"
fi

# Set environment variables
export PYTHONPATH="/home/pi/wizards:$PYTHONPATH"
export FLASK_APP="rune_controller.py"
export FLASK_ENV="production"

# Start the rune controller
echo "Starting Rune Controller..."
python3 rune_controller.py