#!/bin/bash
"""
Startup script for Central Controller
====================================

This script sets up the environment and starts the central controller service.
It should be run at system boot.
"""

# Set working directory
cd /home/pi/Wizards/raspberry_pi/central_controller

# Activate virtual environment if it exists
if [ -d "/home/pi/Wizards/venv" ]; then
    source /home/pi/Wizards/venv/bin/activate
    echo "Activated Python virtual environment"
fi

# Set environment variables
export PYTHONPATH="/home/pi/Wizards:$PYTHONPATH"
export FLASK_APP="central_controller.py"
export FLASK_ENV="production"

# Start the central controller
echo "Starting Central Controller..."
python3 central_controller.py