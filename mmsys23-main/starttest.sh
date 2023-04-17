#!/bin/bash 
cmake .
make
cp ./bin/MPDtest ./mininet/
chmod 777 ./mininet/MPDtest
cd mininet
sudo python twohosts_twoswitches.py
# client ./MPDtest ./config/downnode_mn.json
