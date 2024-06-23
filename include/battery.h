/*
 * Copyright (c) 2018-2024 Corey Hinshaw
 */

#ifndef BATTERY_H
#define BATTERY_H

#include <bits/pthreadtypes.h>
#include <stdbool.h>
#include <time.h>

/* battery states */
#define STATE_AC 0
#define STATE_DISCHARGING 1
#define STATE_WARNING 2
#define STATE_CRITICAL 3
#define STATE_DANGER 4
#define STATE_FULL 5

/* system paths */
#define POWER_SUPPLY_SUBSYSTEM "/sys/class/power_supply"

/* Battery state strings */
#define POWER_SUPPLY_FULL "Full"
#define POWER_SUPPLY_DISCHARGING "Discharging"

#define POWER_SUPPLY_ATTR_LENGTH 15

/* battery information */
typedef struct BatteryState {
  char **names;
  int count;
  bool discharging;
  bool full;
  char state;
  int level;
  int energy_full;
  int energy_now;
  int inotify_fd;
  int *watch_fds;
  pthread_cond_t *bat_state_change;
  pthread_mutex_t *state_change_mut;
} BatteryState;

BatteryState init_batteries(char **battery_names, int battery_count);
void uninit_batteries(BatteryState * battery);
int find_batteries(char ***battery_names);
int validate_batteries(char **battery_names, int battery_count);
void wait_for_update_battery_state(BatteryState *battery, bool required);

#endif
