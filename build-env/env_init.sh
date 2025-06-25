#!/bin/bash

echo "UID=$(id -u)" >> ./.env
echo "GID=$(id -g)" >> ./.env

echo "# Put any custom setup commands here e.g. installing additional packages" >> custom_setup.sh