#!/bin/bash

echo "UID=$(id -u)" > ./.env
echo "GID=$(id -g)" >> ./.env

if [ ! -f custom-setup.sh ]; then
    echo "#!/bin/bash" >> custom-setup.sh
    echo "# Put any custom setup commands here e.g. installing additional packages" >> custom-setup.sh
fi
