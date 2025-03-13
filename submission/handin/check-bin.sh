#!/bin/sh
THISYEAR=$(date +%Y)
COUNT=$(tar ztvf bin.tar.gz | awk '{print $4;}' | grep -c ^$THISYEAR)
