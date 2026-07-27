// このファイルを wifi_config.h という名前でコピーして、実際の値を書き込んでください。
// wifi_config.h は .gitignore 済みなので Git にはコミットされません。

#pragma once

#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// pc_game/game_server.py を動かしているPCのIPアドレスとポート
// (例: ターミナルで `ipconfig getifaddr en0` などで確認したIP)
#define PC_SERVER_HOST "192.168.1.100"
#define PC_SERVER_PORT 8000
