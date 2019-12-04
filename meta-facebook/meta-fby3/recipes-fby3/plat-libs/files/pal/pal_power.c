#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/mman.h>
#include <string.h>
#include <ctype.h>
#include <openbmc/kv.h>
#include <openbmc/libgpio.h>
#include <openbmc/obmc-i2c.h>
#include "pal.h"

#define CPLD_PWR_CTRL_BUS "/dev/i2c-12"
#define CPLD_PWR_CTRL_ADDR 0x1F

enum {
  POWER_STATUS_ALREADY_OK = 1,
  POWER_STATUS_OK = 0,
  POWER_STATUS_ERR = -1,
  POWER_STATUS_FRU_ERR = -2,
};

bool
is_server_off(void) {
  return POWER_STATUS_OK;
}

// Power Off the server
static int
server_power_off(uint8_t fru) {
  return bic_server_power_off(fru);
}

// Power On the server
static int
server_power_on(uint8_t fru) {
  return bic_server_power_on(fru);
}

static int
server_power_12v_on(uint8_t fru) {
  int i2cfd;
  char cmd[64] = {0};
  uint8_t tbuf[2] = {0};
  uint8_t rbuf[2] = {0};
  uint8_t tlen = 0;
  uint8_t rlen = 0;
  int ret = 0;

  i2cfd = open(CPLD_PWR_CTRL_BUS, O_RDWR);
  if ( i2cfd < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to open %s", __func__, CPLD_PWR_CTRL_BUS);
    goto error_exit;
  }

  tbuf[0] = 0x0;
  tlen = 1;
  rlen = 1;
  ret = i2c_rdwr_msg_transfer(i2cfd, CPLD_PWR_CTRL_ADDR, tbuf, tlen, rbuf, rlen);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to do i2c_rdwr_msg_transfer, tlen=%d", __func__, tlen);
    goto error_exit;
  }

  msleep(100);

  tbuf[0] = 0x0;
  tbuf[1] = (rbuf[0] | (0x1 << fru));
  tlen = 2;
  ret = i2c_rdwr_msg_transfer(i2cfd, CPLD_PWR_CTRL_ADDR, tbuf, tlen, NULL, 0);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to do i2c_rdwr_msg_transfer, tlen=%d", __func__, tlen);
    goto error_exit;
  }

  sleep(1);

  snprintf(cmd, sizeof(cmd), "sv start ipmbd_%d > /dev/null 2>&1", fru); 
  system(cmd);

  sleep(2);
  
  ret = server_power_on(fru);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to do server_power_on fru%d", __func__, fru);
  }

  //TODO: need to change the status of FM_BMC_SLOT${num}_ISOLATED_EN

error_exit:
  if ( i2cfd > 0 ) close(i2cfd);

  return ret; 
}

static int
server_power_12v_off(uint8_t fru) {
  int i2cfd;
  char cmd[64] = {0};
  uint8_t tbuf[2] = {0};
  uint8_t rbuf[2] = {0};
  uint8_t tlen = 0;
  uint8_t rlen = 0;
  int ret = 0;

  i2cfd = open(CPLD_PWR_CTRL_BUS, O_RDWR);
  if ( i2cfd < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to open %s", __func__, CPLD_PWR_CTRL_BUS);
    goto error_exit;
  }

  tbuf[0] = 0x0;
  tlen = 1;
  rlen = 1;
  ret = i2c_rdwr_msg_transfer(i2cfd, CPLD_PWR_CTRL_ADDR, tbuf, tlen, rbuf, rlen);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to do i2c_rdwr_msg_transfer fails, tlen=%d", __func__, tlen);
    goto error_exit;
  }

  msleep(100);

  tbuf[0] = 0x0;
  tbuf[1] = (rbuf[0] &~(0x1 << fru));
  tlen = 2;
  ret = i2c_rdwr_msg_transfer(i2cfd, CPLD_PWR_CTRL_ADDR, tbuf, tlen, NULL, 0);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Failed to do i2c_rdwr_msg_transfer fails, tlen=%d", __func__, tlen);
    goto error_exit;
  }

  snprintf(cmd, sizeof(cmd), "sv stop ipmbd_%d > /dev/null 2>&1", fru);
  system(cmd);

  //TODO: need to change the status of FM_BMC_SLOT${num}_ISOLATED_EN

error_exit:
  if ( i2cfd > 0 ) close(i2cfd);

  return ret;
}

int
pal_get_server_12v_power(uint8_t slot_id, uint8_t *status) {
  //TODO: need to check the status with a gpio
  *status = 1;
  return POWER_STATUS_OK;
}

int
pal_get_server_power(uint8_t fru, uint8_t *status) {
  int ret;

  ret = is_valid_slot_id(fru);
  if ( ret < 0 ) {
    return POWER_STATUS_FRU_ERR;
  }

  ret = bic_get_server_power_status(fru, status);
  if ( ret < 0 ) {
    //TODO: if bic_get_server_power_status is failed, we should check the 12V status of the system
    //Now we suppose it is 12V-off
    *status = SERVER_12V_OFF;
    ret = POWER_STATUS_OK;
    //syslog(LOG_ERR, "%s: bic_get_server_power_status fails on slot%d\n", __func__, fru);
    //return POWER_STATUS_ERR;
  }

  return ret;
}

// Power Off, Power On, or Power Reset the server in given slot
int
pal_set_server_power(uint8_t fru, uint8_t cmd) {
  uint8_t status;
  int ret = 0;

  ret = is_valid_slot_id(fru);
  if ( ret < 0 ) {
    return POWER_STATUS_FRU_ERR;
  }

  switch (cmd) {
    case SERVER_12V_OFF:
    case SERVER_12V_ON:
    case SERVER_12V_CYCLE:
      //do nothing
      break;
    default:
      if (pal_get_server_power(fru, &status) < 0) {
        return POWER_STATUS_ERR;
      }
      break;
  }

  switch(cmd) {
    case SERVER_POWER_ON:
      return (status == SERVER_POWER_ON)?POWER_STATUS_ALREADY_OK:server_power_on(fru);
      break;

    case SERVER_POWER_OFF:
      return (status == SERVER_POWER_OFF)?POWER_STATUS_ALREADY_OK:server_power_off(fru);
      break;

    case SERVER_POWER_CYCLE:
      if (status == SERVER_POWER_ON) {
        return bic_server_power_cycle(fru);
      } else if (status == SERVER_POWER_OFF) {
        return server_power_on(fru);
      }
      break;

    case SERVER_GRACEFUL_SHUTDOWN:
      return (status == SERVER_POWER_OFF)?POWER_STATUS_ALREADY_OK:bic_server_graceful_power_off(fru);
      break;

    case SERVER_POWER_RESET:
      return bic_server_power_reset(fru);
      break;

    case SERVER_12V_ON:
      if ( pal_get_server_12v_power(fru, &status) < 0 ) {
        return POWER_STATUS_ERR;
      } 
      //return (status == SERVER_POWER_ON)?POWER_STATUS_ALREADY_OK:server_power_12v_on(fru);  
      return server_power_12v_on(fru);
      break;

    case SERVER_12V_OFF:
      if ( pal_get_server_12v_power(fru, &status) < 0 ) {
        return POWER_STATUS_ERR;
      }
      //return (status == SERVER_POWER_OFF)?POWER_STATUS_ALREADY_OK:server_power_12v_off(fru);
      return server_power_12v_off(fru);
      break;

    case SERVER_12V_CYCLE:
      break;

    default:
      syslog(LOG_WARNING, "%s() wrong cmd: 0x%02X", __func__, cmd);
      return POWER_STATUS_ERR;
  }

  return POWER_STATUS_OK;
}

int
pal_sled_cycle(void) {
  int ret;
  uint8_t bmc_location = 0;

  ret = get_bmc_location(&bmc_location);
  if ( ret < 0 ) {
    syslog(LOG_WARNING, "%s() Cannot get the location of BMC", __func__);
    return POWER_STATUS_ERR;
  }

  if ( bmc_location == BB_BMC ) {
    ret = system("i2cset -y 11 0x40 0xd9 c &> /dev/null");
  } else {
    //TODO: The power control of class2
  }

  return ret;
}

int
pal_set_last_pwr_state(uint8_t fru, char *state) {
  return 0;
}

int
pal_get_last_pwr_state(uint8_t fru, char *state) {
  return 0;
}
