/*
 * Copyright (c) 2018-2024 Corey Hinshaw
 */

#include "battery.h"
#include <atomic_ops.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define INOTIFY_BUF_SIZE 4096

static char *attr_path = NULL;

static void set_attributes(char *battery_name, char **now_attribute,
                           char **full_attribute) {
  sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/charge_now", battery_name);
  if (access(attr_path, F_OK) == 0) {
    *now_attribute = "charge_now";
    *full_attribute = "charge_full";
  } else {
    sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/energy_now", battery_name);
    if (access(attr_path, F_OK) == 0) {
      *now_attribute = "energy_now";
      *full_attribute = "energy_full";
    } else {
      *now_attribute = "capacity";
      *full_attribute = NULL;
    }
  }
}

static bool is_type_battery(char *name) {
  FILE *file;
  char type[11] = "";

  sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/type", name);
  file = fopen(attr_path, "r");
  if (file != NULL) {
    if (fscanf(file, "%10s", type) == 0) { /* Continue... */
    }
    fclose(file);
  }
  return strcmp(type, "Battery") == 0;
}

static bool has_capacity_field(char *name) {
  FILE *file;
  int capacity = -1;
  char *now_attribute;
  char *full_attribute;

  set_attributes(name, &now_attribute, &full_attribute);

  if (strcmp(now_attribute, "capacity") == 0) {
    sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/capacity", name);
    file = fopen(attr_path, "r");
    if (file != NULL) {
      if (fscanf(file, "%d", &capacity) == 0) { /* Continue... */
      }
      fclose(file);
    }
  } else {
    capacity = 1;
  }
  return capacity >= 0;
}

static bool is_battery(char *name) {
  return is_type_battery(name) && has_capacity_field(name);
}

int find_batteries(char ***battery_names) {
  unsigned int path_len =
      strlen(POWER_SUPPLY_SUBSYSTEM) + POWER_SUPPLY_ATTR_LENGTH;
  unsigned int entry_name_len = 5;
  int battery_count = 0;
  DIR *dir;
  struct dirent *entry;

  attr_path = realloc(attr_path, path_len + entry_name_len);

  dir = opendir(POWER_SUPPLY_SUBSYSTEM);
  if (dir) {
    while ((entry = readdir(dir)) != NULL) {
      if (strlen(entry->d_name) > entry_name_len) {
        entry_name_len = strlen(entry->d_name);
        attr_path = realloc(attr_path, path_len + entry_name_len);
        if (attr_path == NULL)
          err(EXIT_FAILURE, "Memory allocation failed");
      }

      if (is_battery(entry->d_name)) {
        *battery_names =
            realloc(*battery_names, sizeof(char *) * (battery_count + 1));
        if (*battery_names == NULL)
          err(EXIT_FAILURE, "Memory allocation failed");
        (*battery_names)[battery_count] = strdup(entry->d_name);
        if ((*battery_names)[battery_count] == NULL)
          err(EXIT_FAILURE, "Memory allocation failed");
        battery_count++;
      }
    }
    closedir(dir);
  }

  return battery_count;
}

struct BatteryStateWrapper {
  BatteryState *battery;
  int bat_id;
};

void *watch_for_file_changes(void *battery) {
  struct BatteryStateWrapper *bat = battery;
  char buf[INOTIFY_BUF_SIZE];
  while (bat->battery->watching) {
    // blocks here, until the watched file changes or a signal is send to the
    // thread
    int len = read(bat->battery->inotify_fd, &buf, sizeof(buf));

    // error while reading -> signal was send to stop or IO-Error
    if (len == -1) {
      perror("Unexpected Error in inotify thread");
      printf("Thread stopped\n");
      return NULL;
    }

    // file changed, update request with conditional variable
    pthread_cond_signal(bat->battery->bat_state_change);
  }
  printf("Thread stopped\n");
  return NULL;
}

BatteryState *init_batteries(char **battery_names, int battery_count) {
  BatteryState *battery;
  battery = calloc(1, sizeof(*battery));
  battery->names = battery_names;
  battery->count = battery_count;
  battery->bat_state_change = calloc(1, sizeof(*battery->bat_state_change));
  battery->state_change_mut = calloc(1, sizeof(*battery->state_change_mut));
  if (battery->bat_state_change == NULL || battery->state_change_mut == NULL) {
    perror("Error on initialising conditional var");
    return battery;
  }

  // initialise conditional variable and mutex
  // for battery charging state changes on all batteries
  pthread_cond_init(battery->bat_state_change, NULL);
  pthread_mutex_init(battery->state_change_mut, NULL);
  battery->watching = calloc(1, sizeof(*battery->watching));
  if (battery->watching == NULL) {
    perror("Error on initialising atomic variable for inotify");
    return battery;
  }
  *battery->watching = ATOMIC_VAR_INIT(true);

  // init inotify-fd for notification on battery charging state changes
  battery->inotify_fd = inotify_init();
  if (battery->inotify_fd == -1) {
    perror("Error on initialising inotify");
    return battery;
  }
  battery->watch_fds = calloc(battery_count, sizeof(int));
  if (battery->watch_fds == NULL) {
    perror("Error while creating memory for inotify watch fds");
    return battery;
  }

  battery->thread_ids = calloc(battery->count, sizeof(*battery->thread_ids));
  if (battery->thread_ids == NULL) {
    perror("Error while creating memory for inotify thread infos");
    return battery;
  }

  // add watch fds for file modifications for each battery charge state file
  for (int i = 0; i < battery->count; i++) {
    sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/status", battery->names[i]);
    battery->watch_fds[i] =
        inotify_add_watch(battery->inotify_fd, attr_path, IN_MODIFY | IN_ACCESS);
    if (battery->watch_fds[i] == -1) {
      fprintf(stderr, "Cannot watch '%s': %s\n", attr_path, strerror(errno));
      continue;
    }

    struct BatteryStateWrapper *b_state =
        calloc(1, sizeof(struct BatteryStateWrapper));
    if (b_state == NULL) {
      perror("Error while creating memory for battery-state wrapper");
      continue;
    }
    b_state->battery = battery;
    b_state->bat_id = i;

    // start a thread polling the watch_fd
    int ret = pthread_create(&battery->thread_ids[i], NULL,
                             &watch_for_file_changes, b_state);
    if (ret != 0) {
      printf("Couldn't start the inotify thread for %s\n", battery->names[i]);
    }
  }

  return battery;
}

void uninit_batteries(BatteryState *battery) {
  if (battery->inotify_fd != -1) {
    // stopping threads
    battery->watching = false;
    if (battery->thread_ids != NULL) {
      for (int i = 0; i < battery->count; i++) {
        pthread_cancel(battery->thread_ids[i]);
        pthread_join(battery->thread_ids[i], NULL);
      }
    }
    pthread_cond_broadcast(battery->bat_state_change);

    // close watch fds
    if (battery->watch_fds != NULL) {
      for (int i = 0; i < battery->count; i++) {
        if (battery->watch_fds[i] != -1) {
          close(battery->watch_fds[i]);
        }
      }
      // stop inotify
      close(battery->inotify_fd);
    }
  }
}

int validate_batteries(char **battery_names, int battery_count) {
  unsigned int path_len =
      strlen(POWER_SUPPLY_SUBSYSTEM) + POWER_SUPPLY_ATTR_LENGTH;
  unsigned int name_len = 5;
  int return_value = -1;

  attr_path = realloc(attr_path, path_len + name_len);

  for (int i = 0; i < battery_count; i++) {
    if (strlen(battery_names[i]) > name_len) {
      name_len = strlen(battery_names[i]);
      attr_path = realloc(attr_path, path_len + name_len);
      if (attr_path == NULL)
        err(EXIT_FAILURE, "Memory allocation failed");
    }
    if (!is_battery(battery_names[i]) && return_value < 0) {
      return_value = i;
    }
  }
  return return_value;
}

void wait_for_update_battery_state(BatteryState *battery, bool required,
                                   struct timespec timeout) {
  char state[15];
  char *now_attribute;
  char *full_attribute;
  unsigned int tmp_now;
  unsigned int tmp_full;
  struct timespec ts;
  FILE *file;

  battery->discharging = false;
  battery->full = true;
  battery->energy_now = 0;
  battery->energy_full = 0;
  set_attributes(battery->names[0], &now_attribute, &full_attribute);

  // wait for changes on the state files or for timeout
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout.tv_sec;
  ts.tv_nsec += timeout.tv_nsec;
  pthread_mutex_lock(battery->state_change_mut);
  pthread_cond_timedwait(battery->bat_state_change,
                                       battery->state_change_mut, &ts);
  pthread_mutex_unlock(battery->state_change_mut);

  /* iterate through all batteries */
  for (int i = 0; i < battery->count; i++) {
    sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/status", battery->names[i]);
    file = fopen(attr_path, "r");
    if (file == NULL || fscanf(file, "%12s", state) == 0) {
      if (required)
        err(EXIT_FAILURE, "Could not read %s", attr_path);
      battery->discharging |= 0;
      if (file)
        fclose(file);
      continue;
    }
    fclose(file);

    battery->discharging |= strcmp(state, POWER_SUPPLY_DISCHARGING) == 0;
    battery->full &= strcmp(state, POWER_SUPPLY_FULL) == 0;

    sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/%s", battery->names[i],
            now_attribute);
    file = fopen(attr_path, "r");
    if (file == NULL || fscanf(file, "%u", &tmp_now) == 0) {
      if (required)
        err(EXIT_FAILURE, "Could not read %s", attr_path);
      if (file)
        fclose(file);
      continue;
    }
    fclose(file);

    if (full_attribute != NULL) {
      sprintf(attr_path, POWER_SUPPLY_SUBSYSTEM "/%s/%s", battery->names[i],
              full_attribute);
      file = fopen(attr_path, "r");
      if (file == NULL || fscanf(file, "%u", &tmp_full) == 0) {
        if (required)
          err(EXIT_FAILURE, "Could not read %s", attr_path);
        if (file)
          fclose(file);
        continue;
      }
      fclose(file);
    } else {
      tmp_full = 100;
    }

    battery->energy_now += tmp_now;
    battery->energy_full += tmp_full;
  }

  battery->level = round(100.0 * battery->energy_now / battery->energy_full);
}
