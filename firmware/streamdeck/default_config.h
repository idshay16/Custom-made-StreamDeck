// default_config.h — written to /config.json on first boot (or "config reset").
//
// SCHEMA
// {
//   "version": 1,
//   "typing": "altcode" | "direct",   // how text is typed (see hid.cpp)
//   "pages": [ { "name": "Main", "keys": [ <key> x up to 12 ] } ]
// }
// key (K1..K12, row-major from the top-left; null or {} = empty):
// {
//   "label": "Mic", "icon": "mic.png",          // icon file in /icons
//   "color": "#20c060",                           // idle tile + LED color
//   "press": <action>,                            // on press (or on release if "long" is set)
//   "long":  <action>,                            // optional, held >= 500 ms
//   "state": "obs.mute:Mic/Aux",                  // optional state that turns the key "on"
//   "on_color": "#ff2020", "on_label": "MUTED"    // optional look while on
// }
// action ("type" + fields; any action may add "flip": "<state>" to toggle a state):
//   hotkey : "keys": "ctrl+shift+m", "hold": false   (hold = press on down, release on up)
//   media  : "key": play_pause|next|prev|stop|mute|vol_up|vol_down
//   text   : "text": "...", "enter": false
//   launch : "cmd": "notepad"                         (Win+R, type, Enter)
//   url    : "url": "https://..."                     (same as launch)
//   macro  : "steps": [ {"keys":"ctrl+c"}, {"delay":100}, {"text":"hi"}, {"media":"mute"} ]
//   page   : "to": "<page name>" | "next" | "prev"
//   obs    : "op": scene|record|stream|mute|raw, "scene", "input", "request", "data"
//   govee  : "device": "<ip>", "op": toggle|on|off|brightness|color, "value": 0-100, "color": "#rrggbb"
//   multi  : "actions": [ <action>, ... ]
//   none   : nothing (useful with "flip": a long-press that only resyncs an
//            optimistic state, e.g. Discord mute got out of step)
#pragma once

static const char DEFAULT_CONFIG[] = R"json({
  "version": 1,
  "typing": "altcode",
  "pages": [
    {
      "name": "Main",
      "keys": [
        { "label": "Play",   "color": "#3080ff", "press": { "type": "media", "key": "play_pause" } },
        { "label": "Prev",   "color": "#3080ff", "press": { "type": "media", "key": "prev" } },
        { "label": "Next",   "color": "#3080ff", "press": { "type": "media", "key": "next" } },
        { "label": "Mute",   "color": "#ff9020", "press": { "type": "media", "key": "mute" } },
        { "label": "Vol -",  "color": "#ff9020", "press": { "type": "media", "key": "vol_down" } },
        { "label": "Vol +",  "color": "#ff9020", "press": { "type": "media", "key": "vol_up" } },
        { "label": "Discord Mic", "color": "#5865f2",
          "press": { "type": "hotkey", "keys": "ctrl+shift+f13", "flip": "discord.mute" },
          "long":  { "type": "none", "flip": "discord.mute" },
          "state": "discord.mute", "on_color": "#ff2020", "on_label": "Mic OFF" },
        { "label": "Discord Deafen", "color": "#5865f2",
          "press": { "type": "hotkey", "keys": "ctrl+shift+f14", "flip": "discord.deafen" },
          "state": "discord.deafen", "on_color": "#ff2020", "on_label": "Deaf" },
        { "label": "Record", "color": "#606060",
          "press": { "type": "obs", "op": "record" },
          "state": "obs.record", "on_color": "#ff2020", "on_label": "REC" },
        { "label": "Stream", "color": "#606060",
          "press": { "type": "obs", "op": "stream" },
          "state": "obs.stream", "on_color": "#ff2020", "on_label": "LIVE" },
        { "label": "Notepad", "color": "#20c060", "press": { "type": "launch", "cmd": "notepad" } },
        { "label": "OBS >",  "color": "#a0a0a0", "press": { "type": "page", "to": "next" } }
      ]
    },
    {
      "name": "OBS",
      "keys": [
        { "label": "Scene 1", "color": "#8040ff", "press": { "type": "obs", "op": "scene", "scene": "Scene" },
          "state": "obs.scene:Scene", "on_color": "#20c060" },
        { "label": "Scene 2", "color": "#8040ff", "press": { "type": "obs", "op": "scene", "scene": "Scene 2" },
          "state": "obs.scene:Scene 2", "on_color": "#20c060" },
        {}, {},
        { "label": "Mic", "color": "#20c060", "press": { "type": "obs", "op": "mute", "input": "Mic/Aux" },
          "state": "obs.mute:Mic/Aux", "on_color": "#ff2020", "on_label": "Mic OFF" },
        { "label": "Desktop", "color": "#20c060", "press": { "type": "obs", "op": "mute", "input": "Desktop Audio" },
          "state": "obs.mute:Desktop Audio", "on_color": "#ff2020", "on_label": "Muted" },
        {}, {},
        { "label": "Record", "color": "#606060", "press": { "type": "obs", "op": "record" },
          "state": "obs.record", "on_color": "#ff2020", "on_label": "REC" },
        { "label": "Stream", "color": "#606060", "press": { "type": "obs", "op": "stream" },
          "state": "obs.stream", "on_color": "#ff2020", "on_label": "LIVE" },
        {},
        { "label": "< Main", "color": "#a0a0a0", "press": { "type": "page", "to": "Main" } }
      ]
    }
  ]
})json";
