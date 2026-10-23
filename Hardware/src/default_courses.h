#pragma once
#include <stdint.h>

// Courses installed on the display out of the box. Pars come from the clubs'
// own scorecards where available (see README for sources); edit from the phone.
struct DefaultCourse {
  const char* name;
  uint8_t holes;
  uint8_t par[18];
};

static const DefaultCourse kDefaultCourses[] = {
    {"Byneset Nord", 18, {5, 3, 4, 4, 4, 3, 5, 3, 4, 5, 4, 4, 3, 4, 5, 4, 4, 4}},
    {"Byneset S\xC3\xB8r", 9, {4, 4, 3, 4, 3, 4, 3, 3, 4}},
    {"Trondheim GK Sommerseter", 18, {4, 4, 3, 5, 4, 4, 4, 5, 3, 4, 4, 3, 5, 4, 4, 4, 5, 3}},
    {"Asker Golfklubb", 18, {4, 3, 5, 3, 4, 3, 5, 4, 4, 3, 5, 3, 5, 4, 3, 5, 4, 3}},
    {"B\xC3\xA6rum Golfklubb", 18, {5, 4, 3, 5, 3, 4, 4, 4, 5, 3, 4, 5, 3, 4, 4, 3, 4, 4}},
    {"Saurines de la Torre", 18, {4, 4, 4, 4, 5, 3, 4, 5, 4, 3, 4, 4, 5, 3, 4, 4, 5, 3}},
    {"New Sierra Golf", 18, {5, 4, 3, 5, 4, 3, 4, 4, 4, 4, 5, 4, 3, 4, 4, 4, 3, 5}},
    {"La Serena Golf", 18, {5, 4, 3, 4, 4, 4, 5, 4, 3, 4, 5, 4, 4, 4, 3, 4, 3, 5}},
    {"Lo Romero Golf", 18, {4, 5, 4, 4, 3, 5, 3, 4, 4, 5, 4, 3, 4, 4, 3, 4, 5, 4}},
};
