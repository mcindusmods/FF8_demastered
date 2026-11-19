#pragma once
//DEMASTER's day/night clock (DAY_NIGHT in demaster.conf): one clock for the whole game.
//
//It runs while the game draws frames: in fields, on the world map and in menus at 1 second per second (DAY_NIGHT_DAY of day
//and DAY_NIGHT_NIGHT of night, DAY_NIGHT_FADE-second fades), in battle DAY_NIGHT_BATTLE times slower, so the time of day
//goes on from wherever the battle left it. A field with time = day or night in its _pbr.txt is always that, and the clock
//waits underneath while you are there (battles started from it too) until you go somewhere else. DEMASTER_EXP\field_time.txt
//can say otherwise for some fields at some part of the story (fieldpbr::TimeRule: Deling City follows the clock but is night
//for Laguna's visit to Julia and the sorceress parade); its rules are applied when a field is entered and every frame in it,
//by the story's counter (GAME_MOMENT). Each save slot has its
//own time of day (DAY_NIGHT_SAVES): when the game writes a save file, the file, its play time and the clock go into
//DEMASTER_EXP\daynight.dat (sealed: field_pbr.h PackSaveClocks), and loading that save (its play time comes back) brings
//its time of day back. A save without one (made before, or elsewhere) gets the time its play time gives; a new game starts
//in the morning. With DAY_NIGHT_SAVES=0 there is one clock for the game, kept in DEMASTER_EXP\daynight.txt.
//
//Keys for testing (DAY_NIGHT_KEYS): Ctrl+F7 swaps day and night, Ctrl+F8 pauses the clock, Ctrl+F9 runs it at 1x, 10x,
//60x, Ctrl+F6 jumps to 5 seconds before the next fade. While the clock is paused or sped up, and for a few seconds after a
//key, a thin bar across the top of the screen shows the cycle (amber day, blue night) and where the clock is (DAY_NIGHT_BAR).
//
//What it changes: PBR fields (day and night lights cross-fade, the painted light dims at night, the glows come on) and the
//characters lit by their room. Battles and the world map have no PBR yet; they only move the clock.

#include "field_pbr.h"
#include "field_moving_lights.h"

#include <string>

void DayNightSwap(); //SwapBuffers (renderer thread): the clock moves on, the keys, the file, the bar
//game thread, when a field's .map is read (and the renderer, when its _pbr.txt is read again): its time of day
void DayNightFieldEntered(const std::string& field, fieldpbr::FieldTime own);
//how night the clock says it is (0 day, 1 night, between: a fade); 0 with DAY_NIGHT off. Renderer thread
double DayNightClockAmount();
//the place you are: the last field's time (its _pbr.txt's, or field_time.txt's rule for this point of the story; cycle on the
//world map and in fields that follow the clock)
fieldpbr::FieldTime DayNightPlaceTime();
//for a field with this time setting: its own (day 0, night 1) or the clock's
double DayNightAmount(fieldpbr::FieldTime time);
//the evening (DAY_NIGHT_EVENING): like the night amount, but its dusk starts earlier. The day's own lights go out with
//1 - this and the glows come on with it. The clock's, or a field's fixed time
double DayNightClockEvening();
double DayNightEvening(fieldpbr::FieldTime time);
//the clock itself: seconds into its cycle, and the cycle (DAY_NIGHT_DAY ...). Renderer thread
double DayNightClockTime();
fieldpbr::DayNightCycle DayNightCycleInUse();
//where the clock is on the sun's arc (by day) or the moon's (by night), for the moving lights. With DAY_NIGHT off: the day,
//mid-morning. Renderer thread
fieldpbr::SkyTime DayNightSky();
