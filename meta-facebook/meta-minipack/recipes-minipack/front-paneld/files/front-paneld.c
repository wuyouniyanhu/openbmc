/*
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <openbmc/kv.h>
#include <openbmc/ipmi.h>
#include <openbmc/ipmb.h>
#include <openbmc/pal.h>
#include <openbmc/obmc-i2c.h>
#include <openbmc/sdr.h>

#define TMP75_NAME    "tmp75"
#define ADM1278_NAME  "adm1278"

#define BTN_MAX_SAMPLES   200
#define BTN_POWER_OFF     40

#define ADM1278_ADDR 0x10
#define TMP75_1_ADDR 0x48
#define TMP75_2_ADDR 0x4b

#define INTERVAL_MAX  5

static uint8_t m_pos = 0xff;

static void
pim_device_detect_log(uint8_t num, uint8_t bus, uint8_t addr,
                      char* device, int state) {
  switch (state) {
    case I2C_BUS_ERROR:
    case I2C_FUNC_ERROR:
      syslog(LOG_CRIT, "PIM %d Bus:%d get fail", num, bus);
      break;
    case I2C_DEVICE_ERROR:
      syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s get fail",
             num, bus, addr, device);
      break;
    case I2c_DRIVER_EXIST:
      syslog(LOG_WARNING, "PIM %d Bus:%d Addr:0x%x Device:%s driver exist",
             num, bus, addr, device);
      break;
  }
}

static void
pim_driver_add(uint8_t num) {
  int state;
  uint8_t bus = ((num - 1) * 8) + 80;

  state = pal_detect_i2c_device(bus+2, TMP75_1_ADDR, MODE_AUTO, 0);
  if (state) {
    pim_device_detect_log(num, bus+2, TMP75_1_ADDR, TMP75_NAME, state);
  } else {
    if (pal_add_i2c_device(bus+2, TMP75_1_ADDR, TMP75_NAME)) {
      syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                       "driver cannot add.",
                       num, bus+2, TMP75_1_ADDR, TMP75_NAME);
    }
  }
  msleep(50);

  state = pal_detect_i2c_device(bus+3, TMP75_2_ADDR, MODE_AUTO, 0);
  if (state) {
    pim_device_detect_log(num, bus+3, TMP75_2_ADDR, TMP75_NAME, state);
  } else {
    if (pal_add_i2c_device(bus+3, TMP75_2_ADDR, TMP75_NAME)) {
      syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                       "driver cannot add.",
                       num, bus+3, TMP75_2_ADDR, TMP75_NAME);
    }
  }
  msleep(50);

  state = pal_detect_i2c_device(bus+4, ADM1278_ADDR, MODE_AUTO, 0);
  if (state) {
    pim_device_detect_log(num, bus+4, ADM1278_ADDR, ADM1278_NAME, state);
  } else {
    if (pal_add_i2c_device(bus+4, ADM1278_ADDR, ADM1278_NAME)) {
      syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                       "driver cannot add.",
                       num, bus+4, ADM1278_ADDR, ADM1278_NAME);
    }
  }
}

static void
pim_driver_del(uint8_t num) {
  uint8_t bus = ((num - 1) * 8) + 80;

  if (pal_del_i2c_device(bus+2, TMP75_1_ADDR)) {
    syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                      "driver cannot delete.",
                      num, bus+2, TMP75_1_ADDR, TMP75_NAME);
  }

  if (pal_del_i2c_device(bus+3, TMP75_2_ADDR)) {
    syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                      "driver cannot delete.",
                      num, bus+3, TMP75_2_ADDR, TMP75_NAME);
  }

  if (pal_del_i2c_device(bus+4, ADM1278_ADDR)) {
    syslog(LOG_CRIT, "PIM %d Bus:%d Addr:0x%x Device:%s "
                      "driver cannot delete.",
                      num, bus+4, ADM1278_ADDR, ADM1278_NAME);
  }
}

static int
pim_thresh_init_file_check(uint8_t fru) {
  char fru_name[32];
  char fpath[64];

  pal_get_fru_name(fru, fru_name);
  snprintf(fpath, 64, INIT_THRESHOLD_BIN, fru_name);

  return access(INIT_THRESHOLD_BIN, F_OK);
}

int
pim_thresh_init(uint8_t fru) {
  int i, sensor_cnt;
  uint8_t snr_num;
  uint8_t *sensor_list;
  thresh_sensor_t snr[MAX_NUM_FRUS][MAX_SENSOR_NUM] = {0};

  pal_get_fru_sensor_list(fru, &sensor_list, &sensor_cnt);
  for (i = 0; i < sensor_cnt; i++) {
    snr_num = sensor_list[i];
    if (sdr_get_snr_thresh(fru, snr_num, &snr[fru - 1][snr_num]) < 0) {
#ifdef DEBUG
      syslog(LOG_WARNING, "pim_thresh_init: sdr_get_snr_thresh for FRU: %d",
             fru);
#endif
      continue;
    }
  }

  if (access(THRESHOLD_PATH, F_OK) == -1) {
    mkdir(THRESHOLD_PATH, 0777);
  }

  if (pal_copy_all_thresh_to_file(fru, snr[fru - 1]) < 0) {
    syslog(LOG_WARNING, "%s: Fail to copy thresh to file for FRU: %d",
           __func__, fru);
    return -1;
  }

  return 0;
}

static int
get_handsw_pos(uint8_t *pos) {
  if (m_pos > HAND_SW_BMC)
    return -1;

  *pos = m_pos;
  return 0;
}

// Thread for monitoring scm plug
static void *
scm_monitor_handler(void *unused) {
  int curr = -1;
  int prev = -1;
  int ret;
  uint8_t prsnt = 0;
  uint8_t power;

  while (1) {
    ret = pal_is_fru_prsnt(FRU_SCM, &prsnt);
    if (ret) {
      goto scm_mon_out;
    }
    curr = prsnt;
    if (curr != prev) {
      if (curr) {
        // SCM was inserted
        syslog(LOG_WARNING, "SCM Insertion\n");

        /* Setup TH3 PCI-e repeater */
        run_command("/usr/local/bin/setup_pcie_repeater.sh th3 write");
        ret = pal_get_server_power(FRU_SCM, &power);
        if (ret) {
          goto scm_mon_out;
        }
        if (power == SERVER_POWER_OFF) {
          sleep(3);
          syslog(LOG_WARNING, "SCM power on\n");
          pal_set_server_power(FRU_SCM, SERVER_POWER_ON);
          /* Setup management port LED */
          run_command("/usr/local/bin/setup_mgmt.sh led");
          goto scm_mon_out;
        }
      } else {
        // SCM was removed
        syslog(LOG_WARNING, "SCM Extraction\n");
      }
    }
scm_mon_out:
    prev = curr;
    sleep(1);
  }
  return 0;
}

// Thread for monitoring pim plug
static void *
pim_monitor_handler(void *unused) {
  uint8_t fru;
  uint8_t num;
  uint8_t ret;
  uint8_t prsnt = 0;
  uint8_t prsnt_ori = 0;
  uint8_t curr_state = 0x00;
  uint8_t pim_type;
  uint8_t pim_type_old[10] = {PIM_TYPE_UNPLUG};
  uint8_t interval[10];
  bool thresh_first = true;

  memset(interval, INTERVAL_MAX, sizeof(interval));
  while (1) {
    for (fru = FRU_PIM1; fru <= FRU_PIM8; fru++) {
      ret = pal_is_fru_prsnt(fru, &prsnt);
      if (ret) {
        goto pim_mon_out;
      }
      /* FRU_PIM1 = 3, FRU_PIM2 = 4, ...., FRU_PIM8 = 10 */
      /* Get original prsnt state PIM1 @bit0, PIM2 @bit1, ..., PIM8 @bit7 */
      num = fru - 2;
      prsnt_ori = GETBIT(curr_state, (num - 1));
      /* 1 is prsnt, 0 is not prsnt. */
      if (prsnt != prsnt_ori) {
        if (prsnt) {
          syslog(LOG_WARNING, "PIM %d is plugged in.", num);
          pim_driver_add(num);
          pim_type = pal_get_pim_type(fru);
          if (pim_type != pim_type_old[num]) {
            if (pim_type == PIM_TYPE_16Q) {
              if (!pal_set_pim_type_to_file(fru, "16q")) {
                syslog(LOG_INFO, "PIM %d type is 16Q", num);
                pim_type_old[num] = PIM_TYPE_16Q;
              } else {
                syslog(LOG_WARNING,
                       "pal_set_pim_type_to_file: PIM %d set 16Q failed", num);
              }
            } else if (pim_type == PIM_TYPE_4DD) {
              if (!pal_set_pim_type_to_file(fru, "4dd")) {
                syslog(LOG_INFO, "PIM %d type is 4DD", num);
                pim_type_old[num] = PIM_TYPE_4DD;
              } else {
                syslog(LOG_WARNING,
                       "pal_set_pim_type_to_file: PIM %d set 4DD failed", num);
              }
            } else {
              if (!pal_set_pim_type_to_file(fru, "none")) {
                syslog(LOG_CRIT,
                        "PIM %d type cannot detect, DOMFPGA get fail", num);
                pim_type_old[num] = PIM_TYPE_NONE;
              } else {
                syslog(LOG_WARNING,
                      "pal_set_pim_type_to_file: PIM %d set none failed", num);
              }
            }
            if (thresh_first == true) {
              thresh_first = false;
            } else {
              if (pim_thresh_init_file_check(fru)) {
                pim_thresh_init(fru);
              } else {
                pal_set_pim_thresh(fru);
              }
            }
          }
        } else {
          syslog(LOG_WARNING, "PIM %d is unplugged.", num);
          pim_type_old[num] = PIM_TYPE_UNPLUG;
          pal_clear_thresh_value(fru);
          pim_driver_del(num);
          if (pal_set_pim_type_to_file(fru, "unplug")) {
            syslog(LOG_WARNING,
                    "pal_set_pim_type_to_file: PIM %d set unplug failed", num);
          }
        }
        /* Set PIM1 prsnt state @bit0, PIM2 @bit1, ..., PIM8 @bit7 */
        curr_state = prsnt ? SETBIT(curr_state, (num - 1))
                           : CLEARBIT(curr_state, (num - 1));
      }
      /* 1 is prsnt, 0 is not prsnt. */
      if (prsnt) {
        if (interval[num] == 0) {
          interval[num] = INTERVAL_MAX;
          pal_set_pim_sts_led(fru);
        } else {
          interval[num]--;
        }
      }
    }
pim_mon_out:
    sleep(1);
  }
  return 0;
}

void
exithandler(int signum) {
  int brd_rev;
  pal_get_board_rev(&brd_rev);
  set_sled(brd_rev, SLED_CLR_YELLOW, SLED_SMB);
  exit(0);
}

// Thread for monitoring sim LED
static void *
simLED_monitor_handler(void *unused) {
  int brd_rev;
  uint8_t sys_ug = 0, fan_ug = 0, psu_ug = 0, smb_ug = 0;
  uint8_t interval[4];

  memset(interval, INTERVAL_MAX, sizeof(interval));
  pal_get_board_rev(&brd_rev);
  init_led();
  while(1) {
    sleep(1);
    pal_mon_fw_upgrade(brd_rev, &sys_ug, &fan_ug, &psu_ug, &smb_ug);
    if (sys_ug == 0) {
      if (interval[0] == 0) {
        interval[0] = INTERVAL_MAX;
        set_sys_led(brd_rev);
      } else {
        interval[0]--;
      }
    }
    if (fan_ug == 0) {
      if (interval[1] == 0) {
        interval[1] = INTERVAL_MAX;
        set_fan_led(brd_rev);
      } else {
        interval[1]--;
      }
    }
    if (psu_ug == 0) {
      if (interval[2] == 0) {
        interval[2] = INTERVAL_MAX;
        set_psu_led(brd_rev);
      } else {
        interval[2]--;
      }
    }
    if (smb_ug == 0) {
      if (interval[3] == 0) {
        interval[3] = INTERVAL_MAX;
        set_smb_led(brd_rev);
      } else {
        interval[3]--;
      }
    }
  }
  return 0;
}

// Thread for monitoring debug card hotswap
static void *
debug_card_handler(void *unused) {
  int curr = -1;
  int prev = -1;
  int ret;
  uint8_t prsnt = 0;
  uint8_t prev_phy_pos = 0xff, pos, btn;
  char str[8];
 
  /* Clear Debug Card uart sel button at the first time */
  pal_clr_dbg_uart_btn();

   while (1) {
    ret = pal_get_hand_sw_physically(&pos);
    if (ret) {
      goto debug_card_out;
    }

    if (pos == prev_phy_pos) {
      goto get_hand_sw_cache;
    }

    msleep(10);
    ret = pal_get_hand_sw_physically(&pos);
    if (ret) {
      goto debug_card_out;
    }

    prev_phy_pos = pos;
    snprintf(str, sizeof(str), "%u", pos);
    ret = kv_set("scm_hand_sw", str, 0, 0);
    if (ret) {
      goto debug_card_out;
    }

get_hand_sw_cache:
    ret = pal_get_hand_sw(&pos);
    if (ret) {
      goto debug_card_out;
    }
    m_pos = pos;

    // Check if debug card present or not
    ret = pal_is_debug_card_prsnt(&prsnt);
    if (ret) {
      goto debug_card_out;
    }
    curr = prsnt;

    // Check if Debug Card was either inserted or removed
    if (curr != prev) {
      if (!curr) {
        // Debug Card was removed
        syslog(LOG_WARNING, "Debug Card Extraction\n");
      } else {
        // Debug Card was inserted
        syslog(LOG_WARNING, "Debug Card Insertion\n");
      }
    }
    prev = curr;

    ret = pal_get_dbg_uart_btn(&btn);
    if (ret) {
      goto debug_card_out;
    }

    if (btn) {
      syslog(LOG_CRIT, "UART Sel Button pressed");
      /* Toggle the UART position */
      pal_switch_uart_mux(!pos);
      /* Clear Debug Card uart sel button */
      pal_clr_dbg_uart_btn();
    }
debug_card_out:
    if (curr == 1)
      msleep(500);
    else
      sleep(1);
  }

  return 0;
}

/* Thread to handle Power Button to power on/off userver */
static void *
pwr_btn_handler(void *unused) {
  int ret, i;
  uint8_t pos, btn;
  uint8_t cmd = 0;
  uint8_t power;
  bool release_flag = true;

  while (1) {
    /* Check the position of hand switch */
    ret = get_handsw_pos(&pos);
    if (ret || pos == HAND_SW_BMC) {
      sleep(1);
      continue;
    }

    /* Check if Debug Card power button is pressed */
    ret = pal_get_dbg_pwr_btn(&btn);
    if (ret || !btn) {
      if (false == release_flag) {
        release_flag = true;
      }
      goto pwr_btn_out;
    }

    if (false == release_flag) {
      goto pwr_btn_out;
    }
    release_flag = false;
    syslog(LOG_WARNING, "Power Button Pressed");

    /* Wait for the button to be released */
    for (i = 0; i < BTN_POWER_OFF; i++) {
      ret = pal_get_dbg_pwr_btn(&btn);
      if (ret || btn ) {
        msleep(100);
        continue;
      }
      release_flag = true;
      syslog(LOG_WARNING, "Power Button Released");
      break;
    }

    /* Get the current power state (on or off) */
    if (pos == HAND_SW_SERVER) {
      ret = pal_get_server_power(FRU_SCM, &power);
      if (ret) {
        goto pwr_btn_out;
      }
      /* Set power command should reverse of current power state */
      cmd = !power;
    }

    /* To determine long button press */
    if (i >= BTN_POWER_OFF) {
      /* if long press (>4s) and hand-switch position == bmc, then initiate
         sled-cycle */
      if (pos == HAND_SW_BMC) {
        /* minipack do nothing here. */
      } else {
        pal_update_ts_sled();
        syslog(LOG_CRIT, "Power Button Long Press");
      }
    } else {
      /* Current state is ON and it is not a long press,
         graceful shutdown userver */
      if (power == SERVER_POWER_ON) {
        cmd = SERVER_GRACEFUL_SHUTDOWN;
      }
      pal_update_ts_sled();
      syslog(LOG_CRIT, "Power Button Press");
    }

    if (pos == HAND_SW_SERVER) {
      if (cmd == SERVER_POWER_ON) {
        pal_set_restart_cause(pos, RESTART_CAUSE_PWR_ON_PUSH_BUTTON);
      }
      ret = pal_set_server_power(FRU_SCM, cmd);
      if (!ret) {
        if (cmd == SERVER_POWER_ON) {
          syslog(LOG_CRIT, "Successfully power on micro-server");
        } else if (cmd == SERVER_POWER_OFF) {
          syslog(LOG_CRIT, "Successfully power off micro-server");
        } else if (cmd == SERVER_GRACEFUL_SHUTDOWN) {
          syslog(LOG_CRIT, "Successfully graceful shutdown micro-server");
        }
      }
    }
pwr_btn_out:
    msleep(100);
  }

  return 0;
}

/* Thread to handle Reset Button to reset userver */
static void *
rst_btn_handler(void *unused) {
  int ret;
  uint8_t pos, btn;

  /* Clear Debug Card reset button at the first time */
  pal_clr_dbg_rst_btn();

  while (1) {
    /* Check the position of hand switch
     * and if reset button is pressed */
    if (get_handsw_pos(&pos) || pal_get_dbg_rst_btn(&btn)) {
      goto rst_btn_out;
    }

    if (pos == HAND_SW_BMC && btn) {
      pal_clr_dbg_rst_btn();
    } else if (pos == HAND_SW_SERVER && btn) {
      syslog(LOG_CRIT, "Reset Button pressed");
      pal_update_ts_sled();
      /* Reset userver */
      ret = pal_set_server_power(FRU_SCM, SERVER_POWER_RESET);
      if (!ret) {
        syslog(LOG_CRIT, "Successfully power reset micro-server");
      }
      /* Clear Debug Card reset button status */
      pal_clr_dbg_rst_btn();
    }
rst_btn_out:
    msleep(500);
  }

  return 0;
}

int
main (int argc, char * const argv[]) {
  pthread_t tid_scm_monitor;
  pthread_t tid_pim_monitor;
  pthread_t tid_debug_card;
  pthread_t tid_simLED_monitor;
  pthread_t tid_pwr_btn;
  pthread_t tid_rst_btn;
  int rc;
  int pid_file;
  int brd_rev;

  signal(SIGTERM, exithandler);
  pid_file = open("/var/run/front-paneld.pid", O_CREAT | O_RDWR, 0666);
  rc = flock(pid_file, LOCK_EX | LOCK_NB);
  if (rc) {
    if (EWOULDBLOCK == errno) {
      printf("Another front-paneld instance is running...\n");
      exit(-1);
    }
  } else {
    openlog("front-paneld", LOG_CONS, LOG_DAEMON);
  }

  if (pal_get_board_rev(&brd_rev)) {
    syslog(LOG_WARNING, "Get board revision failed\n");
    exit(-1);
  }

  if (pthread_create(&tid_scm_monitor, NULL, scm_monitor_handler, NULL) != 0) {
    syslog(LOG_WARNING, "pthread_create for scm monitor error\n");
    exit(1);
  }
  
  if (pthread_create(&tid_pim_monitor, NULL, pim_monitor_handler, NULL) != 0) {
    syslog(LOG_WARNING, "pthread_create for pim monitor error\n");
    exit(1);
  }
  
  if (pthread_create(&tid_simLED_monitor, NULL, simLED_monitor_handler, NULL) 
	  != 0) {
    syslog(LOG_WARNING, "pthread_create for simLED monitor error\n");
    exit(1);
  }

  if (brd_rev != BOARD_REV_EVTA) {
    if (pthread_create(&tid_debug_card, NULL, debug_card_handler, NULL) != 0) {
      syslog(LOG_WARNING, "pthread_create for debug card error\n");
      exit(1);
    }

    if (pthread_create(&tid_pwr_btn, NULL, pwr_btn_handler, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for power button error\n");
      exit(1);
    }

    if (pthread_create(&tid_rst_btn, NULL, rst_btn_handler, NULL) < 0) {
      syslog(LOG_WARNING, "pthread_create for reset button error\n");
      exit(1);
    }
    pthread_join(tid_debug_card, NULL);
    pthread_join(tid_pwr_btn, NULL);
    pthread_join(tid_rst_btn, NULL);
  }

  pthread_join(tid_scm_monitor, NULL);
  pthread_join(tid_pim_monitor, NULL);
  pthread_join(tid_simLED_monitor, NULL);

  return 0;
}
