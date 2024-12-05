#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Orbbec *.{hpp,cpp,txt,json} LICENSE release/

mv release score-addon-orbbec
7z a score-addon-orbbec.zip score-addon-orbbec
